"""vividcode C++ indexer. JSONL protocol v1; SQLite schema v2.

Only parses compilation commands; never executes them. A failed refresh keeps the
last complete snapshot. Each TU/config owns its symbols, relations and dependencies.
"""
import argparse
import contextlib
import ctypes
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import sys
import time


VERSION = '2.0.1'
SCHEMA = 2
SOURCE_EXTS = {'.cc', '.cpp', '.cxx', '.c++'}


def emit(**data):
    print(json.dumps({'protocol': 1, **data}, ensure_ascii=False), flush=True)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_hash(path):
    with open(path, 'rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def reject_links(path):
    p = Path(os.path.abspath(path))
    for part in [p, *p.parents]:
        if part.is_symlink() or (hasattr(part, 'is_junction') and part.is_junction()):
            raise ValueError('Linked index path rejected: ' + str(part))
        if part.exists() and getattr(part.stat(), 'st_file_attributes', 0) & 0x400:
            raise ValueError('Reparse point rejected: ' + str(part))


def shared_home():
    configured = os.environ.get('VIVIDCODE_HOME')
    home = Path(configured) if configured else Path(os.environ['LOCALAPPDATA']) / 'vividcode'
    if not home.is_absolute():
        raise ValueError('Shared directory must be an absolute path.')
    return home


def read_policy(root):
    settings = shared_home() / 'read-policy.json'
    reject_links(settings)
    data = json.loads(settings.read_text(encoding='utf-8')) if settings.exists() else {}
    count = data.get('maxSourceFiles', 20000)
    if data and (data.get('app') != 'vividcode' or data.get('schema') != 1):
        raise ValueError('Invalid source read policy format.')
    if type(count) is not int or not 1 <= count <= 2147483647:
        raise ValueError('Invalid source file count limit.')
    manifest = Path(root) / '.vividcode/project.json'
    reject_links(manifest)
    project = json.loads(manifest.read_text(encoding='utf-8-sig')) if manifest.exists() else {}
    return dict(maxSourceFiles=count, maxEntries=max(50000, count * 5 // 2),
                force=project.get('app') == 'vividcode' and project.get('schema') == 1 and project.get('forceRead') is True)

def semantic_directory(project):
    # Same canonical lower-case UTF-8 path hash as SharedRuntime.h.
    root = Path(project).resolve()
    key = digest(root.as_posix().lower().encode('utf-8'))
    return shared_home() / 'semantic' / key


def layout(project):
    root = Path(os.path.abspath(project))
    reject_links(root / '.vividcode')
    if not root.is_dir() or root.name.lower() == '.vividcode':
        raise ValueError('Select the project root directory.')
    root = root.resolve()
    manifest = root / '.vividcode/project.json'
    reject_links(manifest)
    if not manifest.is_file():
        raise ValueError('Project not initialized. Run vividcode -g from the project root first.')
    data = json.loads(manifest.read_text(encoding='utf-8-sig'))
    if data.get('app') != 'vividcode' or data.get('schema') != 1:
        raise ValueError('Unsupported project.json.')
    sem = semantic_directory(root)
    reject_links(sem)
    if sem.exists():
        for path in sem.iterdir():
            reject_links(path)
        owner = json.loads((sem / 'project.json').read_text(encoding='utf-8-sig'))
        if owner.get('app') != 'vividcode' or owner.get('schema') != 1 or Path(owner.get('projectRoot', '')).as_posix().lower() != root.as_posix().lower():
            raise ValueError('Shared index ownership mismatch.')
    return root, sem


@contextlib.contextmanager
def lifecycle_lock(root):
    """Same Win32 exclusive handle as ProjectStore; crash closes the OS handle."""
    from ctypes import wintypes as w
    path = str(root / '.vividcode-semantic.lock')
    reject_links(path)
    k = ctypes.WinDLL('kernel32', use_last_error=True)
    k.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, ctypes.c_void_p, w.DWORD, w.DWORD, w.HANDLE]
    k.CreateFileW.restype = w.HANDLE
    k.CloseHandle.argtypes = [w.HANDLE]
    handle = k.CreateFileW(path, 0xC0010000, 0, None, 4, 0x04000000, None)
    if handle == ctypes.c_void_p(-1).value:
        raise RuntimeError('Another indexing, generation or deletion operation is running.')
    try:
        yield
    finally:
        k.CloseHandle(handle)


def atomic_json(path, value):
    reject_links(path)
    temp = path.with_suffix('.tmp')
    reject_links(temp)
    temp.write_text(json.dumps(value, ensure_ascii=False), encoding='utf-8')
    os.replace(temp, path)


def split_command(command):
    from ctypes import wintypes as w
    s = ctypes.WinDLL('shell32', use_last_error=True)
    k = ctypes.WinDLL('kernel32', use_last_error=True)
    s.CommandLineToArgvW.argtypes = [w.LPCWSTR, ctypes.POINTER(ctypes.c_int)]
    s.CommandLineToArgvW.restype = ctypes.POINTER(w.LPWSTR)
    k.LocalFree.argtypes = [ctypes.c_void_p]
    k.LocalFree.restype = ctypes.c_void_p
    count = ctypes.c_int()
    ptr = s.CommandLineToArgvW(command.strip(), ctypes.byref(count))
    if not ptr:
        raise ValueError('Could not split the compiler command.')
    try:
        return [ptr[i] for i in range(count.value)]
    finally:
        k.LocalFree(ptr)


def commands(path):
    entries = json.loads(Path(path).read_text(encoding='utf-8-sig'))
    if not isinstance(entries, list):
        raise ValueError('Compilation database must contain a top-level array.')
    result = {}
    for entry in entries:
        if not isinstance(entry, dict):
            raise ValueError('Compilation database entries must be objects.')
        directory, source = entry.get('directory'), entry.get('file')
        if not isinstance(directory, str) or not os.path.isabs(directory) or not Path(directory).is_dir():
            raise ValueError('Compilation entry directory must be a valid absolute directory.')
        if not isinstance(source, str):
            raise ValueError('Compilation entry is missing file.')
        source = os.path.abspath(os.path.join(directory, source))
        if Path(source).suffix.lower() not in SOURCE_EXTS:
            continue
        argv = entry.get('arguments')
        if argv is None:
            if not isinstance(entry.get('command'), str):
                raise ValueError('Compilation entry is missing arguments or command.')
            argv = split_command(entry['command'])
        if not isinstance(argv, list) or not argv or not all(isinstance(a, str) for a in argv):
            raise ValueError('arguments must be a non-empty array of strings.')
        mode = 'cl' if Path(argv[0]).name.lower() in {'cl.exe', 'cl', 'clang-cl.exe', 'clang-cl'} else 'gcc'
        args, i = [], 1
        while i < len(argv):
            arg = argv[i]
            if arg.startswith('@') or arg.startswith(('/Yu', '/Yc')) or arg == '-include-pch':
                raise ValueError('Response files and precompiled header arguments are not supported: ' + arg)
            # Preserve semantic options, including optimisation/runtime macros.
            normalized = '/' + arg[1:] if mode == 'cl' and arg.startswith('-') and not arg.startswith('--') else arg
            output = {'/Fo', '/Fd', '/Fe', '/Fa', '/Fp'} if mode == 'cl' else {'-o', '-MF', '-MT', '-MQ', '-MJ'}
            skip = {'/c', '/nologo', '/showIncludes', '/FS', '/MP', '/Zi', '/Z7'} if mode == 'cl' else {'-c', '-MMD', '-MD', '-MP', '-fcolor-diagnostics'}
            if normalized in output:
                i += 2
                continue
            if normalized in skip or (mode == 'cl' and any(normalized.startswith(p) and len(normalized) > len(p) for p in output)):
                i += 1
                continue
            if arg == source or os.path.normcase(os.path.abspath(os.path.join(directory, arg))) == os.path.normcase(source):
                i += 1
                continue
            # Options with a following value must stay paired (e.g. -include a.hpp).
            pairs = {'-I', '-isystem', '-iquote', '-include', '-imacros', '-isysroot', '--sysroot', '-target', '-x', '-D', '-U', '/I', '/FI', '/D', '/U', '/external:I'}
            if arg in pairs or normalized in pairs:
                if i + 1 >= len(argv):
                    raise ValueError('Missing option value: ' + arg)
                args.extend([arg, argv[i + 1]])
                i += 2
                continue
            args.append(arg)
            i += 1
        if mode == 'cl':
            args.insert(0, '--driver-mode=cl')
        record = {'file': source, 'directory': directory, 'arguments': args, 'compiler': argv[0], 'mode': mode}
        key = digest(json.dumps(record, sort_keys=True).encode())[:24]
        record['config_id'] = key
        result[key] = record
    return result


def connect(path, readonly=False):
    reject_links(path)
    if readonly:
        conn = sqlite3.connect(path.as_uri() + '?mode=ro', uri=True)
    else:
        conn = sqlite3.connect(path)
    version = conn.execute('PRAGMA user_version').fetchone()[0]
    if version not in (0, SCHEMA) or (version == 0 and conn.execute("SELECT name FROM sqlite_master WHERE type='table'").fetchone()):
        conn.close()
        raise ValueError('Incompatible index database version. Reinitialize this project index.')
    if not readonly and version == 0:
        conn.executescript('CREATE TABLE IF NOT EXISTS units (id TEXT PRIMARY KEY, command TEXT NOT NULL, fingerprint TEXT NOT NULL, dependencies TEXT NOT NULL, payload TEXT NOT NULL); CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY,value TEXT NOT NULL);')
        conn.execute(f'PRAGMA user_version={SCHEMA}')
    return conn


def inventory(root, records):
    """Track newly created/deleted project files and immediate include search dirs.

    Contents of resolved dependencies are hashed separately. Directory mtimes alone
    cannot detect a missing __has_include__ target becoming available.
    """
    roots = {str(root)}
    for record in records.values():
        args = record['arguments']
        for i, arg in enumerate(args):
            value = None
            if arg in {'-I', '-isystem', '-iquote', '-idirafter', '-imsvc', '/I', '/external:I'} and i + 1 < len(args):
                value = args[i + 1]
            else:
                for prefix in ('/external:I', '-external:I', '-isystem', '-imsvc', '-I', '/I'):
                    if arg.startswith(prefix) and len(arg) > len(prefix):
                        value = arg[len(prefix):]
                        break
            if value:
                roots.add(os.path.abspath(os.path.join(record['directory'], value)))
    for key in ('INCLUDE', 'CPATH', 'CPLUS_INCLUDE_PATH'):
        roots.update(p for p in os.environ.get(key, '').split(os.pathsep) if p)
    paths = []
    for search_root in sorted(roots):
        paths.append(search_root)
        for directory, dirs, files in os.walk(search_root, followlinks=False):
            dirs[:] = sorted(d for d in dirs if d not in {'.vividcode', '.git', '.codegraph', '.vs', '.work', '__pycache__'} and not d.lower().startswith('.vividcode-') and not Path(directory, d).is_symlink() and not getattr(Path(directory, d).stat(), 'st_file_attributes', 0) & 0x400)
            paths.extend(str(Path(directory, f)) for f in sorted(files))
    # Environment include paths are part of the effective build configuration.
    env = {k: os.environ.get(k, '') for k in ('INCLUDE', 'CPATH', 'CPLUS_INCLUDE_PATH', 'SDKROOT', 'VIVIDCODE_CLANG_RESOURCE')}
    return digest(json.dumps([paths, env], sort_keys=True).encode())


def parse_unit(root, record, lib):
    import clang_bridge
    from extractor import TuExtractor
    source = record['file']
    before = file_hash(source)
    started = time.time_ns()
    args = list(record['arguments'])
    configured_resource = os.environ.get('VIVIDCODE_CLANG_RESOURCE')
    if configured_resource:
        resource = Path(configured_resource)
        if not (resource / 'include/stddef.h').is_file():
            raise ValueError('Invalid Clang built-in headers directory: ' + str(resource))
    else:
        resource_root = Path(lib.path).resolve().parent.parent / 'lib/clang'
        resources = [p for p in resource_root.iterdir() if (p / 'include/stddef.h').is_file()] if resource_root.is_dir() else []
        version = __import__('re').search(r'(?:clang version|LLVM version)\s+(\d+)', lib.version())
        resources = [p for p in resources if version and p.name.split('.')[0] == version.group(1)]
        resource = sorted(resources)[-1] if resources else None
    if resource is not None:
        args += ['-resource-dir', str(resource.resolve())]
    index = lib.lib.clang_createIndex(0, 0)
    tu = ctypes.c_void_p()
    try:
        encoded = (ctypes.c_char_p * len(args))(*(a.encode('utf-8') for a in args))
        with contextlib.chdir(record['directory']):
            error = lib.lib.clang_parseTranslationUnit2(index, source.encode('utf-8'), encoded, len(args), None, 0, clang_bridge.CXTRANSLATIONUNIT_KEEP_GOING, ctypes.byref(tu))
            if error or not tu.value:
                raise ValueError(f'libclang parse failed, error {error}: {source}')
            diagnostics = lib.diagnostics(tu)
            failures = [d for d in diagnostics if d['severity'] >= clang_bridge.SEV_ERROR]
            if failures:
                raise ValueError('\n'.join(f"{d['file']}:{d['line']}:{d['column']}: {d['text']}" for d in failures[:30]))
            symbols, relations = TuExtractor(lib, str(root)).extract(tu)
            includes = {os.path.abspath(p) for p in lib.inclusions(tu)} | {source}
        dependencies = {p: file_hash(p) for p in sorted(includes)}
        if dependencies[source] != before or any(os.stat(p).st_mtime_ns >= started for p in includes):
            raise ValueError('Source or headers changed during parsing. Please retry.')
        return {'symbols': symbols, 'relations': relations, 'diagnostics': diagnostics}, dependencies
    finally:
        if tu.value:
            lib.lib.clang_disposeTranslationUnit(tu)
        lib.lib.clang_disposeIndex(index)


def index_project(project, compilation):
    root, sem = layout(project)
    with lifecycle_lock(root):
        # Revalidate after acquiring the lifecycle lock; -d may have just finished.
        root, sem = layout(root)
        sem.mkdir(parents=True, exist_ok=True)
        atomic_json(sem / 'project.json', {'app': 'vividcode', 'schema': 1, 'projectRoot': root.as_posix()})
        compilation = str(Path(compilation).resolve())
        state = {'compile_commands': compilation, 'last_status': 'running', 'schema': SCHEMA, 'language': 'c++'}
        atomic_json(sem / 'config.json', state)
        db = None
        try:
            records = commands(compilation)
            if not records:
                raise ValueError('No C++ source files in the compilation database.')
            import clang_bridge
            lib = clang_bridge.get_libclang()
            parser = lib.version()
            signature = digest((VERSION + parser + file_hash(lib.path) + inventory(root, records)).encode())
            db = connect(sem / 'index.db')
            old = {row[0]: row[1:] for row in db.execute('SELECT id,fingerprint,dependencies FROM units')}
            updates, reused, failures = {}, 0, []
            for n, (key, record) in enumerate(records.items()):
                emit(type='progress', current=n + 1, total=len(records), file=record['file'])
                cached = old.get(key)
                if cached and cached[0] == signature:
                    try:
                        if all(file_hash(p) == h for p, h in json.loads(cached[1]).items()):
                            reused += 1
                            continue
                    except OSError:
                        pass
                try:
                    payload, deps = parse_unit(root, record, lib)
                    updates[key] = (json.dumps(record), signature, json.dumps(deps), json.dumps(payload, ensure_ascii=False))
                except (OSError, ValueError, RuntimeError) as exc:
                    failures.append({'file': record['file'], 'message': str(exc)})
                    emit(type='diagnostic', file=record['file'], message=str(exc))
            if failures:
                state.update(last_status='failed', errors=failures)
                raise ValueError(f'{len(failures)} translation units failed. Previous complete index retained.')
            # Validate content again immediately before commit (including reused TUs).
            for key in records:
                deps = json.loads(updates[key][2] if key in updates else old[key][1])
                if any(file_hash(p) != h for p, h in deps.items()):
                    raise ValueError('Files changed before commit. Please reindex.')
            with db:
                for key in set(old) - set(records):
                    db.execute('DELETE FROM units WHERE id=?', (key,))
                for key, values in updates.items():
                    db.execute('INSERT OR REPLACE INTO units VALUES(?,?,?,?,?)', (key, *values))
                db.execute('INSERT OR REPLACE INTO meta VALUES(?,?)', ('parser', parser))
            state.update(last_status='ok', indexed=len(updates), reused=reused, total=len(records), parser=parser)
            atomic_json(sem / 'config.json', state)
            emit(type='result', ok=True, **state)
            return 0
        except (OSError, ValueError, RuntimeError, sqlite3.Error, KeyboardInterrupt) as exc:
            state.update(last_status='failed', message=str(exc) or 'Cancelled')
            atomic_json(sem / 'config.json', state)
            emit(type='result', ok=False, **state)
            return 1
        finally:
            if db:
                db.close()


def export_project(project):
    root, sem = layout(project)
    state = json.loads((sem / 'config.json').read_text(encoding='utf-8')) if (sem / 'config.json').exists() else {}
    output = {'ok': True, 'schema': SCHEMA, 'language': 'c++', 'state': state, 'units': []}
    if (sem / 'index.db').exists():
        db = connect(sem / 'index.db', readonly=True)
        try:
            for key, command, payload in db.execute('SELECT id,command,payload FROM units ORDER BY id'):
                output['units'].append({'config_id': key, **json.loads(command), **json.loads(payload)})
        finally:
            db.close()
    return output


def main():
    parser = argparse.ArgumentParser(description='vividcode C++ semantic index')
    parser.add_argument('command', choices=['index', 'export'])
    parser.add_argument('--project', required=True)
    parser.add_argument('--compile-commands')
    args = parser.parse_args()
    try:
        if args.command == 'index':
            if not args.compile_commands:
                raise ValueError('Provide --compile-commands.')
            return index_project(args.project, args.compile_commands)
        root, _ = layout(args.project)
        with lifecycle_lock(root):
            emit(**export_project(args.project))
        return 0
    except (OSError, ValueError, RuntimeError, sqlite3.Error) as exc:
        emit(type='result', ok=False, message=str(exc))
        return 1


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    sys.stderr.reconfigure(encoding='utf-8')
    sys.exit(main())
