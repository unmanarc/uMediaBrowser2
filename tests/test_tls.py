"""TLS integration tests. Run: python3 tests/test_tls.py [binary]."""
from contextlib import contextmanager
import http.client
import os
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import tempfile
import time
import unittest

from config_support import config_command

BINARY = Path(sys.argv.pop(1) if len(sys.argv) > 1 else 'build/timeline/uMediaBrowser2').resolve()


class TLSTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='umedia-tls-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.root = Path(cls.temp.name)
        cls.key, cls.cert = cls.root/'server.key', cls.root/'server.crt'
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                        '-keyout', str(cls.key), '-out', str(cls.cert), '-days', '1',
                        '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost'],
                       check=True, capture_output=True)
        cls.invalid = cls.root/'invalid.pem'
        cls.invalid.write_text('not a PEM file')

    def command(self, **overrides):
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            self.port = sock.getsockname()[1]
        settings = {'http.rootdir': self.root, 'http.laddr': '127.0.0.1',
                    'http.lport': self.port, 'tls.enabled': True,
                    'tls.keyfile': self.key, 'tls.certfile': self.cert}
        settings.update(overrides)
        return config_command(BINARY, self.root, **settings)

    def request(self, context=None, secure=True):
        connection = (http.client.HTTPSConnection('localhost', self.port, context=context, timeout=2)
                      if secure else http.client.HTTPConnection('localhost', self.port, timeout=2))
        try:
            connection.request('GET', '/?act=pwa_manifest')
            response = connection.getresponse()
            response.read()
            return response.status
        finally:
            connection.close()

    @contextmanager
    def server(self, context=None, secure=True, **overrides):
        env = dict(os.environ)
        env.pop('APIKEY', None)
        with tempfile.TemporaryFile() as log:
            process = subprocess.Popen(self.command(**overrides), stdout=log, stderr=log, env=env)
            try:
                for _ in range(200):
                    if process.poll() is not None:
                        log.seek(0)
                        self.fail(log.read().decode())
                    try:
                        self.assertEqual(self.request(context, secure), 200)
                        break
                    except OSError:
                        time.sleep(.025)
                else:
                    self.fail('Configured server did not start')
                yield
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()

    def test_https_with_explicit_enable(self):
        context = ssl.create_default_context(cafile=str(self.cert))
        with self.server(context):
            self.assertEqual(self.request(context), 200)

    def test_disabled_tls_serves_http_and_ignores_certificate_paths(self):
        with self.server(secure=False, **{'tls.enabled': False, 'tls.keyfile': '/missing/key',
                                         'tls.certfile': '/missing/cert', 'tls.cafile': '/missing/ca'}):
            self.assertEqual(self.request(secure=False), 200)

    def test_ca_requires_client_certificate(self):
        context = ssl.create_default_context(cafile=str(self.cert))
        context.load_cert_chain(str(self.cert), str(self.key))
        with self.server(context, **{'tls.cafile': self.cert}):
            with self.assertRaises((OSError, http.client.RemoteDisconnected)):
                self.request(ssl.create_default_context(cafile=str(self.cert)))

    def test_unreadable_or_invalid_credentials_prevent_startup(self):
        for key in ('tls.keyfile', 'tls.certfile', 'tls.cafile'):
            paths = [self.root/'missing.pem']
            if key != 'tls.cafile': paths.append(self.invalid)
            for path in paths:
                with self.subTest(key=key, path=path):
                    result = subprocess.run(self.command(**{key: path}), capture_output=True, timeout=5)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(('Cannot load '+key).encode(), result.stderr)


if __name__ == '__main__':
    unittest.main()
