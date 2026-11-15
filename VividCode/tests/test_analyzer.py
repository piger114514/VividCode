"""Real libclang integration checks; all project data lives in temporary dirs."""
import contextlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'analyzer'))
import analyzer


class IndexTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='vividcode 中文 ')
        self.root = Path(self.temp.name) / 'project'
        self.root.mkdir()
        self.old_home = os.environ.get('VIVIDCODE_HOME')
        os.environ['VIVIDCODE_HOME'] = str(Path(self.temp.name) / 'shared home')
        (self.root / '.vividcode').mkdir()
        (self.root / '.vividcode/project.json').write_text(json.dumps({'app': 'vividcode', 'schema': 1}))
        (self.root / 'api.hpp').write_text('''#pragma once
struct Base { virtual int value() { return 0; } };
struct Child : Base { int member; int value() override; };
int twice(int); double twice(double);
''')
        (self.root / 'a.cpp').write_text('''#include "api.hpp"
int Child::value() { return member; }
int twice(int x) { return x * 2; }
double twice(double x) { return x * 2; }
''')
        (self.root / 'b.cpp').write_text('''#include "api.hpp"
int caller(Child &c) { return twice(3) + c.value(); }
''')
        self.entries = [{'directory': str(self.root), 'file': f, 'arguments': ['clang++', '-std=c++17', '-I.', '-c', f, '-o', f + '.obj']} for f in ['a.cpp', 'b.cpp']]
        self.compilation = self.root / 'compile_commands.json'
        self.save_commands()

    def tearDown(self):
        if self.old_home is None:
            os.environ.pop('VIVIDCODE_HOME', None)
        else:
            os.environ['VIVIDCODE_HOME'] = self.old_home
        self.temp.cleanup()

    def save_commands(self):
        self.compilation.write_text(json.dumps(self.entries), encoding='utf-8')

    def run_index(self):
        with contextlib.redirect_stdout(io.StringIO()):
            return analyzer.index_project(str(self.root), str(self.compilation))

    def exported(self):
        return analyzer.export_project(str(self.root))

    def test_variable_parameters_shadowing_and_cross_file_references(self):
        (self.root / 'api.hpp').write_text('extern int shared;\nint first(int x);\n')
        (self.root / 'a.cpp').write_text('#include "api.hpp"\nint shared=1;\nint first(int x){int value=x; {int value=2; shared+=value;} return value+shared;}\n')
        (self.root / 'b.cpp').write_text('#include "api.hpp"\nint second(int x){int value=x; return value+shared+first(x);}\n')
        self.assertEqual(self.run_index(), 0)
        data = self.exported()
        symbols = {s['id']: s for u in data['units'] for s in u['symbols']}
        sites = [r for u in data['units'] for r in u['relations']]
        values = [s for s in symbols.values() if s['name'] == 'value']
        self.assertEqual(len(values), 3)
        self.assertEqual(len({s['id'] for s in values}), 3)
        params = [s for s in symbols.values() if s['name'] == 'x' and s['kind'] == 'ParmDecl']
        self.assertGreaterEqual(len(params), 2)
        for variable in values + params:
            self.assertIn(variable['parent_id'], symbols)
        shared = next(s['id'] for s in symbols.values() if s['name'] == 'shared')
        self.assertEqual({r['file'] for r in sites if r['target_id'] == shared}, {'a.cpp', 'b.cpp'})
        self.assertTrue(any(r['target_id'] in {s['id'] for s in params} for r in sites))

    def test_semantics_incremental_failure_and_deletion(self):
        self.assertEqual(self.run_index(), 0)
        data = self.exported()
        symbols = [s for u in data['units'] for s in u['symbols']]
        relations = [r for u in data['units'] for r in u['relations']]
        overloads = {s['id'] for s in symbols if s['name'] == 'twice'}
        self.assertEqual(len(overloads), 2)
        kinds = {r['type'] for r in relations}
        self.assertTrue({'inherits', 'calls', 'member_ref', 'possible_call', 'overrides'} <= kinds, kinds)
        self.assertTrue(all(r['line'] > 0 for r in relations))
        ids = {s['id'] for s in symbols}
        self.assertTrue(all(r['source_id'] in ids and r['target_id'] in ids for r in relations))
        self.assertEqual(self.run_index(), 0)
        self.assertEqual(self.exported()['state']['reused'], 2)
        with (self.root / 'api.hpp').open('a') as f:
            f.write('\nstruct NewClass {};\n')
        self.assertEqual(self.run_index(), 0)
        self.assertEqual(self.exported()['state']['indexed'], 2)
        before = self.exported()['units']
        (self.root / 'b.cpp').write_text('#include "missing.hpp"\n')
        self.assertEqual(self.run_index(), 1)
        self.assertEqual(self.exported()['units'], before)
        self.assertEqual(self.exported()['state']['last_status'], 'failed')
        self.entries = self.entries[:1]
        self.save_commands()
        self.assertEqual(self.run_index(), 0)
        self.assertEqual(len(self.exported()['units']), 1)
        self.assertFalse(any(s['name'] == 'caller' for s in self.exported()['units'][0]['symbols']))

    def test_windows_command_forced_include_and_config_isolation(self):
        import subprocess
        self.entries[0].pop('arguments')
        self.entries[0]['command'] = subprocess.list2cmdline(['clang-cl.exe', '/std:c++17', '/I.', '/FIapi.hpp', '/DTEST=1', '/c', 'a.cpp', '/Foa.obj'])
        self.entries.append({'directory': str(self.root), 'file': 'a.cpp', 'arguments': ['clang-cl.exe', '/std:c++17', '/DTEST=2', '/c', 'a.cpp']})
        self.save_commands()
        self.assertEqual(self.run_index(), 0)
        self.assertEqual(len(self.exported()['units']), 3)
        self.entries.reverse()
        self.save_commands()
        self.assertEqual(self.run_index(), 0)
        self.assertEqual(self.exported()['state']['reused'], 3)

    def test_bad_database_and_lock_preserve_snapshot(self):
        self.assertEqual(self.run_index(), 0)
        before = self.exported()['units']
        self.compilation.write_text('{}')
        self.assertEqual(self.run_index(), 1)
        self.assertEqual(self.exported()['units'], before)
        with analyzer.lifecycle_lock(self.root):
            with self.assertRaises(RuntimeError):
                self.run_index()
        self.save_commands()
        self.assertEqual(self.run_index(), 0)

    def test_gcc_forced_include_is_preserved(self):
        (self.root / 'forced.hpp').write_text('#define MAGIC 7\n')
        (self.root / 'a.cpp').write_text('int magic() { return MAGIC; }')
        self.entries = [{'directory': str(self.root), 'file': 'a.cpp', 'arguments': ['clang++', '-include', 'forced.hpp', 'a.cpp']}]
        self.save_commands()
        self.assertEqual(self.run_index(), 0)
        self.assertTrue(any(s['name'] == 'magic' for s in self.exported()['units'][0]['symbols']))

    def test_cancelled_worker_keeps_snapshot_and_releases_lock(self):
        import subprocess
        self.assertEqual(self.run_index(), 0)
        before = self.exported()['units']
        (self.root / 'a.cpp').write_text('\n'.join(f'struct Large{i} {{ int x; }};' for i in range(12000)))
        proc = subprocess.Popen([sys.executable, str(Path(analyzer.__file__)), 'index', '--project', str(self.root), '--compile-commands', str(self.compilation)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            self.assertEqual(json.loads(proc.stdout.readline())['type'], 'progress')
            proc.kill(); proc.wait(timeout=10)
        finally:
            if proc.poll() is None: proc.kill(); proc.wait()
            proc.stdout.close(); proc.stderr.close()
        self.assertEqual(self.exported()['units'], before)
        self.assertEqual(self.exported()['state']['last_status'], 'running')
        with analyzer.lifecycle_lock(self.root):
            pass

    def test_new_shadow_header_invalidates_cache(self):
        (self.root / 'first').mkdir(); (self.root / 'second').mkdir()
        (self.root / 'second/choice.hpp').write_text('struct Second {};')
        (self.root / 'a.cpp').write_text('#include <choice.hpp>')
        self.entries = [{'directory': str(self.root), 'file':'a.cpp', 'arguments':['clang++','-Ifirst','-Isecond','a.cpp']}]
        self.save_commands(); self.assertEqual(self.run_index(), 0)
        (self.root / 'first/choice.hpp').write_text('struct First {};')
        self.assertEqual(self.run_index(), 0)
        self.assertEqual(self.exported()['state']['indexed'], 1)
        self.assertTrue(any(s['name'] == 'First' for s in self.exported()['units'][0]['symbols']))

    def test_shared_cache_isolated_by_project(self):
        self.assertEqual(self.run_index(), 0)
        before = self.exported()['units']
        other = self.root.parent / 'second project'
        other.mkdir()
        (other / '.vividcode').mkdir()
        (other / '.vividcode/project.json').write_text(json.dumps({'app':'vividcode','schema':1}))
        (other / 'second.cpp').write_text('struct AnotherProject {};')
        commands = other / 'compile_commands.json'
        commands.write_text(json.dumps([{'directory':str(other),'file':'second.cpp','arguments':['clang++','second.cpp']}]))
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(analyzer.index_project(other, commands), 0)
        self.assertNotEqual(analyzer.semantic_directory(other), analyzer.semantic_directory(self.root))
        self.assertEqual(self.exported()['units'], before)
        self.assertTrue(any(s['name']=='AnotherProject' for u in analyzer.export_project(other)['units'] for s in u['symbols']))
        self.assertFalse((self.root / '.vividcode/semantic').exists())
        self.assertFalse((other / '.vividcode/semantic').exists())
        owner = analyzer.semantic_directory(self.root) / 'project.json'
        owner.write_text(json.dumps({'app':'vividcode','projectRoot':str(other)}))
        with self.assertRaises(ValueError):
            self.exported()


if __name__ == '__main__':
    unittest.main()
