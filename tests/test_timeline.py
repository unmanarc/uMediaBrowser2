"""Integration tests against a real server and temporary media library.

Run: python3 tests/test_timeline.py build/timeline/uMediaBrowser2
Requires ImageMagick's convert and node.
"""
from contextlib import contextmanager
import datetime
import hashlib
import http.cookiejar
import json
import os
from pathlib import Path
import shutil
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.parse
import urllib.request

BINARY = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else Path('build/timeline/uMediaBrowser2').resolve()


@contextmanager
def sqlite_connection(path):
    """Commit fixture writes and close the connection, including WAL readers."""
    connection = sqlite3.connect(path)
    try:
        with connection:
            yield connection
    finally:
        connection.close()


class TimelineTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='umedia-timeline-')
        # Special characters exercise URI escaping; the DB sits two parents above rootdir.
        special = Path(cls.temp.name) / 'database #?% ñ'
        cls.library = special / 'library & ñ #?%+'
        cls.root = cls.library / 'Familiar'
        cls.root.mkdir(parents=True)
        cls.database = special / 'digikam4.db'
        with sqlite_connection(cls.database) as db:
            db.executescript('''
                CREATE TABLE AlbumRoots(id INTEGER PRIMARY KEY, identifier TEXT, specificPath TEXT);
                CREATE TABLE Albums(id INTEGER PRIMARY KEY, albumRoot INTEGER, relativePath TEXT, UNIQUE(albumRoot, relativePath));
                CREATE TABLE Images(id INTEGER PRIMARY KEY, album INTEGER, name TEXT, modificationDate DATETIME, fileSize INTEGER);
                CREATE TABLE ImageInformation(imageid INTEGER PRIMARY KEY, creationDate DATETIME);
            ''')
            identifier = 'volumeid:?path=' + urllib.parse.quote(str(cls.library), safe='/') + '&fileuuid=test'
            db.execute('INSERT INTO AlbumRoots VALUES(1, ?, ?)', (identifier, '/'))
            db.execute('INSERT INTO AlbumRoots VALUES(2, ?, ?)', ('volumeid:?path='+str(cls.temp.name), '/other'))
        cls.base = Path(cls.temp.name) / 'base.jpg'
        subprocess.run(['convert', '-size', '96x64', 'gradient:#3b82f6-#f6c945', str(cls.base)], check=True)
        cls.photo('A beach/same.jpg', 1_700_000_000)
        cls.photo('B meal/same.jpg', 1_700_000_000 + 30*60 - 1)  # just below the inactivity threshold
        cls.photo('B meal/older.jpg', 1_700_000_000 - 30*60)  # exactly the threshold starts a new event
        for n in range(65):
            cls.photo(f'Large/{n:03}.jpg', 1_800_000_000)
        cls.photo('Other/duplicate.jpg', 1_800_000_000)
        # Enough sparse events to test feed pagination and exhaustion.
        for n in range(14):
            cls.photo(f'Sparse/{n}.jpg', 1_500_000_000 - n*86400)
        cls.photo('Dates & unicode ñ/original.jpg', 1_580_733_296)
        cls.photo('Dates & unicode ñ/create.jpg', 1_580_733_356)
        cls.photo('Dates & unicode ñ/dated.jpg', 1_400_000_000)
        cls.photo('Dates & unicode ñ/historical.jpg', -154_697_104)
        cls.photo('missing.jpg', 1_800_000_000).unlink()
        cls.photo('invalid-date.jpg', 1_800_000_000)
        with sqlite_connection(cls.database) as db:
            db.execute("UPDATE Images SET modificationDate='bad date' WHERE name='invalid-date.jpg'")
        (cls.root/'ignored.txt').write_text('not media')
        cls.photo('unindexed.jpg', 1_800_000_000, indexed=False)
        # Images.album is nullable in digiKam; an orphan must not inherit the previous row's album.
        with sqlite_connection(cls.database) as db:
            db.execute('INSERT INTO Images(album,name,modificationDate,fileSize) VALUES(NULL,?,?,?)',
                       ('unindexed.jpg', '2025-01-01T00:00:00', 123))
        (cls.root/'escape.jpg').symlink_to(cls.base)
        cls.register('escape.jpg', 1_800_000_000)
        (cls.root/'loop').symlink_to(cls.root, target_is_directory=True)
        cls.register('loop/A beach/same.jpg', 1_800_000_000)
        # Same-prefix sibling, parent album and a separate collection must stay out.
        for relative, collection in [('/FamiliarExtra', 1), ('/', 1), ('/', 2)]:
            with sqlite_connection(cls.database) as db:
                db.execute('INSERT INTO Albums(albumRoot, relativePath) VALUES(?, ?)', (collection, relative))
                album = db.execute('SELECT last_insert_rowid()').fetchone()[0]
                db.execute('INSERT INTO Images(album,name,modificationDate,fileSize) VALUES(?,?,?,?)',
                           (album, 'outside.jpg', '2026-01-01T00:00:00', 10))
        (cls.library/'FamiliarExtra').mkdir()
        shutil.copyfile(cls.base, cls.library/'FamiliarExtra/outside.jpg')
        shutil.copyfile(cls.base, cls.library/'outside.jpg')
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0)); port = sock.getsockname()[1]
        cls.url = f'http://127.0.0.1:{port}/'
        cls.cookies = http.cookiejar.CookieJar()
        cls.client = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(cls.cookies))
        cls.log = open(Path(cls.temp.name)/'server.log', 'w+')
        cls.process = subprocess.Popen([str(BINARY), '--rootdir', str(cls.root), '-a', '127.0.0.1', '-l', str(port), '-o', '-u', '--title', '<Test & library>'],
                                       stdout=cls.log, stderr=cls.log, env=dict(os.environ, TZ='UTC'))
        for _ in range(200):
            if cls.process.poll() is not None:
                cls.log.seek(0); raise RuntimeError(cls.log.read())
            try:
                cls.get('view=timeline'); break
            except OSError: time.sleep(.05)
        else: raise RuntimeError('Server did not start')

    @classmethod
    def photo(cls, name, timestamp, indexed=True):
        path = cls.root/name
        path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(cls.base, path)
        os.utime(path, (1_900_000_000, 1_900_000_000))  # deliberately different from digiKam
        if indexed: cls.register(name, timestamp)
        return path

    @classmethod
    def register(cls, name, timestamp):
        relative = '/Familiar'
        if str(Path(name).parent) != '.': relative += '/' + str(Path(name).parent)
        date = datetime.datetime.fromtimestamp(timestamp, datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%S.000')
        with sqlite_connection(cls.database) as db:
            db.execute('INSERT OR IGNORE INTO Albums(albumRoot,relativePath) VALUES(1,?)', (relative,))
            album = db.execute('SELECT id FROM Albums WHERE albumRoot=1 AND relativePath=?', (relative,)).fetchone()[0]
            db.execute('INSERT INTO Images(album,name,modificationDate,fileSize) VALUES(?,?,?,?)',
                       (album, Path(name).name, date, cls.base.stat().st_size))

    @classmethod
    def get(cls, query):
        return cls.client.open(cls.url + '?' + query, timeout=20).read()

    @classmethod
    def api(cls, **params):
        return json.loads(cls.get(urllib.parse.urlencode(params)))['data']

    @classmethod
    def refresh(cls):
        token = next(cookie.value for cookie in cls.cookies if cookie.name == 'CSRFToken')
        request = urllib.request.Request(cls.url+'?act=timeline_refresh', method='POST', headers={'X-CSRFToken':token})
        return json.load(cls.client.open(request, timeout=20))['data']

    @classmethod
    def all_events(cls):
        page = cls.api(act='timeline')
        events = page['events'][:]
        while page['has_more']:
            page = cls.api(act='timeline', cursor=page['next'], revision=page['revision'])
            events.extend(page['events'])
        return events

    def test_pages_and_samples(self):
        page = self.api(act='timeline')
        self.assertEqual(len(page['events']), 8)
        self.assertTrue(page['has_more'])
        events = self.all_events()
        self.assertEqual(len(events), 20)
        self.assertEqual([e['end'] for e in events], sorted((e['end'] for e in events), reverse=True))
        paths = []
        for event in events:
            self.assertLessEqual(len(event['samples']), 6)
            self.assertEqual(len({i['path'] for i in event['samples']}), len(event['samples']))
            paths.extend(i['path'] for i in self.api(act='timeline_event', event=event['id'])['items'])
        self.assertNotIn('/escape.jpg', paths)
        self.assertFalse(any(p.startswith('/loop/') for p in paths))
        self.assertNotIn('/ignored.txt', paths)
        self.assertNotIn('/missing.jpg', paths)
        self.assertNotIn('/invalid-date.jpg', paths)
        self.assertNotIn('/unindexed.jpg', paths)
        self.assertNotIn('/outside.jpg', paths)
        self.assertTrue(page['database_available'])
        self.assertEqual(page['error'], '')
        self.assertEqual(page['gap_seconds'], 1800)
        big = next(e for e in events if e['count'] == 66)
        first = self.api(act='timeline_event', event=big['id'])
        self.assertEqual(len(first['items']), 60)
        self.assertTrue(first['has_more'])
        second = self.api(act='timeline_event', event=big['id'], cursor=first['next'])
        self.assertEqual(len(second['items']), 6)
        self.assertFalse(second['has_more'])
        self.assertEqual(second['items'][-1]['folder'], '/Other')
        # The carousel can request an item beyond the loaded gallery page.
        one = self.api(act='timeline_event', event=big['id'], cursor=65, limit=1)
        self.assertEqual(one['items'][0]['path'], '/Other/duplicate.jpg')
        draws = [self.api(act='timeline_samples', event=big['id'])['samples'] for _ in range(3)]
        self.assertTrue(all(len(draw) == 6 for draw in draws))
        self.assertNotEqual(draws[0], draws[1])
        across = next(e for e in events if e['count'] == 2 and e['end'] == 1_700_000_000 + 30*60 - 1)
        self.assertEqual(across['start'], 1_700_000_000)
        files = self.api(act='timeline_event', event=across['id'])['items']
        self.assertEqual([i['folder'] for i in files], ['/A beach', '/B meal'])
        self.assertEqual([i['name'] for i in files], ['same.jpg','same.jpg'])
        self.assertTrue(all(i['timestamp'] == i['mtime'] and i['date_source'] == 'modificationDate' for i in files))

    def test_digikam_dates_and_read_only_database(self):
        before = hashlib.sha256(self.database.read_bytes()).hexdigest()
        mode = self.database.stat().st_mode
        self.database.chmod(0o444)
        try:
            self.refresh()
            events = self.all_events()
            event = next(e for e in events if any(i['name'] == 'original.jpg' for i in e['samples']))
            items = self.api(act='timeline_event', event=event['id'])['items']
            original = next(i for i in items if i['name'] == 'original.jpg')
            created = next(i for i in items if i['name'] == 'create.jpg')
            self.assertEqual(original['date_source'], 'modificationDate')
            self.assertEqual(original['timestamp'], 1_580_733_296)
            self.assertNotEqual(original['timestamp'], (self.root/original['path'].lstrip('/')).stat().st_mtime)
            self.assertEqual(created['timestamp'] - original['timestamp'], 60)
            historical = next(e for e in events if e['samples'][0]['name'] == 'historical.jpg')['samples'][0]
            self.assertEqual(historical['timestamp'], -154_697_104)
            self.assertEqual(hashlib.sha256(self.database.read_bytes()).hexdigest(), before)
        finally:
            self.database.chmod(mode)

    def test_filename_creation_and_modification_date_priority(self):
        def stamp(date):
            value = datetime.datetime.fromisoformat(date)
            if value.tzinfo is None: value = value.replace(tzinfo=datetime.timezone.utc)
            return int(value.timestamp())

        modified = '2031-10-17T10:40:00'
        created = '2020-02-03T12:34:56.123'
        # Different capture, metadata and processing dates expose the precedence.
        cases = [
            ('IMG_20170706-125641_001_.jpg', created, '2017-07-06T12:56:41', 'filename'),
            ('IMG_20240229_230405_0000_Canon-EOS-M50.CR3', created, '2024-02-29T23:04:05', 'filename'),
            ('img_19650203-123456-edited.jpg', created, '1965-02-03T12:34:56', 'filename'),
            ('IMG-20140128-WA0002.jpg', created, '2014-01-28T00:00:00', 'filename'),
            ('IMG-20161014-WA0026 (1).jpg', created, '2016-10-14T00:00:00', 'filename'),
            ('IMG-20140717-WA0003~2.jpg', created, '2014-07-17T00:00:00', 'filename'),
            ('IMG_20240229.jpg', created, '2024-02-29T00:00:00', 'filename'),
            ('IMG_1234.JPG', created, created, 'creationDate'),
            ('IMG_183061182304648.jpeg', created, created, 'creationDate'),
            ('holiday-20170706-125641.jpg', created, created, 'creationDate'),
            ('IMG_20230229-123456.jpg', created, created, 'creationDate'),  # non-leap year
            ('IMG_20240431-123456.jpg', created, created, 'creationDate'),  # impossible day
            ('IMG_20241301-123456.jpg', created, created, 'creationDate'),
            ('IMG_20240229-250000.jpg', created, created, 'creationDate'),
            ('IMG_20240229-126000.jpg', created, created, 'creationDate'),
            ('IMG_20240229-123460.jpg', created, created, 'creationDate'),
            ('IMG_20240229-1234567.jpg', created, created, 'creationDate'),  # longer counter
            ('IMG_00000101-123456.jpg', created, created, 'creationDate'),
            ('with-offset.jpg', '2025-01-01T03:04:05+03:00', '2025-01-01T00:04:05', 'creationDate'),
            ('historical-capture.jpg', '1965-02-03T12:34:56.000', '1965-02-03T12:34:56', 'creationDate'),
            ('no-information-row.jpg', None, modified, 'modificationDate'),
            ('null-creation.jpg', None, modified, 'modificationDate'),
            ('empty-creation.jpg', '', modified, 'modificationDate'),
            ('invalid-creation.jpg', 'bad date', modified, 'modificationDate'),
            ('invalid-creation-day.jpg', '2023-02-29T12:00:00', modified, 'modificationDate'),
            ('IMG_20241301-123456-both-invalid.jpg', 'bad date', modified, 'modificationDate'),
        ]
        files = []
        try:
            for name, creation, expected, source in cases:
                files.append(self.photo('Priority/'+name, stamp(modified)))
                if name != 'no-information-row.jpg':
                    with sqlite_connection(self.database) as db:
                        imageid = db.execute('SELECT id FROM Images WHERE name=?', (name,)).fetchone()[0]
                        db.execute('INSERT INTO ImageInformation(imageid,creationDate) VALUES(?,?)', (imageid, creation))
            # A valid filename also works when neither database date is usable.
            valid_only = 'IMG_20180709-101112-no-db-date.jpg'
            files.append(self.photo('Priority/'+valid_only, stamp(modified)))
            with sqlite_connection(self.database) as db:
                db.execute('UPDATE Images SET modificationDate=NULL WHERE name=?', (valid_only,))
            cases.append((valid_only, None, '2018-07-09T10:11:12', 'filename'))
            original_db = self.database.read_bytes()
            found = {}
            events = self.all_events()
            for event in events:
                cursor = 0
                while True:
                    page = self.api(act='timeline_event', event=event['id'], cursor=cursor)
                    for item in page['items']:
                        if item['folder'] == '/Priority': found[item['name']] = item
                    if not page['has_more']: break
                    cursor = page['next']
            for name, creation, expected, source in cases:
                with self.subTest(name=name):
                    self.assertIn(name, found)
                    item = found[name]
                    self.assertEqual(item['timestamp'], stamp(expected))
                    self.assertEqual(item['date_source'], source)
                    self.assertEqual(item['mtime'], 0 if name == valid_only else stamp(modified))
            # Feed order and event membership must use the selected capture date too.
            self.assertEqual([e['end'] for e in events], sorted((e['end'] for e in events), reverse=True))
            self.assertEqual(self.database.read_bytes(), original_db)
        finally:
            for path in files: path.unlink()
            with sqlite_connection(self.database) as db:
                db.execute("DELETE FROM ImageInformation WHERE imageid IN (SELECT i.id FROM Images i JOIN Albums a ON a.id=i.album WHERE a.relativePath='/Familiar/Priority')")
                db.execute("DELETE FROM Images WHERE album IN (SELECT id FROM Albums WHERE relativePath='/Familiar/Priority')")

    def test_live_database_changes_without_refresh(self):
        before = self.api(act='timeline')
        photo = self.photo('Live/live.jpg', 2_000_000_000)
        try:
            page = self.api(act='timeline')  # no refresh request or server restart
            self.assertEqual(page['total_items'], before['total_items'] + 1)
            self.assertNotEqual(page['revision'], before['revision'])
            self.assertEqual(page['events'][0]['samples'][0]['path'], '/Live/live.jpg')
            for params in [dict(act='timeline', cursor=before['next'], revision=before['revision']),
                           dict(act='timeline_event', event=before['events'][0]['id'])]:
                with self.assertRaises(urllib.error.HTTPError) as error: self.api(**params)
                self.assertEqual(error.exception.code, 409)
                error.exception.close()
            photo.unlink()
            self.assertEqual(self.api(act='timeline')['total_items'], before['total_items'])
        finally:
            if photo.exists(): photo.unlink()
            with sqlite_connection(self.database) as db:
                db.execute("DELETE FROM Images WHERE name='live.jpg'")

    def test_missing_after_previous_query_is_never_sent(self):
        events = self.all_events()
        event = next(e for e in events if e['count'] == 1 and e['samples'][0]['name'] == 'dated.jpg')
        path = self.root/'Dates & unicode ñ/dated.jpg'
        saved = path.read_bytes()
        before = self.database.read_bytes()
        path.unlink()
        try:
            self.assertEqual(self.api(act='timeline_samples', event=event['id'])['samples'], [])
            self.assertEqual(self.api(act='timeline_event', event=event['id'])['items'], [])
            self.assertEqual(self.database.read_bytes(), before)
        finally:
            path.write_bytes(saved)

    def test_missing_files_leave_no_pagination_holes(self):
        event = next(e for e in self.all_events() if e['count'] == 66)
        path = self.root/'Large/000.jpg'
        saved = path.read_bytes()
        path.unlink()
        try:
            page = self.api(act='timeline_event', event=event['id'], cursor=59)
            self.assertEqual(page['count'], 65)
            self.assertEqual(len(page['items']), 6)
            self.assertEqual([i['index'] for i in page['items']], list(range(59, 65)))
            self.assertEqual(page['items'][-1]['path'], '/Other/duplicate.jpg')
            self.assertFalse(page['has_more'])
            feed = self.api(act='timeline')
            self.assertTrue(any(e['count'] == 65 for e in feed['events']))
        finally:
            path.write_bytes(saved)

    def test_nearest_database_takes_precedence(self):
        nearest = self.root/'digikam4.db'
        photo = self.photo('nearest.jpg', 1_800_000_000, indexed=False)
        original = self.database.read_bytes()
        shutil.copyfile(self.database, nearest)
        try:
            with sqlite_connection(nearest) as db:
                db.execute('DELETE FROM Images')
                album = db.execute("SELECT id FROM Albums WHERE relativePath='/Familiar' AND albumRoot=1").fetchone()[0]
                db.execute('INSERT INTO Images(album,name,modificationDate,fileSize) VALUES(?,?,?,?)',
                           (album, photo.name, '2025-01-01T00:00:00', 123))
            page = self.refresh()
            self.assertTrue(page['database_available'])
            self.assertEqual(page['total_items'], 1)
            item = page['events'][0]['samples'][0]
            self.assertEqual(item['path'], '/nearest.jpg')
            self.assertEqual(item['size'], 123)  # database value, not a filesystem stat
            self.assertEqual(self.database.read_bytes(), original)
        finally:
            nearest.unlink()
            photo.unlink()
            self.refresh()

    def test_root_album_and_database_updates(self):
        photo = self.photo('root-album.jpg', 1_800_000_000)
        try:
            page = self.refresh()
            event = next(e for e in page['events'] if e['count'] == 67)
            files = self.api(act='timeline_event', event=event['id'])['items']
            self.assertIn('/root-album.jpg', [i['path'] for i in files])
            with sqlite_connection(self.database) as db:
                db.execute("UPDATE Images SET modificationDate='2025-01-01T03:04:05+03:00' WHERE name='root-album.jpg'")
            page = self.refresh()
            item = next(i for e in page['events'] for i in e['samples'] if i['path'] == '/root-album.jpg')
            self.assertEqual(item['path'], '/root-album.jpg')
            self.assertEqual(item['timestamp'], 1_735_689_845)
        finally:
            photo.unlink()
            with sqlite_connection(self.database) as db:
                db.execute("DELETE FROM Images WHERE name='root-album.jpg'")
            self.refresh()

    def test_unavailable_database_leaves_folders_working(self):
        saved = self.database.with_suffix('.saved')
        original = self.database.read_bytes()
        self.database.rename(saved)
        try:
            for corrupt in (False, True):
                if corrupt: self.database.write_bytes(b'not a sqlite database')
                page = self.refresh()
                self.assertFalse(page['database_available'])
                self.assertTrue(page['error'])
                self.assertEqual(page['total_items'], 0)
                self.assertIn('unindexed.jpg', [i['name'] for i in self.api(act='ls', path='/')['items']])
            self.assertEqual(self.database.read_bytes(), b'not a sqlite database')
        finally:
            if self.database.exists(): self.database.unlink()
            saved.rename(self.database)
            self.assertEqual(self.database.read_bytes(), original)
            self.refresh()

    def test_command_line_group_threshold(self):
        root = self.library/'Threshold'
        root.mkdir()
        with sqlite_connection(self.database) as db:
            db.execute("INSERT INTO Albums(albumRoot,relativePath) VALUES(1,'/Threshold')")
            album = db.execute('SELECT last_insert_rowid()').fetchone()[0]
            for index, seconds in enumerate((0, 1799, 3599)):
                name = f'{index}.jpg'
                shutil.copyfile(self.base, root/name)
                date = datetime.datetime.fromtimestamp(1_700_000_000+seconds, datetime.timezone.utc).isoformat()
                db.execute('INSERT INTO Images(album,name,modificationDate,fileSize) VALUES(?,?,?,?)',
                           (album, name, date, 123))
        # The gap is measured between adjacent images, not from the group's start.
        for minutes, expected in [(None, 2), ('10', 3), ('60', 1)]:
            with self.subTest(minutes=minutes), socket.socket() as sock:
                sock.bind(('127.0.0.1', 0))
                port = sock.getsockname()[1]
            args = [str(BINARY), '--rootdir', str(root), '-a', '127.0.0.1', '-l', str(port)]
            if minutes is not None: args += ['--timeline-gap-minutes', minutes]
            process = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                       env=dict(os.environ, TZ='UTC'))
            try:
                for _ in range(200):
                    if process.poll() is not None: self.fail('Configured server failed to start')
                    try:
                        with urllib.request.urlopen(f'http://127.0.0.1:{port}/?act=timeline', timeout=2) as response:
                            page = json.load(response)['data']
                        break
                    except OSError: time.sleep(.025)
                else: self.fail('Configured server did not start')
                self.assertEqual(page['total_items'], 3)
                self.assertEqual(page['total_events'], expected)
                self.assertEqual(page['gap_seconds'], int(minutes or '30')*60)
            finally:
                process.terminate()
                try: process.wait(timeout=5)
                except subprocess.TimeoutExpired: process.kill(); process.wait()
        for invalid in ('0', '-1', '1.5', 'abc', '99999999999999999999999'):
            result = subprocess.run([str(BINARY), '--timeline-gap-minutes', invalid],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b'Invalid --timeline-gap-minutes', result.stdout+result.stderr)

    def test_validation_and_stale_cursors(self):
        for params in [dict(act='timeline',cursor='-1'), dict(act='timeline',limit='no'),dict(act='timeline',cursor=1),dict(act='timeline_event',event='../../')]:
            with self.assertRaises(urllib.error.HTTPError) as error: self.api(**params)
            self.assertIn(error.exception.code, (400,409))
            error.exception.close()
        old = self.api(act='timeline')
        new = self.refresh()
        self.assertNotEqual(old['revision'], new['revision'])
        with self.assertRaises(urllib.error.HTTPError) as error:
            self.api(act='timeline',cursor=old['next'],revision=old['revision'])
        self.assertEqual(error.exception.code,409)
        error.exception.close()
        with self.assertRaises(urllib.error.HTTPError) as error:
            self.api(act='timeline_event',event=old['events'][0]['id'])
        self.assertEqual(error.exception.code,409)
        error.exception.close()
        with self.assertRaises(urllib.error.HTTPError) as error: self.api(act='timeline_refresh')
        self.assertEqual(error.exception.code,405)
        error.exception.close()
        request = urllib.request.Request(self.url+'?act=timeline_refresh',method='POST')
        with self.assertRaises(urllib.error.HTTPError) as error: self.client.open(request)
        self.assertEqual(error.exception.code,401)
        error.exception.close()

    def test_html_pages(self):
        folders = self.get('').decode()
        timeline = self.get('view=timeline').decode()
        self.assertIn('id="filesGrid"',folders)
        self.assertNotIn('id="timelineApp"',folders)
        self.assertIn('href="?view=timeline"',folders)
        self.assertIn('id="timelineApp"',timeline)
        self.assertNotIn('id="filesGrid"',timeline)
        for html in (folders,timeline):
            self.assertIn('&lt;Test &amp; library&gt;',html)
            self.assertNotIn('__SHARED_',html)
            script = html.split('<script>')[1].split('</script>')[0]
            subprocess.run(['node','--check','-'],input=script.encode(),check=True,capture_output=True)

    def test_wal_commits_are_visible_and_read_only(self):
        writer = sqlite3.connect(self.database)
        photo = None
        try:
            self.assertEqual(writer.execute('PRAGMA journal_mode=WAL').fetchone()[0], 'wal')
            before = self.api(act='timeline')
            photo = self.photo('Live/wal.jpg', 2_000_000_000, indexed=False)
            writer.execute("INSERT OR IGNORE INTO Albums(albumRoot,relativePath) VALUES(1,'/Familiar/Live')")
            album = writer.execute("SELECT id FROM Albums WHERE albumRoot=1 AND relativePath='/Familiar/Live'").fetchone()[0]
            writer.execute('INSERT INTO Images(album,name,modificationDate,fileSize) VALUES(?,?,?,?)',
                           (album, 'wal.jpg', '2033-05-18T03:33:20', 123))
            writer.commit()  # keep this WAL writer open while the server reads
            wal = Path(str(self.database)+'-wal')
            original_db, original_wal = self.database.read_bytes(), wal.read_bytes()
            page = self.api(act='timeline')
            self.assertEqual(page['total_items'], before['total_items'] + 1)
            self.assertNotEqual(page['revision'], before['revision'])
            event = page['events'][0]
            self.assertEqual(self.api(act='timeline_event', event=event['id'])['items'][0]['path'], '/Live/wal.jpg')
            self.assertEqual(self.database.read_bytes(), original_db)
            self.assertEqual(wal.read_bytes(), original_wal)
        finally:
            if photo is not None and photo.exists(): photo.unlink()
            writer.execute("DELETE FROM Images WHERE name='wal.jpg'")
            writer.commit()
            writer.execute('PRAGMA journal_mode=DELETE').fetchone()
            writer.close()

    def test_y_upload_and_delete_advance_revision(self):
        before = self.api(act='timeline')['total_items']
        token = next(cookie.value for cookie in self.cookies if cookie.name == 'CSRFToken')
        boundary = 'umedia-test-boundary'
        body = (f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="uploaded.jpg"\r\n'
                'Content-Type: image/jpeg\r\n\r\n').encode() + self.base.read_bytes() + f'\r\n--{boundary}--\r\n'.encode()
        request = urllib.request.Request(self.url+'?act=upload&path=%2F', data=body, method='POST',
                                        headers={'X-CSRFToken':token,'Content-Type':'multipart/form-data; boundary='+boundary})
        with self.client.open(request) as response:
            self.assertTrue(json.load(response)['ok'])
        self.assertEqual(self.api(act='timeline')['total_items'],before)
        self.assertIn('uploaded.jpg', [i['name'] for i in self.api(act='ls',path='/')['items']])
        token = next(cookie.value for cookie in self.cookies if cookie.name == 'CSRFToken')
        request = urllib.request.Request(self.url+'?act=delete&files=%2Fuploaded.jpg',method='POST',headers={'X-CSRFToken':token})
        with self.client.open(request) as response:
            self.assertTrue(json.load(response)['ok'])
        self.assertEqual(self.api(act='timeline')['total_items'],before)

    def test_z_refresh_filesystem_changes(self):
        before = self.api(act='timeline')['total_items']
        new_file = self.photo('New/new.jpg', 2_000_000_000, indexed=False)
        self.assertEqual(self.refresh()['total_items'],before)
        self.register('New/new.jpg', 2_000_000_000)
        self.assertEqual(self.refresh()['total_items'],before+1)
        new_file.unlink()
        self.assertEqual(self.refresh()['total_items'],before)

    @classmethod
    def tearDownClass(cls):
        cls.process.terminate()
        try: cls.process.wait(timeout=5)
        except subprocess.TimeoutExpired: cls.process.kill(); cls.process.wait()
        cls.log.close(); cls.temp.cleanup()


if __name__ == '__main__':
    unittest.main()
