"""vividcode Python static indexer. Never imports or executes project code.

Cache syntax facts per content hash; resolve the entire project against those facts
on every refresh so import/definition changes also update dependent call sites.
"""
import argparse
import ast
import io
import json
from pathlib import Path
import sys
import tokenize

# Reuse the existing ownership checks and Windows lifecycle lock. Importing this
# module does not import C++ components (those load only inside the C++ command).
from analyzer import atomic_json, digest, emit, file_hash, layout, lifecycle_lock, reject_links, read_policy

VERSION = 'python-2'
MAX_BYTES = 2 * 1024 * 1024
EXTENSIONS = {'.py', '.pyw', '.pyi'}
EXCLUDED = {'.git', '.vs', '.codegraph', '.codemap', '.vividcode', 'build', 'out',
            'x64', 'x86', 'debug', 'release', 'node_modules', 'vendor', 'third_party',
            '_deps', 'cmakefiles', 'bin', 'obj', '__pycache__', '.venv', 'venv', 'env',
            '.pytest_cache', '.mypy_cache', '.tox', '.idea', 'dist', 'target', '.gradle',
            '.mvn', 'coverage', '.next', '.nuxt', '.cache', '.parcel-cache'}


def scan(root, policy=None):
    policy = policy if policy is not None else read_policy(root)
    pending, files, visited = [root], [], 0
    unity = (root / 'Assets').is_dir() and (root / 'ProjectSettings/ProjectVersion.txt').is_file()
    while pending:
        directory = pending.pop()
        for path in sorted(directory.iterdir()):
            visited += 1
            if not policy['force'] and visited > policy['maxEntries']:
                raise ValueError('Directory scan limit exceeded. Incomplete Python index was not published.')
            if path.is_symlink() or path.is_junction():
                continue
            reject_links(path)
            if path.is_dir():
                name = path.name.lower()
                if name in EXCLUDED or name.startswith(('.vividcode-', 'build-', 'cmake-build-')):
                    continue
                if unity and directory == root and name in {'library', 'temp', 'logs', 'usersettings'}:
                    continue
                pending.append(path)
            elif path.suffix.lower() in EXTENSIONS:
                files.append(path)
                if not policy['force'] and len(files) > policy['maxSourceFiles']:
                    raise ValueError('Python files exceeded the global limit. Incomplete index was not published.')
    return sorted(files)


def read_source(path, force=False):
    reject_links(path)
    with path.open('rb') as stream:
        raw = stream.read() if force else stream.read(MAX_BYTES + 1)
    if not force and len(raw) > MAX_BYTES:
        raise ValueError(f'{path.name} exceeds the 2 MiB read limit.')
    encoding, _ = tokenize.detect_encoding(io.BytesIO(raw).readline)
    return raw.decode(encoding), digest(raw)


def expression(node):
    if isinstance(node, ast.Name):
        return node.id
    if isinstance(node, ast.Attribute):
        base = expression(node.value)
        return base + '.' + node.attr if base else ''
    return ''


