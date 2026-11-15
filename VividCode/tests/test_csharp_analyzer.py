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

    def test_parameters_locals_shadowing_and_member_references(self):
        self.write('App/Variables.cs', 'namespace Sample; class Variables { static int shared=1; static int First(int x){int value=x; {int other=value; shared+=other;} return value+shared;} static int Second(int x){int value=x; return value+shared;} }')
        self.index()
        symbols = {s['id']:s for s in self.export()['units'][0]['symbols']}
        values = [s for s in symbols.values() if s['name']=='value']
        params = [s for s in symbols.values() if s['name']=='x' and s['kind']=='ParmDecl' and s.get('decl_file','').endswith('Variables.cs')]
        self.assertEqual(len(values), 2)
        self.assertEqual(len(params), 2)
        self.assertEqual(len({s['id'] for s in values+params}), 4)
        sites = self.export()['units'][0]['relations']
        for s in values+params:
            self.assertIn(s['parent_id'], symbols)
            self.assertTrue(any(r['target_id']==s['id'] for r in sites), s)

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


class UnityTests(unittest.TestCase):
    write = CSharpTests.write
    run_command = CSharpTests.run_command
    export = CSharpTests.export

    def setUp(self):
        base = BASE / 'build/csharp-test-work'
        base.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix='Unity 中文-', dir=base)
        self.root = Path(self.temp.name) / 'project'
        self.root.mkdir()
        self.env = dict(os.environ, VIVIDCODE_HOME=str(Path(self.temp.name) / 'shared'))
        self.write('.vividcode/project.json', json.dumps({'app': 'vividcode', 'schema': 1}))
        self.write('ProjectSettings/ProjectVersion.txt', 'm_EditorVersion: test-fixture\n')
        self.write('Engine/Engine.csproj', '<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net9.0</TargetFramework></PropertyGroup></Project>')
        self.write('Engine/Engine.cs', 'namespace UnityEngine { public class MonoBehaviour { } }')
        project = str(self.root / 'Engine/Engine.csproj')
        self.run_command(['dotnet', 'build', project, '--ignore-failed-sources'])
        refs = json.loads(self.run_command(['dotnet', 'msbuild', project, '-target:ResolveReferences', '-getItem:ReferencePath', '-verbosity:quiet']).stdout)
        self.references = [r['FullPath'] for r in refs['Items']['ReferencePath']]
        self.references.append(str(self.root / 'Engine/bin/Debug/net9.0/Engine.dll'))
        self.write('Assets/Game.cs', '''using UnityEngine;
public class Player : MonoBehaviour {
    public int Count;
    public int Run() => Pick(Count);
    public int Pick(int x) => x;
    public string Pick(string x) => x;
#if UNITY_EDITOR
    public int EditorOnly() => 1;
#endif
}
''')
        self.write('Assets/Editor/Tool.cs', 'public class Tool { public int Run(Player p) => p.Pick(p.Count); }')
        self.write_project('Assembly-CSharp', 'Assets/Game.cs')
        self.write_project('Assembly-CSharp-Editor', 'Assets/Editor/Tool.cs', 'Assembly-CSharp.csproj')
        self.write('Unity.sln', '\n'.join([
            'Microsoft Visual Studio Solution File, Format Version 12.00',
            'Project("{FAE04EC0-301F-11D3-BF4B-00C04F79EFBC}") = "Game", "Assembly-CSharp.csproj", "{11111111-1111-1111-1111-111111111111}"', 'EndProject',
            'Project("{FAE04EC0-301F-11D3-BF4B-00C04F79EFBC}") = "Editor", "Assembly-CSharp-Editor.csproj", "{22222222-2222-2222-2222-222222222222}"', 'EndProject', 'Global', 'EndGlobal']))
        self.cache = Path(self.env['VIVIDCODE_HOME']) / 'semantic' / hashlib.sha256(self.root.resolve().as_posix().lower().encode()).hexdigest() / 'csharp'

    def tearDown(self):
        self.temp.cleanup()

    def write_project(self, name, source, reference=''):
        from xml.sax.saxutils import escape
        refs = ''.join(f'<Reference Include="{escape(Path(p).stem)}"><HintPath>{escape(p)}</HintPath></Reference>' for p in self.references)
        dep = f'<ProjectReference Include="{reference}" />' if reference else ''
        self.write(name + '.csproj', f'''<Project ToolsVersion="Current">
<PropertyGroup><UnityVersion>test-fixture</UnityVersion><AssemblyName>{name}</AssemblyName><NoStdLib>true</NoStdLib><LangVersion>9.0</LangVersion></PropertyGroup>
<PropertyGroup Condition="'$(Configuration)' == 'Debug'"><DefineConstants>UNITY_EDITOR</DefineConstants></PropertyGroup>
<ItemGroup><Compile Include="{source}" />{refs}{dep}</ItemGroup></Project>''')

    def index(self, config='Debug', ok=True):
        return self.run_command([str(EXE), 'index', '--project', str(self.root), '--project-file', str(self.root / 'Unity.sln'), '--configuration', config], ok)

    def test_unity_semantics_configuration_and_incremental(self):
        self.index()
        data = self.export()
        self.assertEqual(data['state']['project_kind'], 'unity')
        unit = data['units'][0]
        symbols = {s['id']: s for s in unit['symbols']}
        self.assertTrue({'inherits', 'calls', 'member_ref'} <= {r['type'] for r in unit['relations']})
        call = next(r for r in unit['relations'] if r['type'] == 'calls' and r['file'].endswith('/Assets/Editor/Tool.cs') and symbols[r['target_id']]['qualified_name'] == 'Player.Pick')
        self.assertIn('int', symbols[call['target_id']]['signature'])
        self.assertTrue(symbols[call['target_id']]['def_file'].endswith('/Assets/Game.cs'))
        self.assertEqual(symbols[call['target_id']]['def_line'], 5)
        self.index()
        self.assertEqual(self.export()['state']['reused'], 2)
        with (self.root / 'Assets/Editor/Tool.cs').open('a', encoding='utf-8') as f:
            f.write('// editor-only change\n')
        self.index()
        self.assertEqual(self.export()['state']['indexed'], 1)
        self.assertEqual(self.export()['state']['reused'], 1)
        with (self.root / 'Assets/Game.cs').open('a', encoding='utf-8') as f:
            f.write('// runtime dependency change\n')
        self.index()
        self.assertEqual(self.export()['state']['indexed'], 2)
        self.index('Release')
        self.assertFalse(any(s['qualified_name'] == 'Player.EditorOnly' for s in self.export()['units'][0]['symbols']))
        self.index()
        self.assertTrue(any(s['qualified_name'] == 'Player.EditorOnly' for s in self.export()['units'][0]['symbols']))

    def test_unity_missing_input_and_compilation_preserve_snapshot(self):
        self.index()
        before = (self.cache / 'index.json').read_bytes()
        original = (self.root / 'Assets/Game.cs').read_text(encoding='utf-8')
        self.write('Assets/Game.cs', 'invalid C#;')
        self.index(ok=False)
        self.assertEqual(before, (self.cache / 'index.json').read_bytes())
        self.write('Assets/Game.cs', original)
        (self.root / 'Assets/Editor/Tool.cs').unlink()
        result = self.index(ok=False)
        self.assertIn('Unity', result.stdout)
        self.assertEqual(before, (self.cache / 'index.json').read_bytes())
        self.write('Assets/Editor/Tool.cs', 'public class Tool { }')
        (self.root / 'Engine/bin/Debug/net9.0/Engine.dll').unlink()
        self.index(ok=False)
        self.assertEqual(before, (self.cache / 'index.json').read_bytes())

    def test_unity_source_generator_and_evaluated_import(self):
        from xml.sax.saxutils import escape
        roslyn = EXE.parent
        refs = ''.join(f'<Reference Include="{name}"><HintPath>{escape(str(roslyn / (name + ".dll")))}</HintPath></Reference>' for name in ['Microsoft.CodeAnalysis', 'Microsoft.CodeAnalysis.CSharp'])
        self.write('Generator/Generator.csproj', '<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net8.0</TargetFramework></PropertyGroup><ItemGroup>' + refs + '</ItemGroup></Project>')
        self.write('Generator/Gen.cs', '''using Microsoft.CodeAnalysis;
[Generator] public class Gen : ISourceGenerator {
    public void Initialize(GeneratorInitializationContext context) { }
    public void Execute(GeneratorExecutionContext context) {
        context.AddSource("Generated.g.cs", "public static class Generated { public static int Read() => 42; }");
    }
}''')
        self.run_command(['dotnet', 'build', str(self.root / 'Generator/Generator.csproj'), '--ignore-failed-sources'])
        generator = self.root / 'Generator/bin/Debug/net8.0/Generator.dll'
        project = (self.root / 'Assembly-CSharp.csproj').read_text(encoding='utf-8').replace('</Project>', f'<Import Project="options.props" /><ItemGroup><Analyzer Include="{escape(str(generator))}" /></ItemGroup></Project>')
        self.write('Assembly-CSharp.csproj', project)
        self.write('options.props', '<Project><PropertyGroup><DefineConstants>$(DefineConstants);EXTRA_FEATURE</DefineConstants></PropertyGroup></Project>')
        with (self.root / 'Assets/Game.cs').open('a', encoding='utf-8') as f:
            f.write('\n#if EXTRA_FEATURE\npublic class Extra { public int Run() => Generated.Read(); }\n#endif\n')
        self.index()
        unit = self.export()['units'][0]
        names = {s['id']: s['qualified_name'] for s in unit['symbols']}
        self.assertTrue(any(r['type'] == 'calls' and names[r['target_id']] == 'Generated.Read' for r in unit['relations']))
        self.index()
        self.assertEqual(self.export()['state']['reused'], 2)
        self.write('options.props', '<Project />')
        self.index()
        self.assertFalse(any(s['qualified_name'] == 'Extra' for s in self.export()['units'][0]['symbols']))
        before = (self.cache / 'index.json').read_bytes()
        code = (self.root / 'Generator/Gen.cs').read_text(encoding='utf-8')
        self.write('Generator/Gen.cs', code.replace('context.AddSource("Generated.g.cs", "public static class Generated { public static int Read() => 42; }");', 'throw new System.Exception("generator failed");'))
        self.run_command(['dotnet', 'build', str(self.root / 'Generator/Generator.csproj'), '--no-restore'])
        self.index(ok=False)
        self.assertEqual(before, (self.cache / 'index.json').read_bytes())


if __name__ == '__main__':
    unittest.main()
