"""API key integration tests. Run: python3 tests/test_api_key.py [binary]"""
from config_support import config_command
import base64
from contextlib import contextmanager
import http.client
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest

BINARY = Path(sys.argv.pop(1) if len(sys.argv) > 1 else 'build/timeline/uMediaBrowser2').resolve()
KEY = 'test-api-secret'


class APIKeyTests(unittest.TestCase):
    @contextmanager
    def server(self, key, basic=False, title=None):
        with tempfile.TemporaryDirectory(prefix='umedia-api-key-') as directory:
            self.root = Path(directory)
            with socket.socket() as sock:
                sock.bind(('127.0.0.1', 0))
                self.port = sock.getsockname()[1]
            env = dict(os.environ)
            env.pop('APIKEY', None)
            if key is not None:
                env['APIKEY'] = key
            settings = {'http.rootdir': directory, 'http.laddr': '127.0.0.1',
                        'http.lport': self.port, 'permissions.can-upload': True, 'logging.sys': True}
            if title is not None:
                settings['http.title'] = title
            if basic:
                password = self.root / 'password'
                password.write_text('user\npassword\n')
                settings['security.passfile'] = password
            with tempfile.TemporaryFile() as log:
                process = subprocess.Popen(config_command(BINARY, directory, **settings), env=env, stdout=log, stderr=log)
                try:
                    for _ in range(200):
                        if process.poll() is not None:
                            log.seek(0)
                            self.fail('Server exited before accepting requests: ' + log.read().decode())
                        try:
                            self.request('/')
                            break
                        except OSError:
                            time.sleep(.025)
                    else:
                        self.fail('Server failed to start')
                    yield log
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()

    def request(self, path, headers=None, method='GET', body=None):
        connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=2)
        try:
            connection.request(method, path, body=body, headers=headers or {})
            response = connection.getresponse()
            result = response.status, dict(response.getheaders()), response.read()
            return result
        finally:
            connection.close()

    def test_unset_and_empty_disable_requirement(self):
        for key in (None, ''):
            with self.subTest(key=key), self.server(key):
                self.assertEqual(self.request('/')[0], 200)
                self.assertEqual(self.request('/?act=ls', {'API-Key': 'anything'})[0], 200)

    def test_required_for_pages_actions_and_static_paths(self):
        with self.server(KEY) as log:
            for path in ('/', '/?view=timeline', '/?act=ls', '/favicon.ico', '/?act=download&path=/missing',
                         '/?act=pwa_manifest', '/?act=pwa_icon&size=192', '/?act=pwa_icon&size=512'):
                for headers in ({}, {'API-Key': ''}, {'API-Key': 'wrong'}, {'API-Key': KEY.upper()}):
                    with self.subTest(path=path, headers=headers):
                        self.assertEqual(self.request(path, headers)[0], 401)
            self.assertEqual(self.request('/?API-Key='+KEY)[0], 401)
            for header in ('API-Key', 'api-key', 'API-KEY'):
                self.assertEqual(self.request('/?act=ls', {header: KEY})[0], 200)
            log.seek(0)
            output = log.read()
            self.assertIn(b'API Key authentication enabled', output)
            self.assertNotIn(KEY.encode(), output)

    def test_basic_auth_is_additional(self):
        basic = 'Basic ' + base64.b64encode(b'user:password').decode()
        with self.server(KEY, basic=True):
            self.assertEqual(self.request('/', {'Authorization': basic})[0], 401)
            self.assertEqual(self.request('/', {'API-Key': KEY})[0], 401)
            self.assertEqual(self.request('/', {'API-Key': KEY, 'Authorization': basic})[0], 200)

    def test_unauthorized_upload_does_not_write_file(self):
        with self.server(KEY):
            _, headers, _ = self.request('/', {'API-Key': KEY})
            cookie = headers['Set-Cookie'].split(';', 1)[0]
            token = cookie.split('=', 1)[1]
            boundary = 'api-key-test-boundary'
            body = (f'--{boundary}\r\nContent-Disposition: form-data; name="file"; '
                    'filename="unauthorized.txt"\r\nContent-Type: text/plain\r\n\r\n'
                    f'secret\r\n--{boundary}--\r\n').encode()
            for key in (None, 'wrong'):
                headers = {'Cookie': cookie, 'X-CSRFToken': token,
                           'Content-Type': 'multipart/form-data; boundary='+boundary}
                if key is not None:
                    headers['API-Key'] = key
                self.assertEqual(self.request('/?act=upload&path=/', headers, 'POST', body)[0], 401)
                self.assertFalse((self.root / 'unauthorized.txt').exists())


if __name__ == '__main__':
    unittest.main()
