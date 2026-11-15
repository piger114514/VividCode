"""Python integration checks in isolated project/cache directories."""
import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'analyzer'))
import python_indexer as indexer


class PythonIndexTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='vividcode Python 中文 ')
        self.root = Path(self.temp.name) / 'project'
        self.root.mkdir()
        self.old_home = os.environ.get('VIVIDCODE_HOME')
        os.environ['VIVIDCODE_HOME'] = str(Path(self.temp.name) / 'shared')
        self.write('.vividcode/project.json', json.dumps(dict(app='vividcode', schema=1)))

    def tearDown(self):
        if self.old_home is None:
            os.environ.pop('VIVIDCODE_HOME', None)
        else:
            os.environ['VIVIDCODE_HOME'] = self.old_home
        self.temp.cleanup()

    def write(self, file, text, encoding='utf-8'):
        path = self.root / file
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding=encoding)
        return path

    def run_index(self):
        with contextlib.redirect_stdout(io.StringIO()):
            return indexer.index_project(str(self.root))

    def data(self):
        return indexer.export_project(str(self.root))

    def edges(self):
        symbols = {s['id']: s for u in self.data()['units'] for s in u['symbols']}
        return [(symbols[r['source_id']]['qualified_name'], symbols[r['target_id']]['qualified_name'], r)
                for u in self.data()['units'] for r in u['relations']]

    def test_variables_parameters_imports_and_lexical_identity(self):
        self.write('api.py', 'shared=1\ndef first(x):\n    value=x\n    def nested():\n        nonlocal value\n        return value\n    return value+shared\nclass C:\n    field=2\n    def run(self,p):\n        self.count=p\n        return self.count+self.field\n')
        self.write('use.py', 'from api import shared as other\nimport api\ndef second(x):\n    value=x\n    return value+other+api.shared\n')
        self.assertEqual(self.run_index(), 0)
        symbols = {s['id']: s for u in self.data()['units'] for s in u['symbols']}
        values = [s for s in symbols.values() if s['name'] == 'value']
        self.assertEqual(len(values), 2)
        self.assertEqual({s['qualified_name'] for s in values}, {'first.value', 'second.value'})
        params = [s for s in symbols.values() if s['kind'] == 'ParmDecl']
        self.assertEqual({s['qualified_name'] for s in params}, {'first.x', 'second.x', 'C.run.self', 'C.run.p'})
        for a, b in [('first', 'first.x'), ('first.nested', 'first.value'), ('second', 'shared'), ('C.run', 'C.count')]:
            self.assertTrue(any(x == a and y == b for x,y,r in self.edges()), (a,b,self.edges()))
        shared = next(s['id'] for s in symbols.values() if s['qualified_name'] == 'shared')
        sites = [r for u in self.data()['units'] for r in u['relations'] if r['target_id'] == shared]
        self.assertEqual({r['file'] for r in sites}, {'api.py', 'use.py'})

    def test_lambda_comprehension_and_global_variable_scope(self):
        self.write('scope.py', 'x=10\ndef f(x):\n    values=[x for x in range(x)]\n    call=lambda x: x+1\n    return x,values,call\ndef g():\n    global created\n    created=2\n    return created\n')
        self.assertEqual(self.run_index(), 0)
        symbols = {s['id']:s for u in self.data()['units'] for s in u['symbols']}
        xs = [s for s in symbols.values() if s['name']=='x']
        self.assertEqual(len(xs), 4)
        self.assertEqual(len({s['parent_id'] for s in xs}), 4)
        self.assertTrue(any(s['qualified_name']=='created' for s in symbols.values()))
        edges = self.edges()
        self.assertTrue(any(a=='g' and b=='created' for a,b,r in edges))
        self.assertTrue(any(a=='f' and b=='f.x' for a,b,r in edges))
        self.assertFalse(any(a=='f' and b=='x' for a,b,r in edges))

    def test_cross_file_class_inheritance_members_and_async(self):
        self.write('pkg/__init__.py', 'from .api import Worker, helper\n')
        self.write('pkg/api.py', 'def helper(x):\n    return x * 2\nclass Base:\n    def inherited(self):\n        return 1\nclass Worker(Base):\n    def __init__(self):\n        self.count = 2\n    async def run(self):\n        return helper(self.count) + self.inherited()\n')
        self.write('use.pyw', 'from pkg import Worker as W, helper as h\nasync def caller():\n    w = W()\n    return h(2), await w.run()\n')
        self.assertEqual(self.run_index(), 0)
        for source, target, kind in [('Worker', 'Base', 'inherits'), ('Worker.run', 'helper', 'calls'),
                                      ('Worker.run', 'Worker.count', 'member_ref'),
                                      ('Worker.run', 'Base.inherited', 'possible_call'),
                                      ('caller', 'Worker.run', 'possible_call'), ('caller', 'helper', 'calls')]:
            self.assertTrue(any(a == source and b == target and r['type'] == kind for a, b, r in self.edges()), (source, target, self.edges()))
        self.assertTrue(all(r['line'] > 0 and r['col'] > 0 for _, _, r in self.edges()))

    def test_incremental_dependency_reresolution_failure_deletion(self):
        self.write('api.py', 'def f():\n    return 1\n')
        self.write('use.py', 'from api import f\ndef caller():\n    return f()\n')
        self.assertEqual(self.run_index(), 0)
        self.assertEqual(self.run_index(), 0)
        self.assertEqual(self.data()['state']['reused'], 2)
        self.write('api.py', 'def changed():\n    return 1\n')
        self.assertEqual(self.run_index(), 0)
        self.assertEqual(self.data()['state']['indexed'], 1)
        self.assertTrue(any(a == 'caller' and b.startswith('Unresolved: ') for a, b, _ in self.edges()))
        before = self.data()['units']
        self.write('api.py', 'class syntax error:\n')
        self.assertEqual(self.run_index(), 1)
        self.assertEqual(self.data()['units'], before)
        self.assertEqual(self.data()['state']['last_status'], 'failed')
        (self.root / 'api.py').unlink()
        self.assertEqual(self.run_index(), 0)
        self.assertEqual(len(self.data()['units']), 1)

    def test_shadowing_rebinding_dynamic_and_duplicate_names(self):
        self.write('a.py', 'def f(): pass\ndef local(f):\n    f()\ndef rebound():\n    x = C()\n    x = other()\n    x.run()\nclass C:\n    def run(self): pass\ndef outer():\n    def f(): pass\n    f()\ndef dynamic(obj):\n    getattr(obj, "run")()\n')
        self.write('b.py', 'def f(): pass\n')
        self.assertEqual(self.run_index(), 0)
        for source, target in [('local', 'Unresolved: f'), ('rebound', 'Unresolved: x.run'), ('outer', 'outer.f')]:
            self.assertTrue(any(a == source and b == target for a, b, _ in self.edges()))
        self.assertTrue(any(a == 'dynamic' and r['type'] == 'possible_call' for a, b, r in self.edges()))

    def test_src_namespace_package_and_stub_precedence(self):
        self.write('src/pkg/tool.py', 'def f(): return 1\n')
        self.write('src/pkg/tool.pyi', 'def f() -> int: ...\n')
        self.write('main.py', 'import pkg.tool\ndef caller():\n    return pkg.tool.f()\n')
        self.assertEqual(self.run_index(), 0)
        symbols = {s['id']: s for u in self.data()['units'] for s in u['symbols']}
        matching = [r for u in self.data()['units'] for r in u['relations'] if r['type'] == 'calls' and symbols[r['source_id']]['name'] == 'caller']
        self.assertEqual(len(matching), 1)
        self.assertEqual(symbols[matching[0]['target_id']]['def_file'], 'src/pkg/tool.py')

    def test_alias_annotations_class_scope_and_capture_shadowing(self):
        self.write('api.py', 'class A:\n    def run(self): pass\ndef f(): pass\n')
        self.write('use.py', 'import api as m\nfrom api import f\ndef typed(x: m.A):\n    x.run()\nclass C:\n    def f(self): pass\n    def bare(self):\n        f()\ndef handler():\n    try: pass\n    except Exception as f: f()\ndef pattern(value):\n    match value:\n        case {"f": f}: f()\n')
        self.assertEqual(self.run_index(), 0)
        for source, target, kind in [('typed', 'A.run', 'possible_call'), ('C.bare', 'f', 'calls'),
                                      ('handler', 'Unresolved: f', 'possible_call'), ('pattern', 'Unresolved: f', 'possible_call')]:
            self.assertTrue(any(a == source and b == target and r['type'] == kind for a, b, r in self.edges()), (source, target))

    def test_circular_reexports_ambiguous_modules_and_lifecycle_lock(self):
        self.write('a.py', 'from b import f\ndef caller(): f()\n')
        self.write('b.py', 'from a import f\n')
        self.write('pkg/api.py', 'def f(): pass\n')
        self.write('src/pkg/api.py', 'def f(): pass\n')
        self.write('use.py', 'from pkg.api import f\ndef caller(): f()\n')
        self.assertEqual(self.run_index(), 0)
        self.assertFalse(any(b == 'f' for _, b, _ in self.edges()))
        before = self.data()['units']
        with indexer.lifecycle_lock(self.root):
            with self.assertRaises(RuntimeError):
                self.run_index()
        self.assertEqual(self.data()['units'], before)

    def test_cancelled_process_preserves_snapshot_and_recovers(self):
        self.write('old.py', 'def old(): pass\n')
        self.assertEqual(self.run_index(), 0)
        before = self.data()['units']
        # First sorted file takes long enough to terminate after its progress event.
        self.write('aaa.py', '\n'.join(f'def f{i}(): return {i}' for i in range(15000)))
        command = [sys.executable, str(Path(indexer.__file__)), 'index', '--project', str(self.root)]
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding='utf-8')
        try:
            event = json.loads(process.stdout.readline())
            self.assertEqual(event['type'], 'progress')
            process.terminate()
            process.communicate(timeout=10)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()
        self.assertEqual(self.data()['units'], before)
        self.assertEqual(self.data()['state']['last_status'], 'interrupted')
        (self.root / 'aaa.py').unlink()
        self.assertEqual(self.run_index(), 0)
        self.assertEqual(self.data()['state']['last_status'], 'ok')

    def test_encoding_unicode_columns_and_no_project_execution(self):
        self.write('中文.py', '# coding: gbk\ndef 工作():\n    return 1\ndef caller():\n    文本 = "你好"; 工作()\n', 'gbk')
        self.write('danger.py', 'raise RuntimeError("must not execute")\ndef f(): pass\n')
        self.assertEqual(self.run_index(), 0)
        edge = next(r for a, b, r in self.edges() if a == 'caller' and b == '工作')
        self.assertEqual(edge['col'], '    文本 = "你好"; 工作()'.index('工作') + 1)
        result = subprocess.run([sys.executable, str(Path(indexer.__file__)), 'source', '--project', str(self.root), '--file', '中文.py'], capture_output=True, encoding='utf-8', timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('你好', json.loads(result.stdout)['text'])
        (self.root / 'invalid.py').write_bytes(b'# coding: utf-8\n\xff')
        before = self.data()['units']
        self.assertEqual(self.run_index(), 1)
        self.assertEqual(self.data()['units'], before)

    def test_exclusions_large_files_ownership_and_interrupted_snapshot(self):
        self.write('main.PY', 'def f(): pass\n')
        for directory in ('.venv', '__pycache__', 'build', '.vividcode'):
            self.write(directory + '/bad.py', 'bad syntax ???')
        self.assertEqual(self.run_index(), 0)
        self.assertEqual(self.data()['state']['total'], 1)
        cache = indexer.cache_paths(self.root)[2]
        before = self.data()['units']
        state = json.loads((cache / 'config.json').read_text(encoding='utf-8'))
        state['last_status'] = 'running'
        (cache / 'config.json').write_text(json.dumps(state))
        self.assertEqual(self.data()['state']['last_status'], 'interrupted')
        self.assertEqual(self.data()['units'], before)
        self.write('huge.py', ' ' * (indexer.MAX_BYTES + 1))
        self.assertEqual(self.run_index(), 1)
        self.assertEqual(self.data()['units'], before)
        (cache.parent / 'project.json').write_text(json.dumps(dict(app='unknown', schema=1)))
        with self.assertRaises(ValueError):
            self.run_index()


    def test_global_limit_and_forced_large_source(self):
        self.write('a.py', 'def a(): pass\n')
        self.write('b.py', 'def b(): pass\n')
        settings = Path(os.environ['VIVIDCODE_HOME']) / 'read-policy.json'
        settings.parent.mkdir(parents=True, exist_ok=True)
        settings.write_text(json.dumps(dict(app='vividcode', schema=1, maxSourceFiles=1)))
        self.assertEqual(self.run_index(), 1)
        self.write('.vividcode/project.json', json.dumps(dict(app='vividcode', schema=1, forceRead=True)))
        self.write('a.py', '#' + 'x' * (indexer.MAX_BYTES + 1) + '\ndef a(): pass\n')
        self.write('build/ignored.py', 'invalid syntax ???')
        self.write('.vividcode-work-temporary/ignored.py', 'invalid syntax ???')
        self.assertEqual(self.run_index(), 0)
        self.assertEqual(self.data()['state']['total'], 2)
        before = self.data()['units']
        self.write('.vividcode/project.json', json.dumps(dict(app='vividcode', schema=1, forceRead=False)))
        settings.write_text(json.dumps(dict(app='vividcode', schema=1, maxSourceFiles=3)))
        self.assertEqual(self.run_index(), 1)
        self.assertEqual(self.data()['units'], before)
        self.write('a.py', 'def a(): pass\n')
        self.assertEqual(self.run_index(), 0)
        settings.write_text(json.dumps(dict(app='vividcode', schema=1, maxSourceFiles=1)))
        self.assertEqual(self.run_index(), 1)

if __name__ == '__main__':
    unittest.main()
