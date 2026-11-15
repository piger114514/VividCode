"""Real javac semantic/lifecycle integration; all projects and caches are disposable."""
import ctypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest
import winreg

RUNTIME = Path(os.environ.get('VIVIDCODE_JAVA_RUNTIME', Path(__file__).resolve().parents[1] / 'build/Release'))


class JavaTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='vividcode Java 中文 ')
        self.root = Path(self.temp.name) / 'project'; self.root.mkdir()
        self.home = Path(self.temp.name) / 'shared'
        self.env = {**os.environ, 'VIVIDCODE_HOME': str(self.home)}
        shutil.copytree(RUNTIME/'java-analyzer', self.home/'runtime/java-analyzer')
        self.write('.vividcode/project.json', json.dumps(dict(app='vividcode', schema=1)))

    def tearDown(self):
        self.temp.cleanup()

    def write(self, name, text, encoding='utf-8'):
        p = self.root/name; p.parent.mkdir(parents=True, exist_ok=True); p.write_text(text, encoding=encoding); return p

    def command(self, action='index'):
        return [str(RUNTIME/'vividcode.exe'), '--java-analyzer', action, '--project', str(self.root)]

    def index(self, expected=0):
        p = subprocess.run(self.command(), env=self.env, capture_output=True, encoding='utf-8', timeout=190)
        self.assertEqual(p.returncode, expected, p.stdout+p.stderr)
        return p

    def data(self):
        p = subprocess.run(self.command('export'), env=self.env, capture_output=True, encoding='utf-8', timeout=15)
        self.assertEqual(p.returncode, 0, p.stdout+p.stderr)
        return json.loads(p.stdout)

    def cache(self):
        return next((self.home/'semantic').glob('*/java'))

    def jdk(self):
        if self.env.get('VIVIDCODE_JAVA'): return Path(self.env['VIVIDCODE_JAVA']).parent
        if self.env.get('JAVA_HOME'): return Path(self.env['JAVA_HOME'])/'bin'
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,r'SOFTWARE\JavaSoft\JDK') as key:
            version=winreg.QueryValueEx(key,'CurrentVersion')[0]
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,'SOFTWARE\\JavaSoft\\JDK\\'+version) as key:
            return Path(winreg.QueryValueEx(key,'JavaHome')[0])/'bin'

    def graph(self):
        data=self.data(); symbols={s['id']:s for u in data['units'] for s in u['symbols']}
        edges=[(symbols[r['source_id']],symbols[r['target_id']],r) for u in data['units'] for r in u['relations']]
        return symbols, edges

    def edge(self, a, b, kind):
        edges=self.graph()[1]
        result=[r for x,y,r in edges if x['qualified_name']==a and y['qualified_name']==b and r['type']==kind]
        self.assertTrue(result, (a,b,kind,[(x['qualified_name'],y['qualified_name'],r['type']) for x,y,r in edges]))
        return result[0]

    def test_parameters_locals_blocks_and_cross_file_field_references(self):
        self.write('Api.java', 'class Api {static int shared=1; static int first(int x){int value=x; {int other=value; shared+=other;} return value+shared;} static int second(int x){int value=x; return value+shared;}}')
        self.write('Use.java', 'class Use {int run(int x){return Api.shared+Api.first(x);}}')
        self.index()
        symbols, edges = self.graph()
        values = [s for s in symbols.values() if s['name']=='value']
        params = [s for s in symbols.values() if s['kind']=='ParmDecl' and s['name']=='x']
        self.assertEqual(len(values), 2)
        self.assertEqual(len(params), 3)
        self.assertEqual(len({s['id'] for s in values+params}), 5)
        for s in values+params:
            self.assertIn(s['parent_id'], symbols)
            self.assertTrue(any(y['id']==s['id'] for x,y,r in edges), s)
        shared = next(s['id'] for s in symbols.values() if s['name']=='shared')
        self.assertEqual({r['file'] for x,y,r in edges if y['id']==shared}, {'Api.java','Use.java'})

    def test_cross_file_overloads_generics_dispatch_override_and_references(self):
        self.write('src/demo/Api.java', '''package demo;
interface I<T> { T run(T x); }
class Base { public String run(String x) { return x; } }
class Worker extends Base implements I<String> {
 int field=3;
 @Override public String run(String x) { return Helper.work(x)+field; }
}
class Helper { static String work(String x){return x;} static int work(int x){return x;} }
''')
        self.write('src/demo/Use.java', '''package demo;
class Use {
 static void caller(I<String> worker){ Helper.work(2); worker.run("x"); new Worker().run("x"); java.util.function.Function<String,String> f=Helper::work; }
}
''')
        self.index(); self.edge('demo.Worker','demo.Base','inherits'); self.edge('demo.Worker','demo.I','implements')
        self.edge('demo.Worker.run','demo.I.run','overrides'); self.edge('demo.Worker.run','demo.Base.run','overrides')
        self.edge('demo.Worker.run','demo.Worker.field','member_ref'); self.edge('demo.Use.caller','demo.I.run','possible_call')
        self.edge('demo.Use.caller','demo.Helper.work','references')
        targets=[b['id'] for a,b,r in self.graph()[1] if a['qualified_name']=='demo.Use.caller' and b['qualified_name']=='demo.Helper.work' and r['type']=='calls']
        self.assertEqual(len(targets),1); self.assertIn('(int)',targets[0])
        self.assertIn('java.lang.String',self.edge('demo.Worker.run','demo.Helper.work','calls')['target_id'])
        self.assertFalse(any(r['type']=='calls' and b['qualified_name']=='demo.I.run' for a,b,r in self.graph()[1]))

    def test_config_source_roots_release_utf16_and_columns(self):
        self.write('vividcode.java.json',json.dumps(dict(schema=1,sourceRoots=['src'],release='17')))
        line='\tstatic int caller(){ String 中文="😀"; return target(); }'
        self.write('src/Use.java','class Use {\n static int target(){return 1;}\n'+line+'\n}\n','utf-16')
        self.write('excluded/Broken.java','class Broken { syntax ???')
        self.index(); site=self.edge('Use.caller','Use.target','calls')
        self.assertEqual(site['line'],3); self.assertEqual(site['col'],len(line[:line.index('target()')].encode('utf-16-le'))//2+1)
        self.assertEqual(self.data()['state']['total'],1)
        self.write('src/Use.java','record Use(int x) {}'); self.index()
        self.write('vividcode.java.json',json.dumps(dict(schema=1,sourceRoots=['src'],release='11'))); self.index(1)

    def test_cache_change_failure_preserves_complete_snapshot_and_final_deletion(self):
        p=self.write('A.java','class A {static int helper(){return 1;} int caller(){return helper();}}')
        self.index(); self.index(); self.assertEqual(self.data()['state']['reused'],1)
        previous=self.data()['units']; self.write('A.java','class A { int x( {'); self.index(1)
        self.assertEqual(self.data()['units'],previous); self.assertEqual(self.data()['state']['last_status'],'failed')
        self.write('A.java','class A {static int changed(){return 1;} int caller(){return changed();}}'); self.index()
        self.edge('A.caller','A.changed','calls'); self.assertEqual(self.data()['state']['reused'],0)
        p.unlink(); self.index(); self.assertEqual(self.data()['units'],[])

    def test_missing_dependency_does_not_bind_unrelated_same_name(self):
        self.write('A.java','class A {static void run(){} void caller(){ Missing.run(); }}')
        self.index(1); self.assertEqual(self.data()['units'],[])
        self.assertIn('Missing',self.data()['state']['message'])

    def test_external_class_directory_jar_and_dependency_invalidation(self):
        javac=self.jdk()/'javac.exe'; jar=self.jdk()/'jar.exe'
        dep=Path(self.temp.name)/'dep'; dep.mkdir(); source=dep/'Lib.java'; classes=dep/'classes'; classes.mkdir()
        source.write_text('package lib; public class Lib { public static int helper(){return 1;} }')
        subprocess.run([str(javac),'-d',str(classes),str(source)],check=True,capture_output=True)
        self.write('vividcode.java.json',json.dumps(dict(schema=1,classpath=[str(classes)])))
        self.write('Use.java','class Use {int caller(){return lib.Lib.helper();}}'); self.index()
        self.edge('Use.caller','lib.Lib.helper','calls'); self.assertTrue(any(b['is_external'] for a,b,r in self.graph()[1] if b['qualified_name']=='lib.Lib.helper'))
        dependency=dep/'api.jar'; subprocess.run([str(jar),'--create','--file',str(dependency),'-C',str(classes),'.'],check=True,capture_output=True)
        self.write('vividcode.java.json',json.dumps(dict(schema=1,classpath=[str(dependency)]))); self.index(); self.index()
        self.assertEqual(self.data()['state']['reused'],1); before=self.data()['units']
        dependency.write_bytes(b'broken jar'); self.index(1); self.assertEqual(self.data()['units'],before)

    def test_named_module_path_and_dependency_changes(self):
        dep=Path(self.temp.name)/'module'; (dep/'lib').mkdir(parents=True)
        (dep/'module-info.java').write_text('module library.api { exports lib; }')
        (dep/'lib/Api.java').write_text('package lib; public class Api {public static int work(){return 2;}}')
        classes=dep/'classes'
        subprocess.run([str(self.jdk()/'javac.exe'),'-d',str(classes),str(dep/'module-info.java'),str(dep/'lib/Api.java')],check=True,capture_output=True)
        self.write('vividcode.java.json',json.dumps(dict(schema=1,sourceRoots=['src'],modulePath=[str(classes)],release='17')))
        # The dependency class was compiled by the local JDK: use its matching release.
        subprocess.run([str(self.jdk()/'javac.exe'),'--release','17','-d',str(classes),str(dep/'module-info.java'),str(dep/'lib/Api.java')],check=True,capture_output=True)
        self.write('src/module-info.java','module app.main {requires library.api;}')
        self.write('src/app/Use.java','package app; class Use {int caller(){return lib.Api.work();}}')
        self.index(); self.edge('app.Use.caller','lib.Api.work','calls')
        (classes/'lib/Api.class').unlink(); self.index(1)

    def test_annotation_processor_is_never_loaded(self):
        dep=Path(self.temp.name)/'processor'; dep.mkdir(); marker=dep/'EXECUTED'
        source=dep/'Processor.java'; classes=dep/'classes'
        source.write_text('''import javax.annotation.processing.*; import javax.lang.model.*; import javax.lang.model.element.*; import java.util.*;
@SupportedAnnotationTypes("*") public class Processor extends AbstractProcessor {
public SourceVersion getSupportedSourceVersion(){return SourceVersion.latestSupported();}
public boolean process(Set<? extends TypeElement> annotations, RoundEnvironment round){try {new java.io.File("'''+marker.as_posix()+'''").createNewFile();}catch(Exception e){} return false;}}
''', encoding='utf-8')
        subprocess.run([str(self.jdk()/'javac.exe'),'-d',str(classes),str(source)],check=True,capture_output=True)
        services=classes/'META-INF/services'; services.mkdir(parents=True)
        (services/'javax.annotation.processing.Processor').write_text('Processor')
        jar=dep/'processor.jar'; subprocess.run([str(self.jdk()/'jar.exe'),'--create','--file',str(jar),'-C',str(classes),'.'],check=True,capture_output=True)
        self.write('vividcode.java.json',json.dumps(dict(schema=1,classpath=[str(jar)])))
        self.write('A.java','class A { static void work(){} }'); self.index(); self.assertFalse(marker.exists())

    def test_no_source_execution_no_class_outputs_and_environment_failure(self):
        marker=self.root/'EXECUTED'
        self.write('A.java','class A {static {try {new java.io.File("EXECUTED").createNewFile();}catch(Exception e){}} static void caller(){System.out.println("hi");}}')
        self.index(); self.assertFalse(marker.exists()); self.assertEqual(list(self.root.rglob('*.class')),[])
        old=self.data()['units']; self.env['VIVIDCODE_JAVA']=str(self.root/'missing/java.exe'); self.index(1)
        self.assertEqual(self.data()['units'],old)

    def test_global_limits_force_exclusions_and_config_validation(self):
        self.write('A.java','class A {}'); self.index(); previous=self.data()['units']
        self.home.mkdir(exist_ok=True); (self.home/'read-policy.json').write_text(json.dumps(dict(app='vividcode',schema=1,maxSourceFiles=1)))
        self.write('B.java','class B {}'); self.index(1); self.assertEqual(self.data()['units'],previous)
        self.write('.vividcode/project.json',json.dumps(dict(app='vividcode',schema=1,forceRead=True)))
        self.write('build/Bad.java','not Java'); self.write('A.java','class A {} /*'+'x'*(2*1024*1024)+'*/'); self.index()
        self.assertEqual(self.data()['state']['total'],2)
        self.write('vividcode.java.json','{"schema":1,"sourceRoots":["../dep"]}'); self.index(1)

    def test_lifecycle_lock_and_interrupted_state(self):
        self.write('A.java','class A {}'); self.index(); before=self.data()['units']
        lock=str(self.root/'.vividcode-semantic.lock')
        kernel=ctypes.WinDLL('kernel32',use_last_error=True)
        kernel.CreateFileW.restype=ctypes.c_void_p; kernel.CreateFileW.argtypes=[ctypes.c_wchar_p,ctypes.c_uint32,ctypes.c_uint32,ctypes.c_void_p,ctypes.c_uint32,ctypes.c_uint32,ctypes.c_void_p]
        kernel.CloseHandle.argtypes=[ctypes.c_void_p]
        handle=kernel.CreateFileW(lock,0xC0010000,0,None,4,0x04000000,None)
        self.assertNotEqual(handle,ctypes.c_void_p(-1).value)
        try: self.index(1)
        finally: kernel.CloseHandle(handle)
        # Run a changed input, wait until the persistent state reports running,
        # then terminate the actual native indexer (its job owns the JDK child).
        self.write('B.java','class B {}')
        p=subprocess.Popen(self.command(),env=self.env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        try:
            deadline=time.monotonic()+15
            while time.monotonic()<deadline:
                try:
                    if json.loads((self.cache()/'config.json').read_text('utf-8'))['last_status']=='running': break
                except (FileNotFoundError,json.JSONDecodeError): pass
                time.sleep(.02)
            else: self.fail('index did not enter running state')
        finally: p.kill(); p.communicate(timeout=15)
        self.assertEqual(self.data()['state']['last_status'],'interrupted'); self.assertEqual(self.data()['units'],before)
        self.index(); self.assertEqual(self.data()['state']['total'],2)


if __name__=='__main__': unittest.main()
