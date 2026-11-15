"""vividcode 语义索引后端 —— 面向当前分析需求的 libclang ctypes 绑定。

本模块直接通过 ctypes 加载真实 libclang.dll，不依赖 PyPI 的 ``clang`` 包，
也不关闭任何兼容性检查：所有入口都直接调用 libclang 的真实导出符号，符号
缺失或签名不符会立即抛异常。加载到的库版本会通过 ``clang_getClangVersion``
读取并回传，供上层记录到解析器版本字段。

加载顺序：
1. 环境变量 ``VIVIDCODE_LIBCLANG`` 或 ``LIBCLANG_PATH``（DLL 文件或所在目录）。
2. PATH 中本机 clang 所在目录的 libclang.dll。

这个模块只用 CPython 标准库；Python 与 LLVM 由用户自行安装。
"""

from __future__ import annotations

import ctypes
import os
import shutil
from ctypes import (CFUNCTYPE, POINTER, Structure, byref, c_char_p, c_int,
                    c_uint, c_ulong, c_void_p)

# ---------------------------------------------------------------------------
# 类型定义（与 clang-c/Index.h 对齐；CXString / CXCursor 等按值传递）
# ---------------------------------------------------------------------------


class CXString(Structure):
    _fields_ = [("data", c_void_p), ("private_flags", c_uint)]


class CXUnsavedFile(Structure):
    _fields_ = [("Filename", c_char_p), ("Contents", c_char_p), ("Length", c_ulong)]


class CXCursor(Structure):
    _fields_ = [("kind", c_int), ("xdata", c_int), ("data", c_void_p * 3)]


class CXSourceLocation(Structure):
    _fields_ = [("ptr_data", c_void_p * 2), ("int_data", c_uint)]


class CXType(Structure):
    _fields_ = [("kind", c_int), ("data", c_void_p * 2)]


# 访问级别（Index.h: CX_CXXAccessSpecifier）
ACCESS_PUBLIC = 1
ACCESS_PROTECTED = 2
ACCESS_PRIVATE = 3

# 解析选项：KeepGoing 允许收集多个诊断后继续，我们随后按诊断判定 TU 成败。
CXTRANSLATIONUNIT_KEEP_GOING = 0x200

# 诊断级别
SEV_ERROR = 3

# 子节点访问返回值
CHILD_CONTINUE = 1


def _find_library() -> str:
    candidates = []
    env = os.environ.get("VIVIDCODE_LIBCLANG") or os.environ.get("LIBCLANG_PATH")
    if env:
        if os.path.isdir(env):
            env = os.path.join(env, 'libclang.dll')
        if not os.path.isfile(env):
            raise RuntimeError('指定的本机 libclang.dll 不存在，请修正分析环境：' + env)
        return env
    clang = shutil.which('clang.exe') or shutil.which('clang')
    if clang:
        candidates.append(os.path.join(os.path.dirname(clang), 'libclang.dll'))
    for candidate in candidates:
        if candidate and os.path.isfile(candidate):
            return candidate
    raise RuntimeError(
        "找不到本机 libclang.dll。请自行安装 LLVM，再设置 VIVIDCODE_LIBCLANG 或在 vividcode 的分析环境中指定路径。"
    )


