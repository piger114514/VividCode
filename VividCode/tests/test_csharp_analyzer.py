"""Real Roslyn/MSBuild integration checks; no mocked syntax or relations."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BASE = Path(__file__).resolve().parents[1]
EXE = Path(os.environ.get('VIVIDCODE_CSHARP_ANALYZER', BASE / 'build/Release/csharp-analyzer/vividcode-csharp.exe'))


class CSharpTests(unittest.TestCase):
    def setUp(self):
        base = BASE / 'build/csharp-test-work'
        base.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix='中文 工程-', dir=base)
        self.root = Path(self.temp.name) / 'project'
        self.root.mkdir()
        self.env = dict(os.environ, VIVIDCODE_HOME=str(Path(self.temp.name) / 'shared'))
        (self.root / '.vividcode').mkdir()
        self.write('.vividcode/project.json', json.dumps({'app': 'vividcode', 'schema': 1, 'history': [], 'latest': ''}))
        self.write('Library/Library.csproj', '<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net9.0</TargetFramework></PropertyGroup></Project>')
        self.write('App/App.csproj', '<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net9.0</TargetFramework></PropertyGroup><ItemGroup><ProjectReference Include="../Library/Library.csproj" /></ItemGroup></Project>')
        self.write('Library/Model.cs', '''namespace Sample;
public interface IWorker { int Get(); }
public class Base { public virtual int Get() => 0; }
public partial class Worker : Base, IWorker {
    public int Count;
    public int Value { get; set; }
    public override int Get() => Count + Value;
    public int Pick(int x) => x;
    public string Pick(string x) => x;
}
''')
        self.write('Library/More.cs', 'namespace Sample; public partial class Worker { public int More() => Pick(1); }\n')
        self.write('App/Use.cs', '''namespace Sample;
public class Use {
    public int Run() {
        var w = new Worker();
        w.Count = 2;
        w.Value = w.Pick(4);
        return w.More();
    }
    public int ViaInterface(IWorker w) => w.Get();
#if RELEASE_FEATURE
    public int ReleaseOnly() => 1;
#endif
}
''')
        self.run_command(['dotnet', 'restore', str(self.root / 'App/App.csproj'), '--ignore-failed-sources'])
        self.cache = Path(self.env['VIVIDCODE_HOME']) / 'semantic' / hashlib.sha256(self.root.resolve().as_posix().lower().encode()).hexdigest() / 'csharp'

    def tearDown(self):
        self.temp.cleanup()

    def write(self, path, text):
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding='utf-8')

    def run_command(self, args, ok=True):
        result = subprocess.run(args, env=self.env, capture_output=True, encoding='utf-8', errors='replace', timeout=100)
        if ok:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
        return result

    def index(self, config='Debug', ok=True):
        return self.run_command([str(EXE), 'index', '--project', str(self.root), '--project-file', str(self.root / 'App/App.csproj'), '--configuration', config], ok)

    def export(self):
        return json.loads(self.run_command([str(EXE), 'export', '--project', str(self.root)]).stdout)

    def test_semantic_graph_and_dependency_incremental(self):
        self.index()
        data = self.export()
        self.assertEqual(data['state']['indexed'], 2)
        unit = data['units'][0]  # Whole loaded engineering graph.
        symbols = {s['id']: s for s in unit['symbols']}
        types = {r['type'] for r in unit['relations']}
        self.assertTrue({'calls', 'possible_call', 'inherits', 'implements', 'overrides', 'member_ref', 'references'} <= types)
        worker = next(s for s in symbols.values() if s['qualified_name'] == 'Sample.Worker')
        count = next(s for s in symbols.values() if s['qualified_name'] == 'Sample.Worker.Count')
        self.assertEqual(count['parent_id'], worker['id'])
        pick = [s for s in symbols.values() if s['qualified_name'] == 'Sample.Worker.Pick']
        self.assertEqual(len(pick), 2)
        call = next(r for r in unit['relations'] if r['type'] == 'calls' and symbols[r['target_id']]['qualified_name'] == 'Sample.Worker.Pick' and r['file'].endswith('/App/Use.cs'))
        self.assertEqual(call['line'], 6)
        self.assertIn('int', symbols[call['target_id']]['signature'])
        self.assertTrue(symbols[call['target_id']]['def_file'].endswith('/Library/Model.cs'))
        self.assertEqual(symbols[call['target_id']]['def_line'], 8)
        self.assertTrue(any(r['type'] == 'possible_call' and r['line'] == 9 for r in unit['relations']))
        ids = set(symbols)
        self.index()
        self.assertEqual(self.export()['state']['reused'], 2)
        with (self.root / 'Library/Model.cs').open('a', encoding='utf-8') as stream:
            stream.write('// dependency edit\n')
        self.index()
        self.assertEqual(self.export()['state']['indexed'], 2)
        self.assertEqual(ids, {s['id'] for s in self.export()['units'][0]['symbols']})
        with (self.root / 'App/Use.cs').open('a', encoding='utf-8') as stream:
            stream.write('// app-only edit\n')
        self.index()
        self.assertEqual(self.export()['state']['indexed'], 1)
        self.assertEqual(self.export()['state']['reused'], 1)

    def test_failure_snapshot_and_deletion(self):
        self.index()
        before = (self.cache / 'index.json').read_bytes()
        original = (self.root / 'App/Use.cs').read_text(encoding='utf-8')
        self.write('App/Use.cs', 'this is invalid C#;')
        self.index(ok=False)
        self.assertEqual(before, (self.cache / 'index.json').read_bytes())
        self.assertEqual(self.export()['state']['last_status'], 'failed')
        self.write('App/Use.cs', original.replace('w.More()', 'w.Pick(2)'))
        (self.root / 'Library/More.cs').unlink()
        self.index()
        self.assertFalse(any(s['qualified_name'] == 'Sample.Worker.More' for s in self.export()['units'][0]['symbols']))

    def test_configuration_and_cpp_cache_isolation(self):
        self.index()
        cpp = self.cache.parent / 'index.db'
        cpp.write_bytes(b'untouched C++ database')
        csproj = (self.root / 'App/App.csproj').read_text(encoding='utf-8').replace('</PropertyGroup>', '<DefineConstants Condition="\'$(Configuration)\' == \'Release\'">RELEASE_FEATURE</DefineConstants></PropertyGroup>')
        self.write('App/App.csproj', csproj)
        self.index('Release')
        self.assertTrue(any(s['qualified_name'] == 'Sample.Use.ReleaseOnly' for s in self.export()['units'][0]['symbols']))
        self.index('Debug')
        self.assertFalse(any(s['qualified_name'] == 'Sample.Use.ReleaseOnly' for s in self.export()['units'][0]['symbols']))
        self.assertEqual(cpp.read_bytes(), b'untouched C++ database')

    def test_desktop_projects(self):
        for name, prop, code in [
            ('WinForms', 'UseWindowsForms', 'using System.Windows.Forms; public class MainForm : Form { Button button = new Button(); public void UpdateText() { button.Text = "vividcode"; } }'),
            ('Wpf', 'UseWPF', 'using System.Windows; public class MainWindow : Window { public void UpdateTitle() { Title = "vividcode"; } }'),
        ]:
            self.write(f'{name}/{name}.csproj', f'<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net9.0-windows</TargetFramework><{prop}>true</{prop}></PropertyGroup></Project>')
            self.write(f'{name}/Main.cs', code)
            project = str(self.root / f'{name}/{name}.csproj')
            self.run_command(['dotnet', 'build', project, '-c', 'Release', '--ignore-failed-sources'])
            self.run_command([str(EXE), 'index', '--project', str(self.root), '--project-file', project, '--configuration', 'Release'])
            unit = self.export()['units'][0]
            self.assertTrue(any(r['type'] == 'inherits' for r in unit['relations']))
            self.assertTrue(any(r['type'] == 'member_ref' for r in unit['relations']))

    def test_lock_cancel_and_recovery(self):
        self.index()
        before = (self.cache / 'index.json').read_bytes()
        sys.path.insert(0, str(BASE / 'analyzer'))
        from analyzer import lifecycle_lock
        with lifecycle_lock(self.root):
            self.index(ok=False)
        proc = subprocess.Popen([str(EXE), 'index', '--project', str(self.root), '--project-file', str(self.root / 'App/App.csproj')], env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            self.assertIn(b'progress', proc.stdout.readline())
            self.run_command([str(EXE), 'export', '--project', str(self.root)], ok=False)
            proc.kill()
            proc.communicate(timeout=15)
        finally:
            if proc.poll() is None:
                proc.kill(); proc.communicate(timeout=15)
        self.assertEqual(before, (self.cache / 'index.json').read_bytes())
        self.index()
        self.assertEqual(self.export()['state']['last_status'], 'ok')


if __name__ == '__main__':
    unittest.main()
