"""Startup configuration validation. Run: python3 tests/test_config.py [binary]."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = Path(sys.argv.pop(1) if len(sys.argv) > 1 else 'build/timeline/uMediaBrowser2').resolve()


class ConfigurationTests(unittest.TestCase):
    def run_server(self, *args):
        return subprocess.run([str(BINARY), *map(str, args)], capture_output=True, timeout=5)

    def test_only_configuration_path_is_accepted(self):
        for args in ((), ('--rootdir', '/tmp'), ('--debug',), ('one.info', 'two.info'),
                     ('--daemon',), ('--install', 'service')):
            with self.subTest(args=args):
                result = self.run_server(*args)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(b'Usage:', result.stderr)
        self.assertEqual(self.run_server('--help').returncode, 0)

    def test_unreadable_configuration(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_server(Path(directory) / 'missing.info')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'Invalid configuration', result.stderr)

    def test_invalid_configuration_fails_before_listening(self):
        cases = ['http { rootdir "unterminated', 'unknown { value true }',
                 'logging { debug true\ndebug false }', 'http {} http {}',
                 'http scalar', 'http { rootdir { child value } }',
                 'http { unknown true }', 'http { can-upload true }',
                 'rootdir "/tmp"', 'http.lport 8001',
                 'tls { enabled true }', 'tls { enabled true\nkeyfile missing }',
                 '{"http":{"rootdir":"/tmp"}}', '[http]\nlport=8001']
        cases += [f'http {{ {key} {value} }}' for key in ('lport', 'threads')
                  for value in ('0', '-1', '65536', '99999999999999999999', '1.5', '80garbage')]
        cases += [f'{group} {{ {key} invalid }}' for group, key in
                  [('logging', 'debug'), ('logging', 'sys'), ('http', 'ipv4'),
                   ('permissions', 'can-upload'), ('http', 'use-x-real-ip'), ('tls', 'enabled')]]
        cases = [case.replace('{', '{\n').replace('}', '\n}') for case in cases]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'server.info'
            for content in cases:
                with self.subTest(content=content):
                    path.write_text(content)
                    result = self.run_server(path)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(b'Invalid configuration', result.stderr)


if __name__ == '__main__':
    unittest.main()
