"""Mandatory server prefilter integration tests.

Run: python3 tests/test_prefilter.py build/timeline/uMediaBrowser2
Uses the timeline fixture, real HTTP/SQLite, tar/zip readers, and optional Playwright.
"""
from contextlib import contextmanager
import http.cookiejar
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tarfile
import time
from types import SimpleNamespace
import unittest
import urllib.error
import urllib.parse
import urllib.request
import zipfile

import test_timeline as timeline


class PrefilterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = timeline.TimelineTests
        cls.library.setUpClass()
        cls.addClassCleanup(cls.library.tearDownClass)
        cls.rule = dict(field='tag', op='eq', value='Visible')
        cls.prefilter = dict(rules=[cls.rule])
        cls.allowed = ['Prefilter/mixed/allowed.jpg', 'Prefilter/mixed/deep/allowed.jpg',
                       'Prefilter/parent/child/allowed.jpg', 'Prefilter/mixed/no-date.jpg']
        for index, name in enumerate(cls.allowed + ['Prefilter/mixed/excluded.jpg', 'Prefilter/hidden/hidden.jpg',
                                                   'Prefilter/mixed/empty/hidden.jpg', 'Prefilter/missing.jpg']):
            path = cls.library.photo(name, 1_850_000_000 + index*10)
            with timeline.sqlite_connection(cls.library.database) as db:
                image = db.execute('SELECT i.id FROM Images i JOIN Albums a ON a.id=i.album WHERE a.relativePath=? AND i.name=?',
                                   ('/Familiar/'+str(Path(name).parent), Path(name).name)).fetchone()[0]
                db.execute('INSERT OR IGNORE INTO Tags(id,pid,name) VALUES(9800,0,\'Visible\')')
                db.execute('INSERT OR IGNORE INTO Tags(id,pid,name) VALUES(9801,0,\'Hidden\')')
                db.execute('INSERT INTO ImageTags VALUES(?,?)', (image, 9800 if name in cls.allowed or 'missing' in name else 9801))
                db.execute('INSERT INTO ImageMetadata(imageid,make,sensitivity) VALUES(?,?,?)', (image, 'Canon', (index+1)*100))
                if 'no-date' in name:
                    db.execute("UPDATE Images SET modificationDate='bad date' WHERE id=?", (image,))
            if 'missing' in name: path.unlink()
        cls.library.photo('Prefilter/mixed/unindexed.jpg', 1_850_000_000, indexed=False)
        (cls.library.root/'Prefilter/alias').symlink_to(cls.library.root/'Prefilter/mixed', target_is_directory=True)

    @contextmanager
    def server(self, prefilter=None, enabled=True):
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0)); port = sock.getsockname()[1]
        filename = Path(self.library.temp.name)/f'prefilter-{port}.json'
        filename.write_text(json.dumps(self.prefilter if prefilter is None else prefilter))
        args = [str(timeline.BINARY), '--rootdir', str(self.library.root), '-a', '127.0.0.1', '-l', str(port), '-g']
        if enabled: args += ['--prefilter', str(filename)]
        cookies = http.cookiejar.CookieJar()
        opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(cookies))
        url = f'http://127.0.0.1:{port}/'
        def read(**params):
            with opener.open(url+'?'+urllib.parse.urlencode(params), timeout=20) as response:
                return response.read()
        def api(**params):
            return json.loads(read(**params))['data']
        def post(action):
            token = next(c.value for c in cookies if c.name == 'CSRFToken')
            request = urllib.request.Request(url+'?act='+action, method='POST', headers={'X-CSRFToken': token})
            with opener.open(request, timeout=20) as response:
                return json.load(response)['data']
        with open(Path(self.library.temp.name)/f'prefilter-{port}.log', 'w+') as log:
            process = subprocess.Popen(args, stdout=log, stderr=log, env=dict(os.environ, TZ='UTC'))
            try:
                for _ in range(200):
                    if process.poll() is not None:
                        log.seek(0); self.fail(log.read())
                    try: read(view='timeline'); break
                    except OSError: time.sleep(.025)
                else: self.fail('Prefilter server did not start')
                yield SimpleNamespace(url=url, read=read, api=api, post=post, filename=filename)
            finally:
                process.terminate()
                try: process.wait(timeout=5)
                except subprocess.TimeoutExpired: process.kill(); process.wait()

    def assert_http_error(self, status, action):
        with self.assertRaises(urllib.error.HTTPError) as error: action()
        self.assertEqual(error.exception.code, status)
        error.exception.close()

    def test_timeline_and_cannot_override_mandatory_filter(self):
        with self.server() as server:
            page = server.api(act='timeline')
            self.assertTrue(page['prefilter_active'])
            self.assertEqual(page['total_items'], 3)
            self.assertEqual(page['filter'], dict(rules=[]))
            hidden = json.dumps(dict(rules=[dict(self.rule, value='Hidden')]))
            self.assertEqual(server.api(act='timeline', filter=hidden)['total_items'], 0)
            either = json.dumps(dict(rules=[dict(match='any', rules=[self.rule, dict(self.rule, value='Hidden')])]))
            self.assertEqual(server.api(act='timeline', filter=either)['total_items'], 3)
            user = json.dumps(dict(rules=[dict(field='sensitivity', op='gt', value=150)]))
            page = server.api(act='timeline', filter=user)
            self.assertEqual(page['total_items'], 2)
            event = page['events'][0]
            items = server.api(act='timeline_event', event=event['id'], filter=user, limit=1)
            self.assertTrue(items['has_more'])
            second = server.api(act='timeline_event', event=event['id'], filter=user, cursor=1, limit=1)
            self.assertFalse(second['has_more'])
            samples = server.api(act='timeline_samples', event=event['id'], filter=user)['samples']
            expected = {'/'+name for name in self.allowed[1:3]}
            self.assertEqual({item['path'] for item in items['items']+second['items']}, expected)
            self.assertEqual({item['path'] for item in samples}, expected)
            self.assert_http_error(409, lambda: server.api(act='timeline_event', event=event['id']))
            self.assertEqual(server.post('timeline_refresh')['total_items'], 3)
            self.assertEqual({t['name'] for t in server.api(act='timeline_tags')['tags']}, {'Visible'})
            self.assertEqual({t['name'] for t in server.post('timeline_tags_refresh')['tags']}, {'Visible'})

    def test_folders_tree_ancestors_and_undated_files(self):
        with self.server() as server:
            self.assertEqual([n['name'] for n in server.api(act='tree')['nodes']], ['Prefilter'])
            nodes = {n['name']: n for n in server.api(act='tree', path='/Prefilter')['nodes']}
            self.assertEqual(set(nodes), {'mixed', 'parent'})
            self.assertTrue(nodes['parent']['has_children'])
            self.assertEqual(server.api(act='tree', path='/Prefilter/mixed')['nodes'],
                             [dict(name='deep', path='/Prefilter/mixed/deep', has_children=False)])
            items = server.api(act='ls', path='/Prefilter/mixed')['items']
            self.assertEqual({i['name'] for i in items}, {'deep', 'allowed.jpg', 'no-date.jpg'})
            self.assertEqual(server.api(act='ls', path='/Prefilter/hidden')['items'], [])
            # Passing a user query cannot remove the mandatory folder constraint.
            self.assertEqual(server.api(act='ls', path='/Prefilter/mixed', filter='{"rules":[]}')['items'], items)
            # Current filesystem/database state, without requiring the tag rescan.
            path = self.library.root/self.allowed[2]
            saved = path.read_bytes(); path.unlink()
            try:
                self.assertNotIn('parent', {n['name'] for n in server.api(act='tree', path='/Prefilter')['nodes']})
            finally: path.write_bytes(saved)

    def test_metadata_choices_respect_prefilter(self):
        with timeline.sqlite_connection(self.library.database) as db:
            db.execute("UPDATE ImageMetadata SET model='Allowed', lens='Visible lens' WHERE imageid IN (SELECT imageid FROM ImageTags WHERE tagid=9800)")
            db.execute("UPDATE ImageMetadata SET model='Hidden', lens='Hidden lens' WHERE imageid IN (SELECT imageid FROM ImageTags WHERE tagid=9801)")
        with self.server() as server:
            values = server.api(act='timeline_filter_values')['values']
            self.assertEqual(values, dict(make=['Canon'], model=['Allowed'], lens=['Visible lens']))
        with self.server(dict(rules=[dict(self.rule, value='Absent')])) as server:
            self.assertEqual(server.api(act='timeline_filter_values')['values'], dict(make=[], model=[], lens=[]))

    def test_composed_prefilter_and_user_groups(self):
        prefilter = dict(rules=[dict(match='any', rules=[self.rule, dict(self.rule, value='Absent')]),
                               dict(field='sensitivity', op='gt', value=150)])
        user = dict(rules=[dict(match='any', rules=[dict(self.rule, value='Hidden'),
                                                   dict(field='sensitivity', op='lt', value=250)])])
        with self.server(prefilter) as server:
            page = server.api(act='timeline', filter=json.dumps(user))
            self.assertEqual(page['total_items'], 1)
            self.assertEqual(page['events'][0]['samples'][0]['path'], '/Prefilter/mixed/deep/allowed.jpg')
            self.assertEqual({i['name'] for i in server.api(act='ls', path='/Prefilter/mixed')['items']}, {'deep', 'no-date.jpg'})
            self.assertEqual(server.api(act='ls', path='/Prefilter/hidden')['items'], [])
        with self.server(dict(rules=[dict(self.rule, value='Absent')])) as server:
            self.assertEqual(server.api(act='timeline')['total_items'], 0)
            self.assertEqual(server.api(act='tree')['nodes'], [])
            self.assertEqual(server.api(act='ls')['items'], [])

    def test_direct_media_and_archives_respect_prefilter(self):
        with self.server() as server:
            for action in ('download', 'thumb', 'detail'):
                self.assert_http_error(404, lambda: server.read(act=action, file='/Prefilter/mixed/excluded.jpg'))
                self.assert_http_error(404, lambda: server.read(act=action, file='/Prefilter/mixed/unindexed.jpg'))
            self.assertEqual(server.read(act='download', file='/Prefilter/mixed/allowed.jpg'), self.library.base.read_bytes())
            expected = {'mixed/allowed.jpg', 'mixed/no-date.jpg', 'mixed/deep/allowed.jpg'}
            for fmt in ('tar', 'tar.gz', 'zip'):
                with self.subTest(fmt=fmt):
                    content = io.BytesIO(server.read(act='archive', files='/Prefilter/mixed', fmt=fmt))
                    if fmt == 'zip':
                        with zipfile.ZipFile(content) as archive: names = set(archive.namelist())
                    else:
                        with tarfile.open(fileobj=content) as archive: names = set(archive.getnames())
                    self.assertEqual(names, expected)
            self.assert_http_error(400, lambda: server.read(act='archive', files='/Prefilter/hidden'))

    def test_tag_icons_respect_prefilter(self):
        with self.server() as server:
            try:
                with timeline.sqlite_connection(self.library.database) as db:
                    allowed = db.execute("SELECT i.id FROM Images i JOIN Albums a ON a.id=i.album WHERE a.relativePath='/Familiar/Prefilter/mixed' AND i.name='allowed.jpg'").fetchone()[0]
                    excluded = db.execute("SELECT id FROM Images WHERE name='excluded.jpg'").fetchone()[0]
                    db.execute('UPDATE Tags SET icon=? WHERE id IN (9800,9801)', (allowed,))
                self.assertTrue(server.read(act='timeline_tag_icon', id=9800).startswith(b'\xff\xd8'))
                self.assert_http_error(404, lambda: server.read(act='timeline_tag_icon', id=9801))
                with timeline.sqlite_connection(self.library.database) as db:
                    db.execute('UPDATE Tags SET icon=? WHERE id=9800', (excluded,))
                self.assert_http_error(404, lambda: server.read(act='timeline_tag_icon', id=9800))
            finally:
                with timeline.sqlite_connection(self.library.database) as db:
                    db.execute('UPDATE Tags SET icon=NULL WHERE id IN (9800,9801)')

    def test_export_roundtrip_and_loaded_once(self):
        user_rule = dict(field='sensitivity', op='gt', value=150)
        user = json.dumps(dict(rules=[user_rule]))
        with self.server() as server:
            exported = server.api(act='current_filter', filter=user)
            self.assertEqual(exported['rules'], [dict(self.rule, negate=False), dict(user_rule, negate=False)])
            # Changing the file cannot replace the startup restriction.
            server.filename.write_text('{"rules":[]}')
            self.assertEqual(server.api(act='timeline')['total_items'], 3)
            with self.server(exported) as restored:
                self.assertEqual(restored.api(act='timeline')['total_items'], 2)
                self.assertEqual({i['name'] for i in restored.api(act='ls', path='/Prefilter/mixed')['items']}, {'deep', 'no-date.jpg'})
            # Full independent filter budgets combine successfully and export to a prefilter.
            many = json.dumps(dict(rules=[user_rule]*20))
            exported = server.api(act='current_filter', filter=many)
            with self.server(exported) as restored:
                self.assertEqual(restored.api(act='timeline')['total_items'], 2)

    def test_empty_prefilter_and_unconfigured_behavior(self):
        with self.server(dict(rules=[])) as server:
            names = {i['name'] for i in server.api(act='ls', path='/Prefilter/mixed')['items']}
            self.assertIn('excluded.jpg', names)
            self.assertNotIn('unindexed.jpg', names)
        with self.server(enabled=False) as server:
            names = {i['name'] for i in server.api(act='ls', path='/Prefilter/mixed')['items']}
            self.assertIn('unindexed.jpg', names)
            self.assertFalse(server.api(act='timeline')['prefilter_active'])
            self.assertEqual(server.api(act='current_filter'), dict(rules=[]))

    def test_database_failure_never_unfilters_folders(self):
        with self.server() as server:
            backup = self.library.database.with_suffix('.prefilter-backup')
            self.library.database.rename(backup)
            try:
                self.assert_http_error(503, lambda: server.api(act='ls'))
                self.assert_http_error(503, lambda: server.api(act='tree'))
                self.assert_http_error(404, lambda: server.read(act='download', file='/Prefilter/mixed/allowed.jpg'))
                self.assertFalse(server.api(act='timeline')['database_available'])
            finally: backup.rename(self.library.database)

    def test_invalid_startup_filters(self):
        filename = Path(self.library.temp.name)/'invalid-prefilter.json'
        for content in (None, '', ' ', '{', '{}', '[]', '{"rules":[],"typo":1}',
                        json.dumps(dict(rules=[dict(self.rule, field='unknown')])),
                        ' '*65537, json.dumps(dict(rules=[self.rule]*201))):
            with self.subTest(content=str(content)[:80]):
                if content is None: filename.unlink(missing_ok=True)
                else: filename.write_text(content)
                result = subprocess.run([str(timeline.BINARY), '--prefilter', str(filename)],
                                        capture_output=True, timeout=5)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(b'Invalid --prefilter', result.stdout+result.stderr)

    @unittest.skipUnless(importlib.util.find_spec('playwright') and shutil.which('google-chrome'), 'Requires Playwright and Chrome')
    def test_console_export_and_reset_in_browser(self):
        from playwright.sync_api import sync_playwright
        with self.server() as server, sync_playwright() as playwright:
            browser = playwright.chromium.launch(executable_path=shutil.which('google-chrome'), headless=True,
                                                 args=['--no-sandbox', '--disable-dev-shm-usage'])
            try:
                page = browser.new_page(accept_downloads=True)
                errors = []; page.on('pageerror', lambda error: errors.append(str(error)))
                user = json.dumps(dict(rules=[dict(field='sensitivity', op='gt', value=150)]))
                page.goto(server.url+'?'+urllib.parse.urlencode(dict(view='timeline', filter=user)))
                page.wait_for_function('feed.loaded')
                exported = page.evaluate('getCurrentFilter()')
                self.assertEqual(len(exported['rules']), 2)
                self.assertEqual(page.evaluate('showCurrentFilter()'), exported)
                with page.expect_download() as download:
                    self.assertEqual(page.evaluate('downloadCurrentFilter()'), exported)
                self.assertEqual(download.value.suggested_filename, 'prefilter.json')
                self.assertEqual(json.loads(Path(download.value.path()).read_text()), exported)
                page.locator('#btnTimelineFilter').click()
                self.assertTrue(page.locator('#prefilterNotice').is_visible())
                page.locator('#filterReset').click()
                page.wait_for_function('feed.loaded && !filterJSON')
                self.assertIn('3 items', page.locator('#timelineStatus').inner_text())
                self.assertEqual(len(page.evaluate('getCurrentFilter()')['rules']), 1)
                page.goto(server.url)
                self.assertEqual(len(page.evaluate('getCurrentFilter()')['rules']), 1)
                self.assertEqual(errors, [])
            finally: browser.close()


if __name__ == '__main__':
    unittest.main()
