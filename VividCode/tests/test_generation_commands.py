"""Real CLI processes: global ranges, staging publication and same-window stop."""
import hashlib
import ctypes
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


RUNTIME = Path(os.environ.get('VIVIDCODE_TEST_RUNTIME', Path(__file__).resolve().parents[1] / 'build/Release'))


class GenerationCommands(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='vividcode commands 中文 ')
        cls.base = Path(cls.temporary.name)
        cls.home = cls.base / 'shared'
        cls.env = {**os.environ, 'VIVIDCODE_HOME': str(cls.home)}
        subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                        str(Path(__file__).resolve().parents[1] / 'install-runtime.ps1'),
                        '-RuntimeDirectory', str(RUNTIME), '-HomeDirectory', str(cls.home)],
                       env=cls.env, capture_output=True, check=True, timeout=30)
        cls.cli = cls.home / 'runtime/vividcode.exe'

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def setUp(self):
        self.folder = tempfile.TemporaryDirectory(dir=self.base, prefix='project ')
        self.root = Path(self.folder.name)
        (self.home / 'read-policy.json').unlink(missing_ok=True)

    def tearDown(self):
        self.folder.cleanup()

    def command(self, option, expected=0, text='', root=None):
        result = subprocess.run([str(self.cli), option], cwd=root or self.root, env=self.env,
                                input=text, capture_output=True, encoding='utf-8', timeout=35)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        return result

    def manifest(self):
        return json.loads((self.root / '.vividcode/project.json').read_text(encoding='utf-8'))

    def state(self, folder):
        return {p.relative_to(folder).as_posix(): (hashlib.sha256(p.read_bytes()).hexdigest(), p.stat().st_mtime_ns)
                for p in folder.rglob('*') if p.is_file()}

    def assert_no_temporary(self):
        self.assertFalse(list(self.root.glob('.vividcode-work-*')))
        self.assertFalse(list(self.root.glob('.vividcode-old-*')))
        self.assertFalse(list((self.home / 'semantic').glob('*-work-*')))
        self.assertFalse(list((self.home / 'semantic').glob('*-old-*')))

    def large_source(self):
        # Bounded test input: streamed by the product, never loaded as 256 MiB text.
        with (self.root / 'large.cpp').open('wb') as stream:
            stream.truncate(256 * 1024 * 1024)

    def stop_running(self, option, existed):
        (self.root / 'main.cpp').write_text('int main(){return 0;}')
        if existed:
            self.command('-g')
            store = self.root / '.vividcode'
            (store / 'manual.txt').write_text('keep')
            (store / 'semantic').mkdir()
            (store / 'semantic/index.db').write_bytes(b'keep legacy index')
            (store / 'semantic/config.json').write_text('{}')
            cache = self.home / 'semantic' / self.manifest()['cacheKey']
            cache.mkdir(parents=True)
            (cache / 'project.json').write_text(json.dumps(dict(app='vividcode', schema=1,
                                                               projectRoot=self.root.resolve().as_posix())))
            (cache / 'graph.db').write_bytes(b'keep shared graph')
            before, cache_before = self.state(store), self.state(cache)
        self.large_source()
        worker = subprocess.Popen([str(self.cli), option], cwd=self.root, env=self.env,
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  encoding='utf-8')
        try:
            while True:
                line = worker.stdout.readline()
                self.assertTrue(line, 'generator ended before accepting console input')
                if line.startswith('Generating:'):
                    break
            worker.stdin.write('stop\n')
            worker.stdin.flush()
            output, errors = worker.communicate(timeout=15)
            self.assertEqual(worker.returncode, 130, output + errors)
            self.assertEqual(errors, 'Cancelled.\n')
            self.assertNotIn('Refreshed.', output)
            if existed:
                self.assertEqual(self.state(store), before)
                self.assertEqual(self.state(cache), cache_before)
            else:
                self.assertFalse((self.root / '.vividcode').exists())
            self.assert_no_temporary()
            self.command('-s', expected=2)
        finally:
            if worker.poll() is None:
                worker.kill()
                worker.communicate(timeout=5)
            for stream in (worker.stdin, worker.stdout, worker.stderr):
                stream.close()

    def test_global_limit_validation_and_force_exclusions(self):
        for name in ('a.cpp', 'b.cpp'):
            (self.root / name).write_text('// source')
        self.command('-c', text='1\n')
        settings = (self.home / 'read-policy.json').read_bytes()
        for invalid in ('', '0\n', '-1\n', '1.5\n', 'no\n', '2147483648\n'):
            self.command('-c', expected=2, text=invalid)
            self.assertEqual((self.home / 'read-policy.json').read_bytes(), settings)
        self.command('-g', expected=1)
        self.assertFalse((self.root / '.vividcode').exists())
        self.assert_no_temporary()
        other = self.root / 'another-project'
        other.mkdir()
        (other / 'x.cpp').write_text('// x')
        (other / 'y.cpp').write_text('// y')
        self.command('-g', expected=1, root=other)
        # Keep the second project outside the current scan range afterwards.
        other.rename(self.root / 'build')
        (self.root / 'node_modules').mkdir()
        (self.root / 'node_modules/ignored.ts').write_text('???')
        (self.root / 'big.cpp').write_text('// ' + 'x' * (2 * 1024 * 1024 + 1))
        self.command('-sg')
        manifest = self.manifest()
        self.assertTrue(manifest['forceRead'])
        record = json.loads((self.root / '.vividcode/records' / manifest['latest'] / 'index.json').read_text())
        self.assertEqual({f['path'] for f in record['files']}, {'a.cpp', 'b.cpp', 'big.cpp'})
        before = self.state(self.root / '.vividcode')
        self.command('-s', expected=2)
        self.command('--s', expected=2)
        self.assertEqual(self.state(self.root / '.vividcode'), before)
        self.command('-c', text='10\n')
        self.command('-g')
        self.assertFalse(self.manifest()['forceRead'])
        self.assert_no_temporary()

    def test_n_preserves_latest_and_c_does_not_clean_history(self):
        source = self.root / 'main.cpp'
        source.write_text('// one')
        self.command('-g')
        hidden = self.root / '.vividcode/hidden-manual.txt'
        hidden.write_bytes(b'preserve hidden manual data')
        self.assertTrue(ctypes.windll.kernel32.SetFileAttributesW(ctypes.c_wchar_p(str(hidden)), 2))
        source.write_text('// two')
        self.command('-g')
        self.assertEqual(hidden.read_bytes(), b'preserve hidden manual data')
        attributes = ctypes.windll.kernel32.GetFileAttributesW(ctypes.c_wchar_p(str(hidden)))
        self.assertTrue(attributes & 2)
        latest = self.manifest()['latest']
        self.command('-c', text='999\n')
        self.assertEqual(len(self.manifest()['history']), 2)
        self.command('-n')
        self.assertEqual(self.manifest()['history'], [latest])

    def test_concise_refresh_counts_and_unchanged_results(self):
        (self.root / 'a.cpp').write_text('// a')
        (self.root / 'b.hpp').write_text('// b')
        (self.root / 'notes.txt').write_text('not source')
        expected = 'Generating: 0\nGenerating: 2/2\nRefreshed.\n'
        self.assertEqual(self.command('-g').stdout, expected)
        first_id = self.manifest()['latest']
        self.assertEqual(self.command('-g').stdout, expected)
        self.assertEqual(self.manifest()['history'], [first_id])
        (self.root / 'a.cpp').write_text('// changed')
        self.assertEqual(self.command('-g').stdout, expected)
        self.assertEqual(len(self.manifest()['history']), 2)
        self.assertEqual(self.command('-sg').stdout, expected)
        self.assertTrue(self.manifest()['forceRead'])
        self.assertEqual(self.command('-g').stdout, expected)
        self.assertFalse(self.manifest()['forceRead'])

    def test_short_english_settings_cleanup_help_and_errors(self):
        configured = self.command('-c', text='12\n')
        self.assertEqual(configured.stdout, 'Enter file limit: File limit set to 12.\n')
        self.assertEqual(configured.stderr, '')
        invalid = self.command('-c', expected=2, text='0\n')
        self.assertEqual(invalid.stdout, 'Enter file limit: ')
        self.assertEqual(invalid.stderr, 'Enter an integer from 1 to 2147483647.\n')
        (self.root / 'a.cpp').write_text('// a')
        self.command('-g')
        self.assertEqual(self.command('-n').stdout, 'Old data cleared.\n')
        self.assertEqual(self.command('-d').stdout, 'Project cleared.\n')
        self.assertEqual(self.command('-d').stdout, 'Project cleared.\n')
        absent = self.command('-n', expected=1)
        self.assertEqual(absent.stdout, '')
        self.assertEqual(absent.stderr, 'No project data. Run vividcode -g first.\n')
        bad = self.command('-invalid', expected=2)
        self.assertEqual(bad.stdout, '')
        self.assertEqual(bad.stderr, 'Invalid arguments. Use vividcode -help.\n')
        help_result = self.command('-help')
        self.assertTrue(help_result.stdout.isascii())
        self.assertIn('Type stop', help_result.stdout)
        self.assertEqual(help_result.stderr, '')

    def test_english_store_and_policy_failures_preserve_data(self):
        (self.root / '.vividcode').write_text('user file')
        invalid_store = self.command('-g', expected=1)
        self.assertEqual(invalid_store.stdout, '')
        self.assertEqual(invalid_store.stderr, '.vividcode is not a directory.\n')
        self.assertEqual((self.root / '.vividcode').read_text(), 'user file')
        (self.root / '.vividcode').unlink()
        settings = self.home / 'read-policy.json'
        settings.write_text('{invalid')
        broken_policy = self.command('-g', expected=1)
        self.assertEqual(broken_policy.stderr, 'Invalid file limit settings JSON.\n')
        self.assertEqual(settings.read_text(), '{invalid')
        self.assertFalse((self.root / '.vividcode').exists())

    def test_stop_new_normal_generation(self): self.stop_running('-g', False)
    def test_stop_existing_normal_generation(self): self.stop_running('-g', True)
    def test_stop_new_forced_generation(self): self.stop_running('-sg', False)
    def test_stop_existing_forced_generation(self): self.stop_running('-sg', True)

    def test_stop_line_discards_result_then_generation_can_retry(self):
        self.large_source()
        self.command('-sg', expected=130, text='stop\n')
        self.assertFalse((self.root / '.vividcode').exists())
        self.assert_no_temporary()
        (self.root / 'large.cpp').unlink()
        (self.root / 'main.cpp').write_text('// small')
        self.command('-g')
        self.assertEqual(len(self.manifest()['history']), 1)
        before = self.state(self.root / '.vividcode')
        self.large_source()
        self.command('-g', expected=130, text='stop\n')
        self.assertEqual(self.state(self.root / '.vividcode'), before)
        self.assert_no_temporary()


if __name__ == '__main__':
    unittest.main()