class LibClang:
    def __init__(self) -> None:
        path = _find_library()
        # CDLL 使用 cdecl；Windows 版 libclang 导出为 cdecl，正确。
        self.lib = ctypes.CDLL(path)
        self.path = path
        self._bind()

    # -- 基础字符串/索引 ------------------------------------------------
    def _bind(self) -> None:
        lib = self.lib
        lib.clang_getClangVersion.restype = CXString
        lib.clang_getCString.argtypes = [CXString]
        lib.clang_getCString.restype = c_char_p
        lib.clang_disposeString.argtypes = [CXString]
        lib.clang_disposeString.restype = None

        lib.clang_createIndex.argtypes = [c_int, c_int]
        lib.clang_createIndex.restype = c_void_p
        lib.clang_disposeIndex.argtypes = [c_void_p]
        lib.clang_disposeIndex.restype = None

        lib.clang_parseTranslationUnit2.argtypes = [
            c_void_p, c_char_p, POINTER(c_char_p), c_int,
            POINTER(CXUnsavedFile), c_uint, c_uint, POINTER(c_void_p),
        ]
        lib.clang_parseTranslationUnit2.restype = c_int
        lib.clang_disposeTranslationUnit.argtypes = [c_void_p]
        lib.clang_disposeTranslationUnit.restype = None

        # -- 游标 ------------------------------------------------------
        lib.clang_getTranslationUnitCursor.argtypes = [c_void_p]
        lib.clang_getTranslationUnitCursor.restype = CXCursor
        lib.clang_getCursorKind.argtypes = [CXCursor]
        lib.clang_getCursorKind.restype = c_int
        lib.clang_getCursorKindSpelling.argtypes = [c_int]
        lib.clang_getCursorKindSpelling.restype = CXString
        lib.clang_getCursorSpelling.argtypes = [CXCursor]
        lib.clang_getCursorSpelling.restype = CXString
        lib.clang_getCursorDisplayName.argtypes = [CXCursor]
        lib.clang_getCursorDisplayName.restype = CXString
        lib.clang_getCursorUSR.argtypes = [CXCursor]
        lib.clang_getCursorUSR.restype = CXString
        lib.clang_getCursorLocation.argtypes = [CXCursor]
        lib.clang_getCursorLocation.restype = CXSourceLocation
        lib.clang_getCursorSemanticParent.argtypes = [CXCursor]
        lib.clang_getCursorSemanticParent.restype = CXCursor
        lib.clang_getCursorReferenced.argtypes = [CXCursor]
        lib.clang_getCursorReferenced.restype = CXCursor
        lib.clang_getCursorDefinition.argtypes = [CXCursor]
        lib.clang_getCursorDefinition.restype = CXCursor
        lib.clang_equalCursors.argtypes = [CXCursor, CXCursor]
        lib.clang_equalCursors.restype = c_uint
        lib.clang_isCursorDefinition.argtypes = [CXCursor]
        lib.clang_isCursorDefinition.restype = c_uint
        lib.clang_getCXXAccessSpecifier.argtypes = [CXCursor]
        lib.clang_getCXXAccessSpecifier.restype = c_int
        lib.clang_CXXMethod_isVirtual.argtypes = [CXCursor]
        lib.clang_CXXMethod_isVirtual.restype = c_uint
        lib.clang_getOverriddenCursors.argtypes = [CXCursor, POINTER(POINTER(CXCursor)), POINTER(c_uint)]
        lib.clang_getOverriddenCursors.restype = None
        lib.clang_disposeOverriddenCursors.argtypes = [POINTER(CXCursor)]
        lib.clang_disposeOverriddenCursors.restype = None

        # 子节点遍历
        self._visitor_type = CFUNCTYPE(c_int, CXCursor, CXCursor, c_void_p)
        lib.clang_visitChildren.argtypes = [CXCursor, self._visitor_type, c_void_p]
        lib.clang_visitChildren.restype = c_uint

        # -- 类型 ------------------------------------------------------
        lib.clang_getCursorType.argtypes = [CXCursor]
        lib.clang_getCursorType.restype = CXType
        lib.clang_getTypeSpelling.argtypes = [CXType]
        lib.clang_getTypeSpelling.restype = CXString

        # -- 位置与文件 ------------------------------------------------
        lib.clang_getExpansionLocation.argtypes = [
            CXSourceLocation, POINTER(c_void_p), POINTER(c_uint), POINTER(c_uint), POINTER(c_uint),
        ]
        lib.clang_getExpansionLocation.restype = None
        lib.clang_getFileLocation.argtypes = [
            CXSourceLocation, POINTER(c_void_p), POINTER(c_uint), POINTER(c_uint), POINTER(c_uint),
        ]
        lib.clang_getFileLocation.restype = None
        lib.clang_getFileName.argtypes = [c_void_p]
        lib.clang_getFileName.restype = CXString

        # -- 包含关系 --------------------------------------------------
        self._inclusion_visitor_type = CFUNCTYPE(None, c_void_p, POINTER(CXSourceLocation), c_uint, c_void_p)
        lib.clang_getInclusions.argtypes = [c_void_p, self._inclusion_visitor_type, c_void_p]
        lib.clang_getInclusions.restype = None

        # -- 诊断 ------------------------------------------------------
        lib.clang_getNumDiagnostics.argtypes = [c_void_p]
        lib.clang_getNumDiagnostics.restype = c_uint
        lib.clang_getDiagnostic.argtypes = [c_void_p, c_uint]
        lib.clang_getDiagnostic.restype = c_void_p
        lib.clang_getDiagnosticSeverity.argtypes = [c_void_p]
        lib.clang_getDiagnosticSeverity.restype = c_int
        lib.clang_getDiagnosticSpelling.argtypes = [c_void_p]
        lib.clang_getDiagnosticSpelling.restype = CXString
        lib.clang_getDiagnosticLocation.argtypes = [c_void_p]
        lib.clang_getDiagnosticLocation.restype = CXSourceLocation
        lib.clang_disposeDiagnostic.argtypes = [c_void_p]
        lib.clang_disposeDiagnostic.restype = None

    # -- 便捷方法 ------------------------------------------------------
    def str(self, value: CXString) -> str:
        try:
            raw = self.lib.clang_getCString(value)
            return raw.decode("utf-8", "replace") if raw else ""
        finally:
            self.lib.clang_disposeString(value)

    def version(self) -> str:
        return self.str(self.lib.clang_getClangVersion())

    def kind_name(self, kind: int) -> str:
        return self.str(self.lib.clang_getCursorKindSpelling(kind))

    def spelling(self, cursor: CXCursor) -> str:
        return self.str(self.lib.clang_getCursorSpelling(cursor))

    def display_name(self, cursor: CXCursor) -> str:
        return self.str(self.lib.clang_getCursorDisplayName(cursor))

    def usr(self, cursor: CXCursor) -> str:
        return self.str(self.lib.clang_getCursorUSR(cursor))

    def type_spelling(self, cursor: CXCursor) -> str:
        return self.str(self.lib.clang_getTypeSpelling(self.lib.clang_getCursorType(cursor)))

    def location(self, cursor: CXCursor, *, expansion: bool = False) -> tuple[str, int, int]:
        """返回 (绝对路径, 1 基行号, 1 基列号)，失败返回 ("", 0, 0)。"""
        loc = self.lib.clang_getCursorLocation(cursor)
        func = self.lib.clang_getExpansionLocation if expansion else self.lib.clang_getFileLocation
        file = c_void_p()
        line = c_uint()
        column = c_uint()
        offset = c_uint()
        func(loc, byref(file), byref(line), byref(column), byref(offset))
        path = self.str(self.lib.clang_getFileName(file)) if file.value else ""
        return path, int(line.value), int(column.value)

    def children(self, cursor: CXCursor) -> list[CXCursor]:
        out: list[CXCursor] = []

        @self._visitor_type
        def visitor(child: CXCursor, parent: CXCursor, data: c_void_p) -> int:
            out.append(child)
            return CHILD_CONTINUE

        self.lib.clang_visitChildren(cursor, visitor, None)
        return out

    def inclusions(self, tu: c_void_p) -> list[str]:
        out: list[str] = []

        @self._inclusion_visitor_type
        def visitor(included_file: c_void_p, stack, length: c_uint, data: c_void_p) -> None:
            if included_file:
                name = self.str(self.lib.clang_getFileName(included_file))
                if name:
                    out.append(name)

        self.lib.clang_getInclusions(tu, visitor, None)
        return out

    def overridden(self, cursor: CXCursor) -> list[CXCursor]:
        arr = POINTER(CXCursor)()
        count = c_uint()
        self.lib.clang_getOverriddenCursors(cursor, byref(arr), byref(count))
        result = [CXCursor.from_buffer_copy(arr[i]) for i in range(count.value)] if count.value else []
        if arr:
            self.lib.clang_disposeOverriddenCursors(arr)
        return result

    def diagnostics(self, tu: c_void_p) -> list[dict]:
        result = []
        total = int(self.lib.clang_getNumDiagnostics(tu))
        for i in range(total):
            diag = self.lib.clang_getDiagnostic(tu, i)
            severity = int(self.lib.clang_getDiagnosticSeverity(diag))
            text = self.str(self.lib.clang_getDiagnosticSpelling(diag))
            loc = self.lib.clang_getDiagnosticLocation(diag)
            file = c_void_p()
            line = c_uint()
            column = c_uint()
            offset = c_uint()
            self.lib.clang_getFileLocation(loc, byref(file), byref(line), byref(column), byref(offset))
            path = self.str(self.lib.clang_getFileName(file)) if file.value else ""
            result.append({"severity": severity, "text": text, "file": path, "line": int(line.value), "column": int(column.value)})
            self.lib.clang_disposeDiagnostic(diag)
        return result


_instance: LibClang | None = None


def get_libclang() -> LibClang:
    global _instance
    if _instance is None:
        _instance = LibClang()
    return _instance
