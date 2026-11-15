"""Project-local AST extraction; libclang cursors never escape their translation unit."""
import os
from functools import lru_cache
import clang_bridge

LANGUAGE = "c++"
ID_PREFIX = "c++:"
TU_KIND = 350
RECORD_KINDS = {"ClassDecl", "StructDecl", "UnionDecl", "EnumDecl", "EnumConstantDecl", "FieldDecl", "FunctionDecl", "CXXMethod", "Constructor", "Destructor", "Namespace", "TypedefDecl", "TypeAliasDecl", "VarDecl", "ParmDecl", "FunctionTemplate", "ClassTemplate"}
FUNCTION_LIKE_KINDS = {"FunctionDecl", "CXXMethod", "Constructor", "Destructor", "FunctionTemplate"}
CALLABLE_KINDS = FUNCTION_LIKE_KINDS

def norm_slashes(path):
    return path.replace("\\", "/")

@lru_cache(maxsize=65536)
def is_in_project(path, root):
    if not path:
        return False
    try:
        return os.path.normcase(os.path.commonpath([os.path.realpath(path), root])) == os.path.normcase(root)
    except ValueError:
        return False

def to_project_rel(path, root):
    return os.path.relpath(path, root).replace("\\", "/")

def _merge_symbol(existing: dict, incoming: dict) -> dict:
    merged = dict(existing)
    for key in ("usr", "language", "kind", "name", "qualified_name", "signature", "parent_id", "access"):
        if not merged.get(key) and incoming.get(key):
            merged[key] = incoming[key]
    # 声明与定义位置分别保留；定义出现时优先写入 def_*。
    if not merged.get("decl_file") and incoming.get("decl_file"):
        merged["decl_file"] = incoming["decl_file"]
        merged["decl_line"] = incoming["decl_line"]
        merged["decl_col"] = incoming["decl_col"]
    if incoming.get("is_definition"):
        merged["is_definition"] = True
        merged["def_file"] = incoming.get("def_file") or incoming.get("decl_file")
        merged["def_line"] = incoming.get("def_line") or incoming.get("decl_line")
        merged["def_col"] = incoming.get("def_col") or incoming.get("decl_col")
    elif not merged.get("def_file") and incoming.get("def_file"):
        merged["def_file"] = incoming["def_file"]
        merged["def_line"] = incoming["def_line"]
        merged["def_col"] = incoming["def_col"]
    merged["is_external"] = bool(merged.get("is_external")) and bool(incoming.get("is_external"))
    return merged


# ---------------------------------------------------------------------------
# AST 抽取
# ---------------------------------------------------------------------------

def _access_name(lib, cursor) -> str | None:
    level = lib.lib.clang_getCXXAccessSpecifier(cursor)
    if level == clang_bridge.ACCESS_PUBLIC:
        return "public"
    if level == clang_bridge.ACCESS_PROTECTED:
        return "protected"
    if level == clang_bridge.ACCESS_PRIVATE:
        return "private"
    return None


def _qualified_name(lib, cursor) -> str:
    parts: list[str] = []
    cur = cursor
    seen = 0
    while seen < 64:
        kind = lib.lib.clang_getCursorKind(cur)
        if kind == TU_KIND:
            break
        sp = lib.spelling(cur)
        if sp:
            parts.append(sp)
        parent = lib.lib.clang_getCursorSemanticParent(cur)
        if lib.lib.clang_equalCursors(parent, cur) or lib.lib.clang_getCursorKind(parent) == TU_KIND:
            # 顶层实体的语义父级可能是 TU 或自身
            if lib.lib.clang_getCursorKind(parent) == TU_KIND:
                break
            break
        cur = parent
        seen += 1
    return "::".join(reversed(parts))


def _signature(lib, cursor, kind: str) -> str | None:
    if kind in {"VarDecl", "ParmDecl", "FieldDecl"}:
        return lib.type_spelling(cursor)
    if kind in FUNCTION_LIKE_KINDS | {"FunctionTemplate"}:
        disp = lib.display_name(cursor)
        if disp and "(" in disp and disp != lib.spelling(cursor):
            return disp
        return lib.type_spelling(cursor)
    return None