class Facts(ast.NodeVisitor):
    def __init__(self, relative, text):
        self.file, self.lines = relative, text.splitlines()
        self.symbols, self.scopes, self.sites = [], {}, []
        self.by_id, self.fields = {}, set()
        self.scope = self.symbol('', '<module>', 'ModuleDecl', ast.Module(), '')
        self.scopes[self.scope] = dict(parent='', kind='module', bindings={}, types={}, owner='')

    def column(self, node):
        line = getattr(node, 'lineno', 1)
        raw = self.lines[line - 1].encode('utf-8') if line <= len(self.lines) else b''
        # Qt QTextCursor columns count UTF-16 code units; AST counts UTF-8 bytes.
        prefix = raw[:getattr(node, 'col_offset', 0)].decode('utf-8')
        return len(prefix.encode('utf-16-le')) // 2 + 1

    def symbol(self, qualified, name, kind, node, parent):
        identity = 'py:' + digest((self.file + ':' + qualified + ':' + str(getattr(node, 'lineno', 1))).encode())
        line, col = getattr(node, 'lineno', 1), self.column(node)
        self.symbols.append(dict(id=identity, name=name, qualified_name=qualified or self.file,
                                 kind=kind, parent_id=parent, is_external=False,
                                 decl_file=self.file, decl_line=line, decl_col=col,
                                 def_file=self.file, def_line=line, def_col=col,
                                 signature='', decorated=False))
        self.by_id[identity] = self.symbols[-1]
        return identity

    def bind(self, name, binding):
        bindings = self.scopes[self.scope]['bindings']
        self.scopes[self.scope]['types'].pop(name, None)
        writes = self.scopes[self.scope].setdefault('writes', {})
        writes[name] = writes.get(name, 0) + 1
        # Multiple writes are ambiguous; do not silently select a same-name target.
        if name in bindings and bindings[name] != binding:
            bindings[name] = ['unknown', name]
        else:
            bindings[name] = binding

    def site(self, node, kind, target):
        self.sites.append(dict(scope=self.scope, type=kind, expr=target or ast.unparse(node)[:160],
                               file=self.file, line=node.lineno, col=self.column(node)))

    def variable(self, name, node, kind='VarDecl'):
        owner = self.scope
        scope = self.scopes[owner]
        directive = scope.get('directives', {}).get(name)
        if directive == 'nonlocal':
            return
        if directive == 'global':
            while self.scopes[owner]['parent']:
                owner = self.scopes[owner]['parent']
            scope = self.scopes[owner]
        variables = scope.setdefault('variables', {})
        if name not in variables:
            parent = self.by_id[owner]
            qualified = (parent['qualified_name'] + '.' if parent['kind'] != 'ModuleDecl' else '') + name
            variables[name] = self.symbol(qualified, name, kind, node, owner)
        return variables[name]

    def definition(self, node, kind):
        parent = self.scope
        parent_symbol = self.by_id[parent]
        qualified = (parent_symbol['qualified_name'] + '.' if parent_symbol['kind'] != 'ModuleDecl' else '') + node.name
        identity = self.symbol(qualified, node.name, kind, node, parent)
        symbol = self.symbols[-1]
        symbol['decorated'] = bool(node.decorator_list)
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            symbol['signature'] = ('async ' if isinstance(node, ast.AsyncFunctionDef) else '') + '(' + ast.unparse(node.args) + ')'
        self.bind(node.name, ['symbol', identity])
        for decorator in node.decorator_list:
            self.visit(decorator)
        if isinstance(node, ast.ClassDef):
            for base in node.bases:
                self.site(base, 'inherits', expression(base))
                self.sites[-1]['scope'] = identity
                self.sites[-1]['lookup_scope'] = parent
                self.visit(base)
            for keyword in node.keywords:
                self.visit(keyword.value)
        else:
            for value in [*node.args.defaults, *[v for v in node.args.kw_defaults if v], node.returns]:
                if value:
                    self.visit(value)
        owner = identity if kind == 'ClassDecl' else self.scopes[parent]['owner']
        self.scopes[identity] = dict(parent=parent, kind='class' if kind == 'ClassDecl' else 'function',
                                     bindings={}, types={}, owner=owner)
        self.scope = identity
        if hasattr(node, 'args'):
            args = [*node.args.posonlyargs, *node.args.args, *node.args.kwonlyargs]
            if node.args.vararg:
                args.append(node.args.vararg)
            if node.args.kwarg:
                args.append(node.args.kwarg)
            for arg in args:
                self.bind(arg.arg, ['unknown', arg.arg])
                self.variable(arg.arg, arg, 'ParmDecl')
                if arg.annotation:
                    annotation = arg.annotation
                    if isinstance(annotation, ast.Constant) and isinstance(annotation.value, str):
                        try:
                            annotation = ast.parse(annotation.value, mode='eval').body
                        except SyntaxError:
                            continue
                    self.scopes[identity]['types'][arg.arg] = [expression(annotation), parent]
            # The first instance/class parameter need not literally be named self.
            decorators = {expression(d) for d in node.decorator_list}
            if self.scopes[parent]['kind'] == 'class' and args and 'staticmethod' not in decorators:
                self.scopes[identity]['types'][args[0].arg] = ['@' + owner, parent]
        for statement in node.body:
            self.visit(statement)
        self.scope = parent

    def visit_ClassDef(self, node):
        self.definition(node, 'ClassDecl')

    def visit_FunctionDef(self, node):
        self.definition(node, 'CXXMethod' if self.scopes[self.scope]['kind'] == 'class' else 'FunctionDecl')

    visit_AsyncFunctionDef = visit_FunctionDef

    def visit_Import(self, node):
        for alias in node.names:
            self.bind(alias.asname or alias.name.split('.')[0], ['import', alias.name if alias.asname else alias.name.split('.')[0], '', 0])

    def visit_ImportFrom(self, node):
        for alias in node.names:
            if alias.name == '*':
                self.scopes[self.scope]['star_import'] = True
                continue
            self.bind(alias.asname or alias.name, ['import', node.module or '', alias.name, node.level])

    def visit_Call(self, node):
        self.site(node, 'calls', expression(node.func))
        # A call with computed callee is unresolved, never guessed from its text.
        if not expression(node.func):
            self.sites[-1]['expr'] = '<dynamic> ' + ast.unparse(node.func)[:120]
        self.generic_visit(node)

    def visit_Name(self, node):
        if isinstance(node.ctx, (ast.Store, ast.Del)):
            if isinstance(node.ctx, ast.Store):
                self.variable(node.id, node, 'FieldDecl' if self.scopes[self.scope]['kind'] == 'class' else 'VarDecl')
            self.bind(node.id, ['unknown', node.id])
        self.site(node, 'references', node.id)

    def visit_Attribute(self, node):
        self.site(node, 'member_ref', expression(node))
        self.generic_visit(node)

    def visit_Assign(self, node):
        self.visit(node.value)
        for target in node.targets:
            self.visit(target)
            if isinstance(target, ast.Name) and isinstance(node.value, ast.Call) and self.scopes[self.scope]['writes'][target.id] == 1:
                self.scopes[self.scope]['types'][target.id] = [expression(node.value.func), self.scope]
            self.field(target)

    def visit_AnnAssign(self, node):
        if node.value:
            self.visit(node.value)
        self.visit(node.target)
        if isinstance(node.target, ast.Name) and self.scopes[self.scope]['writes'][node.target.id] == 1:
            self.scopes[self.scope]['types'][node.target.id] = [expression(node.annotation), self.scope]
        self.field(node.target)

    def field(self, target):
        scope = self.scopes[self.scope]
        owner = scope['owner']
        name = target.id if isinstance(target, ast.Name) and scope['kind'] == 'class' else ''
        if isinstance(target, ast.Attribute) and isinstance(target.value, ast.Name):
            inferred = scope['types'].get(target.value.id, [''])[0]
            if inferred == '@' + owner:
                name = target.attr
        if owner and name:
            cls = self.by_id[owner]
            qualified = cls['qualified_name'] + '.' + name
            if (owner, name) not in self.fields:
                self.fields.add((owner, name))
                identity = self.scopes[owner].get('variables', {}).get(name) or self.symbol(qualified, name, 'FieldDecl', target, owner)
                self.scopes[owner]['bindings'].setdefault(name, ['symbol', identity])
                self.scopes[owner].setdefault('variables', {})[name] = identity

    def visit_Lambda(self, node):
        for value in [*node.args.defaults, *[v for v in node.args.kw_defaults if v]]:
            self.visit(value)
        parent = self.scope
        name = '<lambda@%d:%d>' % (node.lineno, self.column(node))
        identity = self.symbol(self.by_id[parent]['qualified_name'] + '.' + name, name, 'FunctionDecl', node, parent)
        self.scopes[identity] = dict(parent=parent, kind='function', bindings={}, types={}, owner='')
        self.scope = identity
        args = [*node.args.posonlyargs, *node.args.args, *node.args.kwonlyargs]
        args += [arg for arg in (node.args.vararg, node.args.kwarg) if arg]
        for arg in args:
            self.bind(arg.arg, ['unknown', arg.arg])
            self.variable(arg.arg, arg, 'ParmDecl')
        self.visit(node.body)
        self.scope = parent

    def comprehension(self, node):
        # The outermost iterable runs outside the implicit comprehension scope.
        self.visit(node.generators[0].iter)
        parent = self.scope
        name = '<comprehension@%d:%d>' % (node.lineno, self.column(node))
        identity = self.symbol(self.by_id[parent]['qualified_name'] + '.' + name, name, 'FunctionDecl', node, parent)
        self.scopes[identity] = dict(parent=parent, kind='function', bindings={}, types={}, owner='', comprehension=True)
        self.scope = identity
        for index, generator in enumerate(node.generators):
            if index:
                self.visit(generator.iter)
            self.visit(generator.target)
            for condition in generator.ifs:
                self.visit(condition)
        for value in ([node.key, node.value] if isinstance(node, ast.DictComp) else [node.elt]):
            self.visit(value)
        self.scope = parent

    visit_ListComp = comprehension
    visit_SetComp = comprehension
    visit_DictComp = comprehension
    visit_GeneratorExp = comprehension

    def visit_NamedExpr(self, node):
        self.visit(node.value)
        previous = self.scope
        while self.scopes[self.scope].get('comprehension'):
            self.scope = self.scopes[self.scope]['parent']
        self.visit(node.target)
        self.scope = previous

    def visit_Global(self, node):
        for name in node.names:
            self.scopes[self.scope].setdefault('directives', {})[name] = 'global'
            self.bind(name, ['unknown', name])

    def visit_Nonlocal(self, node):
        for name in node.names:
            self.scopes[self.scope].setdefault('directives', {})[name] = 'nonlocal'
            self.bind(name, ['unknown', name])

    def visit_ExceptHandler(self, node):
        if node.name:
            self.bind(node.name, ['unknown', node.name])
            self.variable(node.name, node)
        self.generic_visit(node)

    def visit_MatchAs(self, node):
        if node.name:
            self.bind(node.name, ['unknown', node.name])
            self.variable(node.name, node)
        self.generic_visit(node)

    visit_MatchStar = visit_MatchAs

    def visit_MatchMapping(self, node):
        if node.rest:
            self.bind(node.rest, ['unknown', node.rest])
            self.variable(node.rest, node)
        self.generic_visit(node)

    def result(self, tree):
        self.visit(tree)
        return dict(file=self.file, symbols=self.symbols, scopes=self.scopes, sites=self.sites)


