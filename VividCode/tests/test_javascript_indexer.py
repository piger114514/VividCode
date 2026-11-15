"""Actual TypeScript compiler + lifecycle/cache integration, isolated from user projects."""
import ctypes
import json
import os
from pathlib import Path
import subprocess
import shutil
import tempfile
import time
import unittest

runtime = Path(os.environ.get('VIVIDCODE_JS_RUNTIME', Path(__file__).resolve().parents[1] / 'build/Release'))


class JavaScriptTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='vividcode JS 中文 ')
        self.root = Path(self.temp.name) / 'project'; self.root.mkdir()
        self.previous = os.environ.get('VIVIDCODE_HOME')
        os.environ['VIVIDCODE_HOME'] = str(Path(self.temp.name) / 'shared')
        component = Path(os.environ['VIVIDCODE_HOME']) / 'runtime/javascript-analyzer'
        component.mkdir(parents=True)
        shutil.copytree(runtime/'javascript-analyzer/typescript',component/'typescript')
        shutil.copy2(runtime/'javascript-analyzer/indexer.cjs',component/'indexer.cjs')
        shutil.copy2(runtime/'javascript-analyzer/runtime.json',component/'runtime.json')
        self.write('.vividcode/project.json', json.dumps(dict(app='vividcode', schema=1)))

    def tearDown(self):
        if self.previous is None: os.environ.pop('VIVIDCODE_HOME', None)
        else: os.environ['VIVIDCODE_HOME'] = self.previous
        self.temp.cleanup()

    def write(self, name, text, encoding='utf-8'):
        p = self.root / name; p.parent.mkdir(parents=True, exist_ok=True); p.write_text(text, encoding=encoding); return p

    def run_index(self):
        process = subprocess.run([str(runtime/'vividcode.exe'),'--javascript-analyzer','index','--project',str(self.root)],capture_output=True,encoding='utf-8',timeout=190)
        self.log = process.stdout + process.stderr
        return process.returncode

    def data(self):
        process = subprocess.run([str(runtime/'vividcode.exe'),'--javascript-analyzer','export','--project',str(self.root)],capture_output=True,encoding='utf-8',timeout=15)
        self.assertEqual(process.returncode,0,process.stdout+process.stderr)
        return json.loads(process.stdout)

    def edges(self):
        data = self.data(); symbols = {s['id']: s for u in data['units'] for s in u['symbols']}
        return [(symbols[r['source_id']]['qualified_name'], symbols[r['target_id']]['qualified_name'], r)
                for u in data['units'] for r in u['relations']]

    def has_edge(self, a, b, kind):
        self.assertTrue(any(x == a and y == b and r['type'] == kind for x, y, r in self.edges()), (a, b, kind, self.edges()))

    def test_parameters_destructuring_shadowing_and_imported_variables(self):
        self.write('api.ts', 'export const shared=1; export function first(x:number){let value=x; {let value=2; console.log(value);} return value+shared;}')
        self.write('use.ts', 'import {shared} from "./api"; function second({x}:{x:number}){let value=x; return value+shared;}')
        self.assertEqual(self.run_index(), 0, self.log)
        symbols = {s['id']:s for u in self.data()['units'] for s in u['symbols']}
        values = [s for s in symbols.values() if s['name']=='value']
        params = [s for s in symbols.values() if s['kind']=='ParmDecl']
        self.assertEqual(len(values), 3)
        self.assertEqual(len(params), 2)
        self.assertTrue(all(s['parent_id'] in symbols for s in values+params))
        shared = next(s['id'] for s in symbols.values() if s['name']=='shared')
        self.assertEqual({r['file'] for u in self.data()['units'] for r in u['relations'] if r['target_id']==shared}, {'api.ts','use.ts'})

    def test_tsconfig_alias_reexport_class_overload_arrow_members(self):
        self.write('tsconfig.json', '{"compilerOptions":{"target":"ES2022","module":"ESNext","moduleResolution":"Bundler","baseUrl":".","paths":{"@api":["src/api.ts"]}},"include":["src"]}')
        self.write('src/api.ts', 'export function helper(x:number):number;\nexport function helper(x:number){return x*2}\nexport class Base {run(){return 1}}\nexport interface I {run():number}\nexport class Worker extends Base implements I { value=2; run(){return helper(this.value)} }\nexport const arrow=()=>helper(1);\n')
        self.write('src/barrel.ts', 'export {helper as h,Worker,arrow} from "@api";\n')
        self.write('src/use.ts', 'import {h,Worker,arrow} from "./barrel";\nexport async function caller(){return h(2)+new Worker().run()+arrow()}\n')
        self.write('excluded.ts', 'function bad( {')
        self.assertEqual(self.run_index(), 0, self.log)
        self.has_edge('caller', 'helper', 'calls')
        self.has_edge('Worker', 'Base', 'inherits'); self.has_edge('Worker', 'I', 'implements')
        self.has_edge('Worker.run', 'helper', 'calls'); self.has_edge('Worker.run', 'Worker.value', 'member_ref')
        self.has_edge('arrow', 'helper', 'calls'); self.has_edge('caller', 'arrow', 'calls')
        self.has_edge('caller', 'Worker.run', 'possible_call')
        self.assertTrue(any(a == 'caller' and b == 'Worker.run' and r['confidence'] == 'possible' for a,b,r in self.edges()))
        helper = [s for u in self.data()['units'] for s in u['symbols'] if s['qualified_name'] == 'helper']
        self.assertTrue(all(s['def_line'] == 2 for s in helper))
        self.assertNotIn('excluded.ts', [u['file'] for u in self.data()['units']])

    def test_commonjs_jsx_tsx_mjs_cts(self):
        self.write('lib.cjs', 'exports.work = function work(){return 2};\n')
        self.write('use.cjs', 'const lib=require("./lib.cjs"); function caller(){return lib.work()}\n')
        self.write('view.tsx', 'export function Child(){return <span/>}\nexport const App=()=> <Child/>;\n')
        self.write('plain.jsx', 'function Local(){return <div/>} function View(){return <Local/>}\n')
        self.write('esm.mjs', 'export const esm=()=>1;\n')
        self.write('types.mts', 'export const typed=()=>1;\n')
        self.write('legacy.cts', 'export function legacy(){return 1}\n')
        self.assertEqual(self.run_index(), 0, self.log)
        self.assertTrue(any(a == 'caller' and b == 'exports.work' for a,b,r in self.edges()), self.edges())
        self.has_edge('App', 'Child', 'possible_call'); self.has_edge('View', 'Local', 'possible_call')
        self.assertEqual(self.data()['state']['total'], 7)

    def test_shadowing_dynamic_no_false_same_name_and_utf16_columns(self):
        self.write('lib.ts', 'export function target(){return 1}\n')
        self.write('use.ts', 'import {target} from "./lib";\nfunction shadow(target:any){return target()}\nfunction dynamic(obj:any){return obj.target()}\nfunction exact(){const 中文="😀"; return target()}\n', 'utf-16')
        self.assertEqual(self.run_index(), 0, self.log)
        self.assertFalse(any(a in {'shadow','dynamic'} and b == 'target' for a,b,r in self.edges()), self.edges())
        self.has_edge('exact', 'target', 'calls')
        site = next(r for a,b,r in self.edges() if a == 'exact' and b == 'target')
        line = 'function exact(){const 中文="😀"; return target()}'
        self.assertEqual(site['col'], len(line[:line.index('target()')].encode('utf-16-le')) // 2 + 1)

    def test_cache_dependency_change_failure_preservation_deletion(self):
        self.write('lib.ts','export function f(){return 1}\n')
        self.write('use.ts','import {f} from "./lib"; export function caller(){return f()}\n')
        self.assertEqual(self.run_index(),0,self.log); self.assertEqual(self.run_index(),0,self.log)
        self.assertEqual(self.data()['state']['reused'],2)
        self.write('lib.ts','export function changed(){return 1}\n')
        self.assertEqual(self.run_index(),0,self.log)
        self.assertFalse(any(a == 'caller' and b == 'f' for a,b,r in self.edges()))
        before = self.data()['units']; self.write('lib.ts','export function broken( {')
        self.assertEqual(self.run_index(),1,self.log); self.assertEqual(self.data()['units'],before)
        self.assertEqual(self.data()['state']['last_status'],'failed')
        (self.root/'lib.ts').unlink(); (self.root/'use.ts').unlink()
        self.assertEqual(self.run_index(),0,self.log); self.assertEqual(self.data()['units'],[])

    def test_config_change_external_declaration_and_added_dependency(self):
        self.write('tsconfig.json','{"compilerOptions":{"module":"ESNext","moduleResolution":"Bundler","baseUrl":".","paths":{"@x":["a.ts"]}}}')
        self.write('a.ts','export function f(){return 1}'); self.write('b.ts','export function f(){return 2}')
        self.write('use.ts','import {f} from "@x"; import {external} from "dep"; export function caller(){return f()+external()}')
        self.assertEqual(self.run_index(),0,self.log)
        before = next(r['target_id'] for a,b,r in self.edges() if a == 'caller' and b == 'f')
        self.write('tsconfig.json','{"compilerOptions":{"module":"ESNext","moduleResolution":"Bundler","baseUrl":".","paths":{"@x":["b.ts"]}}}')
        self.write('node_modules/dep/package.json','{"types":"index.d.ts"}')
        self.write('node_modules/dep/index.d.ts','export declare function external():number;')
        self.assertEqual(self.run_index(),0,self.log); self.assertEqual(self.data()['state']['reused'],0)
        after = next(r['target_id'] for a,b,r in self.edges() if a == 'caller' and b == 'f')
        self.assertNotEqual(before,after)
        self.assertTrue(any(a == 'caller' and b.startswith('Unresolved: ') for a,b,r in self.edges()))
        self.assertFalse(any(u['file'].startswith('node_modules') for u in self.data()['units']))

    def test_project_references_multiple_configs_and_size_limit(self):
        self.write('tsconfig.json','{"files":[],"references":[{"path":"./pkg"}]}')
        self.write('pkg/tsconfig.json','{"compilerOptions":{"composite":true},"include":["*.ts"]}')
        self.write('pkg/a.ts','export function a(){return 1}')
        self.write('other/tsconfig.json','{"include":["*.ts"]}'); self.write('other/b.ts','export const b=()=>1')
        self.assertEqual(self.run_index(),0,self.log); self.assertEqual(self.data()['state']['total'],2)
        before=self.data()['units']; self.write('pkg/huge.ts',' '*(2*1024*1024+1))
        self.assertEqual(self.run_index(),1,self.log); self.assertEqual(self.data()['units'],before)

    def test_actual_cancel_ends_child_and_keeps_snapshot(self):
        self.write('a.ts','export function a(){return 1}')
        self.assertEqual(self.run_index(),0,self.log); before=self.data()['units']
        for i in range(100): self.write(f'many/{i}.ts',f'export class C{i} {{run(){{return {i}}}}}')
        process=subprocess.Popen([str(runtime/'vividcode.exe'),'--javascript-analyzer','index','--project',str(self.root)],stdout=subprocess.PIPE)
        try:
            line=process.stdout.readline(); self.assertIn(b'progress',line)
            # Observe the actual child through ToolHelp, then hold a wait handle
            # to verify cancellation terminates Node as well as its Python parent.
            from ctypes import wintypes as w
            class Entry(ctypes.Structure):
                _fields_=[('size',w.DWORD),('usage',w.DWORD),('pid',w.DWORD),('heap',ctypes.c_size_t),
                          ('module',w.DWORD),('threads',w.DWORD),('parent',w.DWORD),('priority',w.LONG),
                          ('flags',w.DWORD),('exe',w.WCHAR*260)]
            kernel=ctypes.WinDLL('kernel32',use_last_error=True)
            kernel.CreateToolhelp32Snapshot.argtypes=[w.DWORD,w.DWORD]; kernel.CreateToolhelp32Snapshot.restype=w.HANDLE
            kernel.Process32FirstW.argtypes=[w.HANDLE,ctypes.POINTER(Entry)]
            kernel.Process32NextW.argtypes=[w.HANDLE,ctypes.POINTER(Entry)]
            kernel.OpenProcess.argtypes=[w.DWORD,w.BOOL,w.DWORD]; kernel.OpenProcess.restype=w.HANDLE
            kernel.CloseHandle.argtypes=[w.HANDLE]; kernel.WaitForSingleObject.argtypes=[w.HANDLE,w.DWORD]
            child=None; deadline=time.monotonic()+5
            while not child and time.monotonic()<deadline:
                snapshot=kernel.CreateToolhelp32Snapshot(2,0); entry=Entry(); entry.size=ctypes.sizeof(entry)
                try:
                    more=kernel.Process32FirstW(snapshot,ctypes.byref(entry))
                    while more:
                        if entry.parent == process.pid and entry.exe.lower() == 'node.exe':
                            child=kernel.OpenProcess(0x100000,False,entry.pid); break
                        more=kernel.Process32NextW(snapshot,ctypes.byref(entry))
                finally: kernel.CloseHandle(snapshot)
                if not child: time.sleep(0.01)
            self.assertTrue(child,'Actual Node worker did not start')
            time.sleep(0.05); process.kill(); process.wait(timeout=5)
            try: self.assertEqual(kernel.WaitForSingleObject(child,5000),0,'Node worker survived cancellation')
            finally: kernel.CloseHandle(child)
            self.assertEqual(self.data()['units'],before); self.assertEqual(self.data()['state']['last_status'],'interrupted')
            self.assertEqual(self.run_index(),0,self.log)
        finally:
            if process.poll() is None: process.kill(); process.wait()
            process.stdout.close()

    def test_real_analyzer_source_and_never_execute_target(self):
        actual = Path(__file__).resolve().parents[1] / 'javascript-analyzer/indexer.cjs'
        self.write('indexer.cjs', actual.read_text(encoding='utf-8'))
        sentinel = self.root / 'executed.txt'
        self.write('side-effect.cjs', 'require("node:fs").writeFileSync(' + json.dumps(str(sentinel)) + ',"executed");')
        self.assertEqual(self.run_index(),0,self.log)
        self.has_edge('analyze','scan','calls')
        self.assertFalse(sentinel.exists(),'Target project code was executed')
        data=self.data(); symbols={s['id'] for u in data['units'] for s in u['symbols'] if not s.get('is_external')}
        sites=sum(len(u['relations']) for u in data['units'])
        print(f'Real indexer source: {len(symbols)} internal symbols, {sites} relation sites, {data["state"]["unresolved"]} unresolved')
        self.assertEqual(self.run_index(),0,self.log); self.assertEqual(self.data()['state']['reused'],2)


    def test_reassigned_function_is_only_possible(self):
        self.write('a.ts','function target(){return 1}\nfunction replacement(){return 2}\ntarget=replacement;\nexport function caller(){return target()}')
        self.assertEqual(self.run_index(),0,self.log)
        self.has_edge('caller','target','possible_call')


    def test_global_limit_and_forced_large_source(self):
        self.write('a.ts', 'export function a(){return 1}')
        self.write('b.ts', 'export function b(){return 2}')
        settings = Path(os.environ['VIVIDCODE_HOME']) / 'read-policy.json'
        settings.write_text(json.dumps(dict(app='vividcode', schema=1, maxSourceFiles=1)))
        self.assertEqual(self.run_index(), 1, self.log)
        self.write('.vividcode/project.json', json.dumps(dict(app='vividcode', schema=1, forceRead=True)))
        self.write('a.ts', '/*' + 'x' * (2 * 1024 * 1024 + 1) + '*/\nexport function a(){return 1}')
        self.write('build/ignored.ts', 'invalid syntax ???')
        self.write('.vividcode-work-temporary/ignored.ts', 'invalid syntax ???')
        self.assertEqual(self.run_index(), 0, self.log)
        self.assertEqual(self.data()['state']['total'], 2)
        before = self.data()['units']
        self.write('.vividcode/project.json', json.dumps(dict(app='vividcode', schema=1, forceRead=False)))
        settings.write_text(json.dumps(dict(app='vividcode', schema=1, maxSourceFiles=3)))
        self.assertEqual(self.run_index(), 1, self.log)
        self.assertEqual(self.data()['units'], before)
        self.write('a.ts', 'export function a(){return 1}')
        self.assertEqual(self.run_index(), 0, self.log)
        settings.write_text(json.dumps(dict(app='vividcode', schema=1, maxSourceFiles=1)))
        self.assertEqual(self.run_index(), 1, self.log)

if __name__ == '__main__': unittest.main()
