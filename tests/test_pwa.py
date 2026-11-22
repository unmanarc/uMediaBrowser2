"""Installability integration tests. Run: python3 tests/test_pwa.py [binary]."""
import base64
import json
import struct
import unittest
import urllib.parse

import test_api_key as api_key


class PWATests(unittest.TestCase):
    server = api_key.APIKeyTests.server
    request = api_key.APIKeyTests.request

    def test_manifest_icons_and_both_pages(self):
        title = 'Gallery "Family" <&>'
        with self.server(None, title=title):
            for page in ('/', '/?view=timeline'):
                status, _, body = self.request(page)
                self.assertEqual(status, 200)
                self.assertIn(b'rel="manifest" href="?act=pwa_manifest" crossorigin="use-credentials"', body)
                self.assertIn(b'id="btnInstall"', body)
                self.assertNotIn(b'__PWA_', body)
                self.assertIn(b'Gallery &quot;Family&quot;', body)
            status, headers, body = self.request('/?act=pwa_manifest')
            self.assertEqual(status, 200)
            self.assertEqual(headers['Content-Type'], 'application/manifest+json; charset=UTF-8')
            self.assertIn('no-store', headers['Cache-Control'])
            manifest = json.loads(body)
            self.assertEqual(manifest['name'], title)
            self.assertEqual(manifest['display'], 'standalone')
            base = f'http://localhost:{self.port}/?act=pwa_manifest'
            self.assertEqual(urllib.parse.urljoin(base, manifest['start_url']), f'http://localhost:{self.port}/')
            self.assertEqual(manifest['scope'], './')
            self.assertEqual(manifest['id'], './')
            self.assertEqual([s['url'] for s in manifest['shortcuts']], ['./', '?view=timeline'])
            self.assertFalse(manifest['prefer_related_applications'])
            for size, icon in zip((192, 512), manifest['icons']):
                status, headers, body = self.request('/' + icon['src'])
                self.assertEqual(status, 200)
                self.assertEqual(headers['Content-Type'], 'image/png')
                self.assertEqual(body[:8], b'\x89PNG\r\n\x1a\n')
                self.assertEqual(struct.unpack('>II', body[16:24]), (size, size))
                self.assertEqual(icon['sizes'], f'{size}x{size}')
            self.assertEqual(self.request('/?act=pwa_icon&size=1024')[0], 404)

    def test_default_name_and_authentication(self):
        with self.server(api_key.KEY, basic=True):
            basic = 'Basic ' + base64.b64encode(b'user:password').decode()
            for path in ('/?act=pwa_manifest', '/?act=pwa_icon&size=192', '/?act=pwa_icon&size=512'):
                for headers in ({}, {'API-Key': api_key.KEY}, {'Authorization': basic}):
                    self.assertEqual(self.request(path, headers)[0], 401)
                status, _, body = self.request(path, {'API-Key': api_key.KEY, 'Authorization': basic})
                self.assertEqual(status, 200)
                if 'manifest' in path:
                    self.assertEqual(json.loads(body)['short_name'], 'uMediaBrowser')


if __name__ == '__main__':
    unittest.main()