class Resolver:
    def __init__(self, facts):
        self.facts = facts
        self.symbols = {s['id']: s for f in facts.values() for s in f['symbols']}
        self.scopes = {k: v for f in facts.values() for k, v in f['scopes'].items()}
        self.modules, self.module_for, self.root_for = {}, {}, {}
        self.module_names = {}
        # Conventional root/src source roots; ambiguous matches stay unresolved.
        for file, fact in facts.items():
            path = Path(file)
            parts = list(path.with_suffix('').parts)
            if parts[-1].lower() == '__init__':
                parts.pop()
            for prefix in (0, 1) if parts and parts[0] == 'src' else (0,):
                module = '.'.join(parts[prefix:])
                self.modules.setdefault(module, []).append(next(iter(fact['scopes'])))
            self.module_for[file] = '.'.join(parts[1:] if parts and parts[0] == 'src' else parts)
            self.root_for[file] = next(iter(fact['scopes']))
            self.module_names[self.root_for[file]] = self.module_for[file]
        # Namespace packages have no __init__.py, but can still be imported.
        for name in list(self.modules):
            parts = name.split('.')
            for length in range(1, len(parts)):
                parent = '.'.join(parts[:length])
                if parent not in self.modules:
                    identity = 'py:namespace:' + parent
                    self.modules[parent] = [identity]
                    self.module_names[identity] = parent
                    self.symbols[identity] = dict(id=identity, name=parent, qualified_name=parent,
                                                 kind='ModuleDecl', parent_id='', is_external=False)
                    self.scopes[identity] = dict(parent='', kind='module', bindings={}, types={})
        # Prefer implementation to adjacent .pyi; don't collapse different roots.
        for name, ids in self.modules.items():
            implementations = [i for i in ids if Path(self.symbols[i].get('def_file', '')).suffix.lower() != '.pyi']
            self.modules[name] = implementations or ids
        self.bases = {}
        for fact in facts.values():
            for site in fact['sites']:
                if site['type'] == 'inherits':
                    target, _ = self.resolve(site['expr'], site['lookup_scope'])
                    if target in self.symbols and self.symbols[target]['kind'] == 'ClassDecl':
                        self.bases.setdefault(site['scope'], []).append(target)

    def module(self, name):
        matches = self.modules.get(name, [])
        return matches[0] if len(matches) == 1 else None

    def lookup(self, name, scope):
        initial = scope
        while scope:
            data = self.scopes[scope]
            if name in data['bindings']:
                return data['bindings'][name], scope
            if data.get('star_import'):
                return ['unknown', name], scope
            scope = data['parent']
            # A method's bare names don't inherit the enclosing class namespace.
            if self.scopes[initial]['kind'] == 'function':
                while scope and self.scopes[scope]['kind'] == 'class':
                    scope = self.scopes[scope]['parent']
        return None, initial

    def member(self, identity, name, trail):
        if identity not in self.scopes or identity in trail:
            return None
        data = self.scopes[identity]
        if name in data['bindings']:
            return self.binding(data['bindings'][name], identity, trail | {identity})
        targets = {self.member(base, name, trail | {identity}) for base in self.bases.get(identity, [])}
        targets.discard(None)
        return targets.pop() if len(targets) == 1 else None

    def binding(self, value, scope, trail):
        if value[0] == 'symbol':
            return value[1]
        if value[0] != 'import':
            return None
        _, module, item, level = value
        file = self.symbols[scope]['def_file']
        if level:
            package = self.module_for[file].split('.')
            if Path(file).stem.lower() != '__init__':
                package = package[:-1]
            if level > len(package):
                return None
            module = '.'.join(package[:len(package) - level + 1] + ([module] if module else []))
        endpoint = self.module(module)
        if item:
            child = self.module(module + '.' + item)
            return self.member(endpoint, item, trail) if endpoint and not child else child
        return endpoint

    def resolve(self, expr, scope, trail=frozenset()):
        if not expr or scope not in self.scopes or (scope, expr) in trail:
            return None, False
        trail = trail | {(scope, expr)}
        parts = expr.split('.')
        binding, owner_scope = self.lookup(parts[0], scope)
        target = self.binding(binding, owner_scope, trail) if binding else None
        inferred = self.scopes[owner_scope]['types'].get(parts[0])
        dynamic = False
        if len(parts) > 1 and inferred:
            target = inferred[0][1:] if inferred[0].startswith('@') else self.resolve(inferred[0], inferred[1], trail)[0]
            if target not in self.symbols or self.symbols[target]['kind'] != 'ClassDecl':
                target = None
            dynamic = True
        for n, part in enumerate(parts[1:], 1):
            if target and self.symbols[target]['kind'] == 'ModuleDecl':
                child = self.module(self.module_names[target] + '.' + part)
                target = child or self.member(target, part, trail)
            else:
                target = self.member(target, part, trail) if target else None
        if target and self.symbols[target].get('decorated'):
            dynamic = True
        return target, dynamic

    def variable_target(self, expr, scope, trail=frozenset()):
        if not expr or scope not in self.scopes or (scope, expr) in trail:
            return None
        trail = trail | {(scope, expr)}
        parts = expr.split('.')
        if len(parts) > 1:
            base, _ = self.resolve('.'.join(parts[:-1]), scope)
            if not base and len(parts) == 2:
                _, owner = self.lookup(parts[0], scope)
                inferred = self.scopes[owner]['types'].get(parts[0])
                if inferred:
                    base = inferred[0][1:] if inferred[0].startswith('@') else self.resolve(inferred[0], inferred[1])[0]
            if base in self.scopes:
                return self.scopes[base].get('variables', {}).get(parts[-1]) or self.member(base, parts[-1], frozenset())
            return None
        name, initial = parts[0], scope
        while scope:
            data = self.scopes[scope]
            directive = data.get('directives', {}).get(name)
            if directive == 'global':
                scope = self.root_for[self.symbols[initial]['decl_file']]
                if scope == initial:
                    return None
                continue
            if directive == 'nonlocal':
                scope = data['parent']
                while scope and self.scopes[scope]['kind'] != 'function':
                    scope = self.scopes[scope]['parent']
                continue
            if name in data.get('variables', {}):
                return data['variables'][name]
            binding = data['bindings'].get(name)
            if binding:
                if binding[0] == 'symbol':
                    return binding[1]
                if binding[0] == 'import':
                    _, module, item, level = binding
                    file = self.symbols[scope]['decl_file']
                    if level:
                        package = self.module_for[file].split('.')
                        if Path(file).stem != '__init__':
                            package.pop()
                        module = '.'.join(package[:len(package) - level + 1] + ([module] if module else []))
                    endpoint = self.module(module)
                    return self.variable_target(item, endpoint, trail) if endpoint and item else endpoint
                return None
            if data.get('star_import'):
                return None
            scope = data['parent']
            if self.scopes[initial]['kind'] == 'function':
                while scope and self.scopes[scope]['kind'] == 'class':
                    scope = self.scopes[scope]['parent']
        return None

    def units(self):
        units = []
        unresolved = 0
        for file, fact in self.facts.items():
            endpoints = {s['id']: s for s in fact['symbols']}
            relations = []
            seen = set()
            for site in fact['sites']:
                scope = site.get('lookup_scope', site['scope'])
                kind = site['type']
                target, dynamic = self.resolve(site['expr'], scope)
                if kind in {'references', 'member_ref'}:
                    target = self.variable_target(site['expr'], scope) or target
                    if not target or self.symbols[target]['kind'] not in {'VarDecl', 'ParmDecl', 'FieldDecl'}:
                        continue
                    kind = 'member_ref' if self.symbols[target]['kind'] == 'FieldDecl' else 'references'
                if kind == 'member_ref' and (not target or self.symbols[target]['kind'] != 'FieldDecl'):
                    continue
                if not target:
                    if kind == 'inherits':
                        unresolved += 1
                    elif kind == 'calls':
                        unresolved += 1
                    target = 'py:unresolved:' + digest((file + ':' + scope + ':' + site['expr']).encode())
                    # Unknown locals and external imports share explicit unresolved
                    # endpoints; no project symbol is selected merely by name.
                    endpoints[target] = dict(id=target, name='Unresolved: ' + site['expr'], qualified_name='Unresolved: ' + site['expr'],
                                             kind='UnresolvedCall', is_external=True, unresolved=True, parent_id='')
                    if kind == 'calls':
                        kind = 'possible_call'
                else:
                    endpoints[target] = self.symbols[target]
                    if kind == 'calls' and self.symbols[target]['kind'] not in {'FunctionDecl', 'CXXMethod', 'ClassDecl'}:
                        kind = 'possible_call'
                        dynamic = True
                    elif kind == 'calls' and dynamic:
                        kind = 'possible_call'
                relation = dict(source_id=site['scope'], target_id=target, type=kind,
                                confidence='unresolved' if endpoints[target].get('unresolved') else 'possible' if dynamic else 'static',
                                file=file, line=site['line'], col=site['col'])
                key = tuple(relation.values())
                if key not in seen:
                    relations.append(relation)
                    seen.add(key)
            # Include parents of cross-file targets so the shared canvas groups
            # methods under their actual class, also in a single-file view.
            for identity in list(endpoints):
                parent = endpoints[identity].get('parent_id')
                while parent and parent not in endpoints:
                    endpoints[parent] = self.symbols[parent]
                    parent = endpoints[parent].get('parent_id')
            units.append(dict(file=file, scope_name=file, config_id=VERSION + ':' + file,
                              symbols=list(endpoints.values()), relations=relations))
        return units, unresolved