def _location(lib, cursor, *, expansion: bool = False):
    path, line, col = lib.location(cursor, expansion=expansion)
    return path, line, col


class TuExtractor:
    def __init__(self, lib, root: str) -> None:
        self.lib = lib
        self.root = root
        self.symbols: dict[str, dict] = {}
        self.relations: list[dict] = []

    def extract(self, tu) -> tuple[list[dict], list[dict]]:
        root_cursor = self.lib.lib.clang_getTranslationUnitCursor(tu)
        self._walk(root_cursor, [])
        return list(self.symbols.values()), self.relations

    def _symbol_id(self, cursor, usr: str) -> str:
        if usr:
            return ID_PREFIX + usr
        kind = self.lib.kind_name(cursor.kind)
        path, line, col = self._loc(cursor)
        return "%sloc:%s:%d:%d:%s" % (ID_PREFIX, path, line, col, kind)

    def _loc(self, cursor, *, expansion: bool = False):
        return _location(self.lib, cursor, expansion=expansion)

    def _record_symbol(self, cursor, kind: str, context: list[str]) -> str | None:
        usr = self.lib.usr(cursor)
        if not usr:
            # 仅对匿名实体生成位置 ID；多数实体都有 USR。
            pass
        sid = self._symbol_id(cursor, usr)
        path, line, col = self._loc(cursor)
        internal = is_in_project(path, self.root)
        rel_or_abs = to_project_rel(path, self.root) if internal else norm_slashes(path)
        sym = {
            "id": sid, "usr": usr, "language": LANGUAGE, "kind": kind,
            "name": self.lib.spelling(cursor),
            "qualified_name": _qualified_name(self.lib, cursor),
            "signature": _signature(self.lib, cursor, kind),
            "parent_id": self._parent_id(cursor),
            "access": _access_name(self.lib, cursor),
            "decl_file": rel_or_abs, "decl_line": line, "decl_col": col,
            "def_file": None, "def_line": None, "def_col": None,
            "is_definition": bool(self.lib.lib.clang_isCursorDefinition(cursor)),
            "is_external": not internal,
        }
        if sym["is_definition"]:
            sym["def_file"] = rel_or_abs
            sym["def_line"] = line
            sym["def_col"] = col
        else:
            dpath, dline, dcol = self._loc(self.lib.lib.clang_getCursorDefinition(cursor))
            if dpath:
                dinternal = is_in_project(dpath, self.root)
                sym["def_file"] = to_project_rel(dpath, self.root) if dinternal else norm_slashes(dpath)
                sym["def_line"] = dline
                sym["def_col"] = dcol
                if dinternal:
                    sym["is_external"] = False
        if sid in self.symbols:
            self.symbols[sid] = _merge_symbol(self.symbols[sid], sym)
        else:
            self.symbols[sid] = sym
        return sid

    def _parent_id(self, cursor) -> str | None:
        parent = self.lib.lib.clang_getCursorSemanticParent(cursor)
        if self.lib.lib.clang_getCursorKind(parent) == TU_KIND:
            return None
        pusr = self.lib.usr(parent)
        if not pusr:
            return None
        return ID_PREFIX + pusr

    def _relation_source(self, context: list[str]) -> str | None:
        for sid in reversed(context):
            if self.symbols.get(sid, {}).get("kind") in FUNCTION_LIKE_KINDS:
                return sid
        if context:
            return context[-1]
        return None

    def _add_relation(self, source_id, target_id, rel_type, cursor, confidence="definite") -> None:
        if not source_id or not target_id:
            return
        path, line, col = self._loc(cursor, expansion=True)
        internal = is_in_project(path, self.root)
        rel_or_abs = to_project_rel(path, self.root) if internal else norm_slashes(path)
        self.relations.append({
            "source_id": source_id, "target_id": target_id, "type": rel_type,
            "confidence": confidence, "file": rel_or_abs, "line": line, "col": col,
        })

    def _ensure_external_symbol(self, cursor, kind: str) -> str | None:
        """关系终点可能在项目外；记录为外部符号以便关系端点有效。"""
        usr = self.lib.usr(cursor)
        sid = self._symbol_id(cursor, usr)
        if sid in self.symbols:
            return sid
        path, line, col = self._loc(cursor)
        internal = is_in_project(path, self.root)
        rel_or_abs = to_project_rel(path, self.root) if internal else norm_slashes(path)
        sym = {
            "id": sid, "usr": usr, "language": LANGUAGE, "kind": kind,
            "name": self.lib.spelling(cursor),
            "qualified_name": _qualified_name(self.lib, cursor),
            "signature": _signature(self.lib, cursor, kind),
            "parent_id": self._parent_id(cursor),
            "access": _access_name(self.lib, cursor),
            "decl_file": rel_or_abs, "decl_line": line, "decl_col": col,
            "def_file": None, "def_line": None, "def_col": None,
            "is_definition": bool(self.lib.lib.clang_isCursorDefinition(cursor)),
            "is_external": not internal,
        }
        if sym["is_definition"]:
            sym["def_file"] = rel_or_abs
            sym["def_line"] = line
            sym["def_col"] = col
        self.symbols[sid] = sym
        return sid

    def _walk(self, cursor, context: list[str]) -> None:
        lib = self.lib
        kind = lib.kind_name(cursor.kind)
        path, _, _ = self._loc(cursor, expansion=True)
        if path and not is_in_project(path, self.root):
            return
        if path and any(p in {'.vividcode', '.codegraph', '.work'} for p in to_project_rel(path, self.root).split('/')):
            return

        sid = None
        if kind in RECORD_KINDS:
            sid = self._record_symbol(cursor, kind, context)

        # 继承：CXX_BASE_SPECIFIER 的引用游标是基类。
        if kind in {"CXX_BASE_SPECIFIER", "C++ base class specifier"}:
            base = lib.lib.clang_getCursorReferenced(cursor)
            base_kind = lib.kind_name(lib.lib.clang_getCursorKind(base))
            if base_kind in ("ClassDecl", "StructDecl") and context:
                base_id = self._ensure_external_symbol(base, base_kind)
                self._add_relation(context[-1], base_id, "inherits", cursor)

        # 调用：CallExpr。虚函数用 possible_call，其余为直接调用。
        elif kind == "CallExpr":
            target = lib.lib.clang_getCursorReferenced(cursor)
            target_kind = lib.kind_name(lib.lib.clang_getCursorKind(target))
            if target_kind in CALLABLE_KINDS:
                target_id = self._ensure_external_symbol(target, target_kind)
                if target_kind == "CXXMethod" and lib.lib.clang_CXXMethod_isVirtual(target):
                    self._add_relation(self._relation_source(context), target_id, "possible_call", cursor, "possible")
                else:
                    self._add_relation(self._relation_source(context), target_id, "calls", cursor, "definite")

        # 成员引用：MemberRefExpr（字段）。暂不细分读写。
        elif kind == "MemberRefExpr":
            target = lib.lib.clang_getCursorReferenced(cursor)
            target_kind = lib.kind_name(lib.lib.clang_getCursorKind(target))
            if target_kind == "FieldDecl":
                target_id = self._ensure_external_symbol(target, target_kind)
                self._add_relation(self._relation_source(context), target_id, "member_ref", cursor)

        # 名称引用：DeclRefExpr（变量、枚举常量等；函数引用由 CallExpr 覆盖，避免重复）。
        elif kind == "DeclRefExpr":
            target = lib.lib.clang_getCursorReferenced(cursor)
            target_kind = lib.kind_name(lib.lib.clang_getCursorKind(target))
            if target_kind in ("VarDecl", "ParmDecl", "EnumConstantDecl", "FieldDecl"):
                target_id = self._ensure_external_symbol(target, target_kind)
                self._add_relation(self._relation_source(context), target_id, "references", cursor)

        # 虚函数覆写：派生方法 -> 基类虚方法（静态可确定目标）。
        if kind == "CXXMethod" and sid and lib.lib.clang_CXXMethod_isVirtual(cursor):
            for base in lib.overridden(cursor):
                base_kind = lib.kind_name(lib.lib.clang_getCursorKind(base))
                base_id = self._ensure_external_symbol(base, base_kind)
                self._add_relation(sid, base_id, "overrides", cursor, "static")

        new_context = context + [sid] if sid else context
        for child in lib.children(cursor):
            self._walk(child, new_context)