def cache_paths(project):
    root, shared = layout(project)
    cache = shared / 'python'
    for path in (cache, cache / 'config.json', cache / 'index.json'):
        reject_links(path)
    return root, shared, cache


def index_project(project):
    root, _, _ = cache_paths(project)
    with lifecycle_lock(root):
        root, shared, cache = cache_paths(root)
        cache.mkdir(parents=True, exist_ok=True)
        atomic_json(shared / 'project.json', dict(app='vividcode', schema=1, projectRoot=root.as_posix()))
        state = dict(schema=1, language='python', project_root=root.as_posix(), last_status='running', parser=sys.version.split()[0])
        atomic_json(cache / 'config.json', state)
        try:
            old = json.loads((cache / 'index.json').read_text(encoding='utf-8')) if (cache / 'index.json').exists() else {}
            policy = read_policy(root)
            signature = VERSION + ':' + file_hash(Path(__file__)) + ':' + json.dumps(policy, sort_keys=True)
            old_facts = old.get('facts', {}) if old.get('version') == signature and old.get('parser') == state['parser'] else {}
            paths = scan(root, policy)
            facts, hashes, reused = {}, {}, 0
            for n, path in enumerate(paths):
                relative = path.relative_to(root).as_posix()
                emit(type='progress', current=n + 1, total=len(paths), file=relative)
                text, content_hash = read_source(path, policy['force'])
                hashes[relative] = content_hash
                if old.get('hashes', {}).get(relative) == content_hash and relative in old_facts:
                    facts[relative] = old_facts[relative]
                    reused += 1
                else:
                    try:
                        tree = ast.parse(text, filename=relative)
                    except SyntaxError as exc:
                        raise ValueError(f'{relative}:{exc.lineno}:{exc.offset} · {exc.msg}') from exc
                    facts[relative] = Facts(relative, text).result(tree)
            units, unresolved = Resolver(facts).units()
            if read_policy(root) != policy or scan(root, policy) != paths or any(file_hash(root / file) != value for file, value in hashes.items()):
                raise ValueError('Source changed during analysis. Press F5 to retry; previous results retained.')
            state.update(last_status='ok', total=len(paths), indexed=len(paths) - reused, reused=reused, unresolved=unresolved,
                         analysis='static', message='Python static analysis searches the project root and src. Instance/decorator calls are possible calls; dynamic/external targets are unresolved. Lambdas/comprehensions have separate scopes. Project code is not executed.')
            state['message'] += f' {unresolved} unresolved relations.'
            payload = dict(version=signature, parser=state['parser'], hashes=hashes, facts=facts, units=units)
            if len(json.dumps(payload, ensure_ascii=False).encode('utf-8')) > 128 * 1024 * 1024:
                raise ValueError('Python results exceed 128 MiB. Incomplete results were not published.')
            atomic_json(cache / 'index.json', payload)
            atomic_json(cache / 'config.json', state)
            emit(type='result', ok=True, **state)
            return 0
        except (OSError, ValueError, SyntaxError, UnicodeError, LookupError, RecursionError, KeyboardInterrupt) as exc:
            state.update(last_status='failed', message=str(exc) or 'Cancelled; previous results retained.')
            atomic_json(cache / 'config.json', state)
            emit(type='result', ok=False, **state)
            return 1


def export_project(project):
    _, _, cache = cache_paths(project)
    state = json.loads((cache / 'config.json').read_text(encoding='utf-8')) if (cache / 'config.json').exists() else {}
    if state.get('last_status') == 'running':
        state.update(last_status='interrupted', message='Previous analysis did not finish. Previous results retained; press F5 to retry.')
    snapshot = json.loads((cache / 'index.json').read_text(encoding='utf-8')) if (cache / 'index.json').exists() else {}
    return dict(ok=True, schema=1, language='python', state=state, units=snapshot.get('units', []))


def main():
    parser = argparse.ArgumentParser(description='vividcode Python static analysis')
    parser.add_argument('command', choices=['index', 'export', 'source'])
    parser.add_argument('--project', required=True)
    parser.add_argument('--file')
    args = parser.parse_args()
    try:
        if args.command == 'index':
            return index_project(args.project)
        if args.command == 'source':
            root = Path(args.project).resolve()
            path = root / (args.file or '')
            reject_links(path)
            path = path.resolve()
            if not path.is_relative_to(root) or path.suffix.lower() not in EXTENSIONS:
                raise ValueError('Only Python source inside the project can be read.')
            text, _ = read_source(path, read_policy(root)['force'])
            emit(ok=True, text=text)
        else:
            root, _, _ = cache_paths(args.project)
            with lifecycle_lock(root):
                emit(**export_project(root))
        return 0
    except (OSError, ValueError, RuntimeError, SyntaxError, LookupError) as exc:
        emit(type='result', ok=False, message=str(exc))
        return 1


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    sys.stderr.reconfigure(encoding='utf-8')
    sys.exit(main())
