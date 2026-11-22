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
                CREATE TABLE Tags(id INTEGER PRIMARY KEY, pid INTEGER, name TEXT, icon INTEGER, iconkde TEXT);
                CREATE TABLE ImageTagProperties(imageid INTEGER, tagid INTEGER, property TEXT, value TEXT);
                CREATE TABLE ImageTags(imageid INTEGER, tagid INTEGER, UNIQUE(imageid, tagid));
                CREATE INDEX image_tags_image ON ImageTags(imageid);
                CREATE TABLE ImageMetadata(imageid INTEGER PRIMARY KEY, make TEXT, model TEXT, lens TEXT,
                    aperture REAL, focalLength REAL, focalLength35 REAL, exposureTime REAL,
                    exposureProgram INTEGER, exposureMode INTEGER, sensitivity INTEGER, flash INTEGER,
                    whiteBalance INTEGER, whiteBalanceColorTemperature INTEGER, meteringMode INTEGER,
                    subjectDistance REAL, subjectDistanceCategory INTEGER);
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
    def refresh(cls, action='timeline_refresh'):
        token = next(cookie.value for cookie in cls.cookies if cookie.name == 'CSRFToken')
        request = urllib.request.Request(cls.url+'?'+urllib.parse.urlencode(dict(act=action)), method='POST', headers={'X-CSRFToken':token})
        return json.load(cls.client.open(request, timeout=20))['data']

    @classmethod
    def all_events(cls):
        page = cls.api(act='timeline')
        events = page['events'][:]
        while page['has_more']:
            page = cls.api(act='timeline', cursor=page['next'], revision=page['revision'])
            events.extend(page['events'])
        return events

    @staticmethod
    def filter_json(*rules, match='all'):
        """Encode the same JSON contract used by the timeline editor."""
        return json.dumps(dict(rules=[dict(match=match, rules=rules)] if match == 'any' else rules))

    @contextmanager
    def filter_library(self):
        """Create independent tags, dates and camera metadata, then remove all fixture rows.

        Filenames/modification dates deliberately disagree with creationDate. Two
        tags with the same name exercise EXISTS deduplication across tag branches.
        """
        files = []
        records = [
            ('IMG_20300101-100000-a.jpg', '2020-02-29T12:00:00', 'Canon', 'EOS R5', 'RF 24-70%', 100, 2.8),
            ('b.jpg', '2021-01-01T00:00:00', 'Nikon', 'Z_8', '50 mm', 800, 5.6),
            ('c.jpg', None, None, None, None, None, None),
            ('d.jpg', 'bad date', 'CANON', 'EOS R6', 'RF 24-70%', 400, 4.0),
            ('e.jpg', '2020-03-01T03:00:00+03:00', 'Sony', 'ILCE', '35 mm', 200, 1.8),
        ]
        try:
            for index, (name, creation, make, model, lens, iso, aperture) in enumerate(records):
                files.append(self.photo('Filters/'+name, 1_850_000_000 + index*100))
                with sqlite_connection(self.database) as db:
                    imageid = db.execute("SELECT id FROM Images WHERE album=(SELECT id FROM Albums WHERE relativePath='/Familiar/Filters') AND name=?", (name,)).fetchone()[0]
                    if index != 2:
                        db.execute('INSERT INTO ImageInformation(imageid,creationDate) VALUES(?,?)', (imageid, creation))
                        db.execute('INSERT INTO ImageMetadata(imageid,make,model,lens,sensitivity,aperture,exposureTime) VALUES(?,?,?,?,?,?,?)',
                                   (imageid, make, model, lens, iso, aperture, .008))
                    if index in (0, 1):
                        for branch in (9101, 9102):
                            db.execute('INSERT OR IGNORE INTO Tags(id,pid,name) VALUES(?,?,?)', (branch, branch+10, 'Holiday'))
                            db.execute('INSERT INTO ImageTags(imageid,tagid) VALUES(?,?)', (imageid, branch))
                    db.execute('INSERT OR IGNORE INTO Tags(id,pid,name) VALUES(9103,0,?)', ("O'Brien %_ ñ",))
                    if index == 4: db.execute('INSERT INTO ImageTags(imageid,tagid) VALUES(?,9103)', (imageid,))
                    for tagid, tag, indexes in [(9104, 'Aaron', (0, 1)), (9105, 'Esther', (1, 4))]:
                        db.execute('INSERT OR IGNORE INTO Tags(id,pid,name) VALUES(?,0,?)', (tagid, tag))
                        if index in indexes: db.execute('INSERT INTO ImageTags(imageid,tagid) VALUES(?,?)', (imageid, tagid))
            yield records
        finally:
            for path in files: path.unlink(missing_ok=True)
            with sqlite_connection(self.database) as db:
                for table in ('ImageTags', 'ImageMetadata', 'ImageInformation'):
                    db.execute(f"DELETE FROM {table} WHERE imageid IN (SELECT i.id FROM Images i JOIN Albums a ON a.id=i.album WHERE a.relativePath='/Familiar/Filters')")
                db.execute("DELETE FROM Images WHERE album IN (SELECT id FROM Albums WHERE relativePath='/Familiar/Filters')")
                db.execute('DELETE FROM Tags WHERE id IN (9101,9102,9103,9104,9105)')

    def filtered_items(self, filter_json):
        """Traverse filtered event/feed pagination with the same JSON on every call."""
        page = self.api(act='timeline', filter=filter_json, limit=1)
        self.assertTrue(page['database_available'], page['error'])
        items = []
        while True:
            for event in page['events']:
                cursor = 0
                while True:
                    event_page = self.api(act='timeline_event', event=event['id'], cursor=cursor, limit=1, filter=filter_json)
                    self.assertEqual([item['index'] for item in event_page['items']], list(range(cursor, event_page['next'])))
                    items.extend(event_page['items'])
                    if not event_page['has_more']: break
                    cursor = event_page['next']
            if not page['has_more']: break
            page = self.api(act='timeline', filter=filter_json, limit=1, cursor=page['next'], revision=page['revision'])
        self.assertEqual(len(items), page['total_items'])
        return items

    def test_filters_tags_negation_and_literal_values(self):
        with self.filter_library():
            original = self.database.read_bytes()
            tagged = self.filter_json(dict(field='tag', op='eq', value='Holiday'))
            self.assertEqual({i['name'] for i in self.filtered_items(tagged)}, {'IMG_20300101-100000-a.jpg', 'b.jpg'})
            total = self.api(act='timeline')['total_items']
            negative = self.filter_json(dict(field='tag', op='eq', value='Holiday', negate=True))
            files = self.filtered_items(negative)
            self.assertEqual(len(files), total-2)
            self.assertIn('c.jpg', {i['name'] for i in files})
            for text, expected in [("O'Brien %_ ñ", {'e.jpg'}), ('holiday', set()), ("Holiday' OR 1=1 --", set()), ('%_', set())]:
                with self.subTest(tag=text):
                    actual = self.filtered_items(self.filter_json(dict(field='tag', op='eq', value=text)))
                    self.assertEqual({i['name'] for i in actual}, expected)
            self.assertEqual(self.database.read_bytes(), original)
            self.assertIn('unindexed.jpg', [i['name'] for i in self.api(act='ls', path='/', filter=tagged)['items']])

    def test_filter_sql_payloads_are_literal_bound_values(self):
        """Exercise real SQL binding, including payloads that actually exist as data."""
        with self.filter_library():
            payloads = ["' OR 1=1 --", "') OR 1=1 /*", "'; DROP TABLE Images; --",
                        '\"; SELECT 1; --', "O'Brien %_ ñ", r"\' OR 1=1 --",
                        ':filter0 :root ? @value $value', 'line\n--comment\n/*text*/']
            for payload in payloads:
                with sqlite_connection(self.database) as db:
                    imageid = db.execute("SELECT id FROM Images WHERE name='e.jpg'").fetchone()[0]
                    db.execute('UPDATE Tags SET name=? WHERE id=9103', (payload,))
                    db.execute('UPDATE ImageMetadata SET make=?,model=?,lens=? WHERE imageid=?',
                               (payload, payload, payload, imageid))
                original = self.database.read_bytes()
                for field in ('tag', 'make', 'model', 'lens'):
                    leaf = dict(field=field, op='eq' if field == 'tag' else 'contains', value=payload)
                    missing = dict(field='tag', op='eq', value='never-present-tag')
                    queries = [self.filter_json(leaf),
                               self.filter_json(dict(match='any', rules=[missing, leaf])),
                               self.filter_json(dict(match='any', rules=[leaf, missing]), missing)]
                    for query, expected in zip(queries, [{'e.jpg'}, {'e.jpg'}, set()]):
                        with self.subTest(field=field, payload=payload, query=query):
                            self.assertEqual({i['name'] for i in self.filtered_items(query)}, expected)
                self.assertEqual(self.database.read_bytes(), original)

    def test_filters_metadata_operators_and_boolean_combinations(self):
        with self.filter_library():
            cases = [
                ([dict(field='make', op='eq', value='Canon')], 'all', {'IMG_20300101-100000-a.jpg'}),
                ([dict(field='model', op='eq', value='Z_8')], 'all', {'b.jpg'}),
                ([dict(field='model', op='eq', value='Z')], 'all', set()),
                ([dict(field='lens', op='eq', value='RF 24-70%')], 'all', {'IMG_20300101-100000-a.jpg','d.jpg'}),
                ([dict(field='make', op='contains', value='canon')], 'all', {'IMG_20300101-100000-a.jpg','d.jpg'}),
                ([dict(field='lens', op='contains', value='%')], 'all', {'IMG_20300101-100000-a.jpg','d.jpg'}),
                ([dict(field='model', op='contains', value='_')], 'all', {'b.jpg'}),
                ([dict(field='sensitivity', op='gt', value=400)], 'all', {'b.jpg'}),
                ([dict(field='sensitivity', op='lt', value=200)], 'all', {'IMG_20300101-100000-a.jpg'}),
                ([dict(field='exposureTime', op='eq', value=.008)], 'all', {'IMG_20300101-100000-a.jpg','b.jpg','d.jpg','e.jpg'}),
                ([dict(field='make', op='contains', value='Canon'), dict(field='aperture', op='lt', value=3)], 'all', {'IMG_20300101-100000-a.jpg'}),
                ([dict(field='make', op='contains', value='Nikon'), dict(field='aperture', op='lt', value=2)], 'any', {'b.jpg','e.jpg'}),
                ([dict(field='tag', op='eq', value='Holiday'), dict(field='make', op='contains', value='Canon', negate=True)], 'all', {'b.jpg'}),
            ]
            for rules, match, expected in cases:
                with self.subTest(rules=rules, match=match):
                    items = self.filtered_items(self.filter_json(*rules, match=match))
                    self.assertEqual({i['name'] for i in items}, expected)
            negative = self.filter_json(dict(field='make', op='contains', value='Canon', negate=True))
            self.assertIn('c.jpg', {i['name'] for i in self.filtered_items(negative)})

    def test_metadata_choices_scope_snapshot_and_exact_values(self):
        with self.filter_library():
            # New fixture files enter the choices only after the shared path rescan.
            self.assertEqual(self.api(act='timeline_filter_values')['values'], dict(make=[], model=[], lens=[]))
            self.refresh('timeline_tags_refresh')
            original = self.database.read_bytes()
            values = self.api(act='timeline_filter_values')['values']
            self.assertEqual(values, dict(make=['CANON', 'Canon', 'Nikon', 'Sony'],
                                         model=['EOS R5', 'EOS R6', 'ILCE', 'Z_8'],
                                         lens=['35 mm', '50 mm', 'RF 24-70%']))
            self.assertEqual(self.database.read_bytes(), original)
            # User conditions do not hide alternatives while editing another condition.
            query = self.filter_json(dict(field='make', op='eq', value='Canon'))
            self.assertEqual(self.api(act='timeline_filter_values', filter=query)['values'], values)
            try:
                with sqlite_connection(self.database) as db:
                    # Exclude out-of-root, missing and symlink paths, and invalid text.
                    for name in ('outside.jpg', 'missing.jpg', 'escape.jpg'):
                        db.execute('INSERT INTO ImageMetadata(imageid,make) SELECT id,? FROM Images WHERE name=?',
                                   (name, name))
                    db.execute("UPDATE ImageMetadata SET model=' \t\n',lens=? WHERE model='ILCE'", ('x'*257,))
                    db.execute("UPDATE ImageMetadata SET lens=? WHERE model='EOS R6'", ('x\x00y',))
                    db.execute("UPDATE ImageMetadata SET lens=? WHERE model='Z_8'", (' <lens> & ñ %_ ',))
                values = self.api(act='timeline_filter_values')['values']
                self.assertEqual(values['make'], ['CANON', 'Canon', 'Nikon', 'Sony'])
                self.assertEqual(values['model'], ['EOS R5', 'EOS R6', 'Z_8'])
                self.assertEqual(values['lens'], [' <lens> & ñ %_ ', 'RF 24-70%'])
                exact = self.filter_json(dict(field='lens', op='eq', value=' <lens> & ñ %_ '))
                self.assertEqual({i['name'] for i in self.filtered_items(exact)}, {'b.jpg'})
            finally:
                with sqlite_connection(self.database) as db:
                    db.execute("DELETE FROM ImageMetadata WHERE imageid IN (SELECT id FROM Images WHERE name IN ('outside.jpg','missing.jpg','escape.jpg'))")
        self.refresh('timeline_tags_refresh')

    def test_filters_use_creation_date_without_fallback(self):
        with self.filter_library():
            rules = [dict(field='creationDate', op='gt', value='2020-02-29'),
                     dict(field='creationDate', op='lt', value='2020-03-01')]
            files = self.filtered_items(self.filter_json(*rules))
            self.assertEqual([i['name'] for i in files], ['IMG_20300101-100000-a.jpg'])
            self.assertEqual(files[0]['date_source'], 'filename')  # grouping precedence is preserved
            self.assertEqual(self.filtered_items(self.filter_json(dict(field='creationDate', op='gt', value='2029-01-01'))), [])
            strict = self.filter_json(dict(field='creationDate', op='lt', value='2020-03-01T00:00:01Z'))
            self.assertEqual({i['name'] for i in self.filtered_items(strict)}, {'IMG_20300101-100000-a.jpg','e.jpg'})
            negative = self.filter_json(dict(field='creationDate', op='gt', value='2020-01-01', negate=True))
            self.assertTrue({'c.jpg','d.jpg'}.issubset({i['name'] for i in self.filtered_items(negative)}))

    def test_filters_composed_groups_keep_main_and(self):
        with self.filter_library():
            aaron = dict(field='tag', op='eq', value='Aaron')
            esther = dict(field='tag', op='eq', value='Esther')
            either = dict(match='any', rules=[aaron, esther])
            query = self.filter_json(either)
            items = self.filtered_items(query)
            self.assertEqual({i['name'] for i in items}, {'IMG_20300101-100000-a.jpg','b.jpg','e.jpg'})
            self.assertEqual(len(items), 3)  # b.jpg has both tags, but occurs once.
            limited = self.filter_json(either, dict(field='sensitivity',op='gt',value=150))
            self.assertEqual({i['name'] for i in self.filtered_items(limited)}, {'b.jpg','e.jpg'})
            both = dict(match='all', rules=[aaron, esther])
            self.assertEqual([i['name'] for i in self.filtered_items(self.filter_json(both))], ['b.jpg'])
            nested = dict(match='any', rules=[
                dict(match='all', rules=[aaron, dict(field='make',op='contains',value='Canon')]),
                esther,
            ])
            query = self.filter_json(nested, dict(field='sensitivity',op='lt',value=500))
            self.assertEqual({i['name'] for i in self.filtered_items(query)}, {'IMG_20300101-100000-a.jpg','e.jpg'})
            # Negation still includes missing metadata inside composed predicates.
            negative = dict(match='any', rules=[aaron, dict(field='make',op='contains',value='Canon',negate=True)])
            self.assertIn('c.jpg', {i['name'] for i in self.filtered_items(self.filter_json(negative))})
            page = self.api(act='timeline', filter=query)
            canonical = json.dumps(page['filter'], sort_keys=True)
            self.assertNotIn('match', page['filter'])
            self.assertEqual(self.api(act='timeline', filter=canonical)['revision'], page['revision'])
            self.assertEqual({i['name'] for i in self.filtered_items(canonical)}, {'IMG_20300101-100000-a.jpg','e.jpg'})
            event = self.api(act='timeline',filter=self.filter_json(either))['events'][0]
            with self.assertRaises(urllib.error.HTTPError) as error:
                self.api(act='timeline_samples',event=event['id'],filter=self.filter_json(both))
            self.assertEqual(error.exception.code, 409); error.exception.close()

    def test_filters_regroup_events_and_scope_samples_and_cursors(self):
        with self.filter_library():
            # All three files originally form one event; removing the middle one
            # creates a gap >=1800 seconds and therefore two filtered events.
            with sqlite_connection(self.database) as db:
                for index, name in enumerate(('b.jpg','c.jpg','d.jpg')):
                    stamp = datetime.datetime.fromtimestamp(2_000_000_000+index*1000, datetime.timezone.utc).isoformat()
                    db.execute('UPDATE Images SET modificationDate=? WHERE name=?', (stamp,name))
            query = self.filter_json(dict(field='model', op='contains', value='Z_8'), dict(field='model', op='contains', value='R6'), match='any')
            page = self.api(act='timeline', filter=query, limit=1)
            self.assertEqual(page['total_items'], 2)
            self.assertEqual(page['total_events'], 2)
            event = page['events'][0]
            samples = self.api(act='timeline_samples', event=event['id'], filter=json.dumps(page['filter'], sort_keys=True))
            self.assertEqual([i['name'] for i in samples['samples']], ['d.jpg'])
            self.assertEqual(samples['samples'][0]['index'], 0)
            baseline = self.api(act='timeline')
            for params in [dict(act='timeline_event', event=event['id']), dict(act='timeline_samples', event=event['id']),
                           dict(act='timeline', cursor=page['next'], revision=page['revision'])]:
                with self.assertRaises(urllib.error.HTTPError) as error: self.api(**params)
                self.assertEqual(error.exception.code, 409); error.exception.close()
            # Merely changing this client's filter must not invalidate other users.
            self.api(act='timeline', filter=self.filter_json(dict(field='tag', op='eq', value='Holiday')))
            self.assertEqual(self.api(act='timeline')['revision'], baseline['revision'])
            token = next(cookie.value for cookie in self.cookies if cookie.name == 'CSRFToken')
            request = urllib.request.Request(self.url+'?'+urllib.parse.urlencode(dict(act='timeline_refresh',filter=query)), method='POST', headers={'X-CSRFToken':token})
            with self.client.open(request) as response: refreshed = json.load(response)['data']
            self.assertEqual(refreshed['total_items'], 2)
            self.assertNotEqual(refreshed['revision'], page['revision'])

    def test_filters_validation(self):
        valid = dict(field='tag', op='eq', value='Holiday')
        bad = ['null', '[]', '{', '{}', '"text"', ' ' * 8193,
               '{"match":"all","rules":[{"field":"sensitivity","op":"eq","value":1e9999}]}']
        bad += [json.dumps(value) for value in [
            dict(match='unknown',rules=[]), dict(match='all',rules={}), dict(match='all',rules=[valid]*21),
            dict(match='all',rules=[],extra=True), dict(match='all',rules=[None]),
            dict(match='any',rules=[valid]), dict(match='any',rules=[]),
        ]]
        invalid_groups = [dict(match='any',rules=[]), dict(match='all',rules={}), dict(match='any'),
                          dict(rules=[valid]), dict(match='unknown',rules=[valid]),
                          dict(match='any',rules=[valid],field='tag'),
                          dict(match='any',rules=[valid],negate=True),
                          dict(match='all',rules=[dict(valid,op='contains')]),
                          dict(match='any',rules=[valid]*20)]  # Group itself counts toward 20.
        deep = valid
        for _ in range(5): deep = dict(match='any',rules=[deep])
        bad += [self.filter_json(group) for group in invalid_groups + [deep]]
        invalid_rules = [dict(valid,field='unknown'), dict(valid,field='make) OR 1=1 --'),
                         dict(valid,field='make\" OR 1=1 --'), dict(valid,op='eq OR 1=1 --'), dict(valid,op='contains'),
                         dict(valid,value=''), dict(valid,value='   '), dict(valid,value='x'*257), dict(valid,value='x\x00y'),
                         dict(valid,value=5), dict(valid,negate='true'), dict(valid,extra=True),
                         dict(field='make',op='gt',value='Canon'), dict(field='sensitivity',op='gt',value='400'),
                         dict(field='sensitivity',op='gt',value=True), dict(field='sensitivity',op='contains',value=400),
                         dict(field='creationDate',op='eq',value='2020-01-01')]
        invalid_rules += [dict(field='creationDate',op='gt',value=date) for date in
                          ['2023-02-29','2024-04-31','0000-01-01','2024-13-01','2024-01-01T24:00',
                           '2024-01-01T12:60','2024-01-01T12:00:60','2024-01-01T12:00:00+14:01',"2024-01-01' OR 1=1"]]
        bad += [self.filter_json(rule) for rule in invalid_rules]
        for query in bad:
            with self.subTest(query=query[:200]):
                with self.assertRaises(urllib.error.HTTPError) as error: self.api(act='timeline',filter=query)
                self.assertEqual(error.exception.code, 400); error.exception.close()
        before = self.api(act='timeline')
        for query in ('', self.filter_json(), json.dumps(dict(match='all',rules=[]))):
            empty = self.api(act='timeline', filter=query)
            self.assertEqual(empty['revision'], before['revision'])
            self.assertEqual(empty['total_items'], before['total_items'])
        deep = valid
        for _ in range(4): deep = dict(match='any',rules=[deep])
        self.api(act='timeline',filter=self.filter_json(deep))
        self.api(act='timeline',filter=self.filter_json(dict(match='any',rules=[valid]*19)))
        flat = self.api(act='timeline',filter=self.filter_json(valid))
        legacy = self.api(act='timeline',filter=json.dumps(dict(match='all',rules=[valid])))
        self.assertEqual(flat['revision'], legacy['revision'])
        # Normalization makes JSON whitespace, key order and omitted negate safe
        # across feed/gallery requests without accepting a different condition.
        query = self.filter_json(dict(field='creationDate',op='gt',value='1960-01-01T00:00Z'))
        self.assertEqual(self.api(act='timeline', filter=query)['filter']['rules'][0]['value'], '1960-01-01T00:00:00Z')

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

    def test_event_and_image_badges_cover_all_pages_and_respect_filters(self):
        """Event tags include unsampled/page-two files, but never missing files or technical tags."""
        try:
            with sqlite_connection(self.database) as db:
                db.executemany('INSERT INTO Tags(id,pid,name) VALUES(?,?,?)', [
                    (9500, 0, 'Travel'), (9501, 9500, 'Beach'),
                    (9502, 0, 'Family'), (9503, 9502, 'Beach'),
                    (9504, 0, 'Only missing'), (9505, 0, '<tag> & ñ'),
                    (9506, 0, '_Digikam_Internal_Tags_'), (9507, 9506, 'Internal child'),
                    (9508, 0, '  \t'),
                ])
                for album, name, ids in [
                    ('/Familiar/Large', '000.jpg', (9501, 9505, 9507, 9508)),
                    ('/Familiar/Large', '001.jpg', (9501,)),
                    ('/Familiar/Other', 'duplicate.jpg', (9503,)),
                    ('/Familiar', 'missing.jpg', (9504,)),
                ]:
                    for tagid in ids:
                        db.execute('INSERT INTO ImageTags(imageid,tagid) SELECT i.id,? FROM Images i JOIN Albums a ON a.id=i.album WHERE a.relativePath=? AND i.name=?',
                                   (tagid, album, name))
            original = self.database.read_bytes()
            event = next(e for e in self.all_events() if e['count'] == 66)
            expected = [dict(id=9505, name='<tag> & ñ', path='<tag> & ñ'),
                        dict(id=9503, name='Beach', path='Family / Beach'),
                        dict(id=9501, name='Beach', path='Travel / Beach')]
            self.assertEqual(event['tags'], expected)
            first = self.api(act='timeline_event', event=event['id'])
            second = self.api(act='timeline_event', event=event['id'], cursor=60)
            self.assertEqual(first['tags'], expected)
            self.assertEqual(second['tags'], expected)
            self.assertEqual(first['items'][0]['tags'], [expected[0], expected[2]])
            self.assertEqual(first['items'][2]['tags'], [])
            self.assertEqual(second['items'][-1]['tags'], [expected[1]])
            sample = self.api(act='timeline_samples', event=event['id'])
            self.assertEqual(sample['tags'], expected)
            for item in sample['samples']:
                one = self.api(act='timeline_event', event=event['id'], cursor=item['index'], limit=1)['items'][0]
                self.assertEqual(item['tags'], one['tags'])
            # Same leaf name keeps both identities, and the event union follows
            # the filtered files rather than the unfiltered time range.
            query = self.filter_json(dict(field='tag', op='eq', value='<tag> & ñ'))
            filtered = self.api(act='timeline', filter=query)
            self.assertEqual(filtered['total_items'], 1)
            self.assertEqual(filtered['events'][0]['tags'], [expected[0], expected[2]])
            self.assertEqual(self.database.read_bytes(), original)
        finally:
            with sqlite_connection(self.database) as db:
                db.execute('DELETE FROM ImageTags WHERE tagid BETWEEN 9500 AND 9508')
                db.execute('DELETE FROM Tags WHERE id BETWEEN 9500 AND 9508')

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

    def test_feed_order_with_pagination_and_filters(self):
        with self.filter_library():
            for query in ({}, {'filter': self.filter_json(dict(field='tag', op='eq', value='Holiday'))}):
                descending = []
                ascending = []
                for order, events in [('desc', descending), ('asc', ascending)]:
                    page = self.api(act='timeline', order=order, limit=3, **query)
                    total = page['total_events']
                    while True:
                        self.assertEqual(page['order'], order)
                        events.extend(page['events'])
                        if not page['has_more']: break
                        page = self.api(act='timeline', order=order, limit=3,
                                        cursor=page['next'], revision=page['revision'], **query)
                    self.assertEqual(len(events), total)
                # IDs and bounds must survive reversing the traversal, including
                # multi-image events and the exact inactivity threshold boundary.
                self.assertEqual([(e['id'], e['start'], e['end'], e['count']) for e in ascending],
                                 [(e['id'], e['start'], e['end'], e['count']) for e in reversed(descending)])
                event = next(e for e in ascending if e['count'])
                self.assertEqual(self.api(act='timeline_event', event=event['id'], **query)['count'], event['count'])
            with self.assertRaises(urllib.error.HTTPError) as error:
                self.api(act='timeline', order='invalid')
            self.assertEqual(error.exception.code, 400)
            error.exception.close()

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

    def test_command_line_tag_exclusions_hide_badges_without_filtering_media(self):
        rules = Path(self.temp.name)/'excluded-tags.txt'
        rules.write_bytes(b"# Tag visibility only\r\n\r\n  # Comment\r\n^Aaron$\r\n^O'Brien\r\n")
        with self.filter_library():
            with sqlite_connection(self.database) as db:
                db.execute('UPDATE Tags SET pid=9104 WHERE id=9101')
                db.execute('UPDATE Tags SET pid=9101 WHERE id=9105')
            baseline = self.api(act='timeline')['total_items']
            with socket.socket() as sock:
                sock.bind(('127.0.0.1', 0)); port = sock.getsockname()[1]
            url = f'http://127.0.0.1:{port}/?'
            cookies = http.cookiejar.CookieJar()
            client = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(cookies))
            args = [str(BINARY), '--rootdir', str(self.root), '-a', '127.0.0.1', '-l', str(port), '--tag-exclude-file', str(rules)]
            process = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=dict(os.environ, TZ='UTC'))
            def api(**params):
                with client.open(url+urllib.parse.urlencode(params), timeout=10) as response:
                    return json.load(response)['data']
            try:
                for _ in range(200):
                    if process.poll() is not None: self.fail('Server with tag exclusions failed to start')
                    try:
                        catalog = api(act='timeline_tags')['tags']; break
                    except OSError: time.sleep(.025)
                else: self.fail('Server with tag exclusions did not start')
                # Aaron -> Holiday -> Esther is hidden; another Holiday branch remains.
                self.assertEqual({tag['id'] for tag in catalog}, {9102})
                self.assertTrue(all(tag['path'] == tag['name'] for tag in catalog))
                self.assertEqual(api(act='timeline')['total_items'], baseline)
                # Hiding an entire tag branch keeps all its media navigable.
                query = self.filter_json(dict(field='tag',op='eq',value='Aaron'))
                feed = api(act='timeline', filter=query)
                self.assertEqual(feed['total_items'], 2)
                items = [item for event in feed['events']
                         for item in api(act='timeline_event', event=event['id'], filter=query)['items']]
                self.assertEqual({item['name'] for item in items}, {'IMG_20300101-100000-a.jpg', 'b.jpg'})
                visible = {9102}
                for event in feed['events']:
                    self.assertEqual({tag['id'] for tag in event['tags']}, visible)
                    gallery = api(act='timeline_event', event=event['id'], filter=query)
                    self.assertEqual({tag['id'] for tag in gallery['tags']}, visible)
                    samples = api(act='timeline_samples', event=event['id'], filter=query)
                    self.assertEqual({tag['id'] for tag in samples['tags']}, visible)
                    for item in gallery['items'] + event['samples'] + samples['samples']:
                        self.assertEqual({tag['id'] for tag in item['tags']}, visible)
                # A file with only excluded tags stays visible with no badges.
                plain = api(act='timeline')
                all_items = [item for event in plain['events']
                             for item in api(act='timeline_event', event=event['id'])['items']]
                self.assertEqual(next(item for item in all_items if item['name'] == 'e.jpg')['tags'], [])
                self.assertEqual(len(api(act='ls', path='/Filters')['items']), 5)
                token = next(cookie.value for cookie in cookies if cookie.name == 'CSRFToken')
                request = urllib.request.Request(url+'act=timeline_tags_refresh', method='POST', headers={'X-CSRFToken':token})
                with client.open(request, timeout=10) as response:
                    self.assertEqual(json.load(response)['data']['tags'], catalog)
            finally:
                process.terminate()
                try: process.wait(timeout=5)
                except subprocess.TimeoutExpired: process.kill(); process.wait()
        for invalid in (rules.with_name('missing-exclusions.txt'), rules):
            rules.write_text('# Comment\n[invalid\n')
            result = subprocess.run([str(BINARY), '--tag-exclude-file', str(invalid)],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b'--tag-exclude-file', result.stdout+result.stderr)
            if invalid == rules: self.assertIn((str(rules)+':2').encode(), result.stdout+result.stderr)

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

    def test_tag_icons_cache_face_crop_and_scope(self):
        """Tag artwork uses the selected image, optional cache, and the public media scope."""
        cache = self.database.parent/'thumbnails-digikam.db'
        with self.filter_library():
            with sqlite_connection(self.database) as db:
                image = db.execute("SELECT i.id FROM Images i JOIN Albums a ON a.id=i.album WHERE a.relativePath='/Familiar/Filters' AND i.name LIKE 'IMG_%'").fetchone()[0]
                db.execute('UPDATE Tags SET icon=? WHERE id=9104', (image,))
                db.execute('INSERT INTO ImageTagProperties VALUES(?,9104,\'tagRegion\',?)',
                           (image, '<rect x="20" y="10" width="20" height="20"/>'))
            self.refresh('timeline_tags_refresh')
            try:
                tag = next(t for t in self.api(act='timeline_tags')['tags'] if t['id'] == 9104)
                self.assertEqual(tag['icon'], '?act=timeline_tag_icon&id=9104')
                self.assertEqual(next(t for t in self.api(act='timeline_tags')['tags'] if t['id'] == 9105)['icon'], '')
                original = self.database.read_bytes()
                cropped = self.get('act=timeline_tag_icon&id=9104')
                size = subprocess.run(['identify', '-format', '%wx%h', '-'], input=cropped, check=True, capture_output=True).stdout
                self.assertEqual(size, b'24x24')
                self.assertEqual(self.database.read_bytes(), original)
                self.assertFalse(cache.exists())
                with sqlite_connection(cache) as db:
                    db.executescript('''CREATE TABLE Thumbnails(id INTEGER PRIMARY KEY, data BLOB, orientationHint INTEGER);
                        CREATE TABLE CustomIdentifiers(identifier TEXT, thumbId INTEGER);
                        CREATE TABLE FilePaths(path TEXT, thumbId INTEGER);''')
                    db.execute('INSERT INTO Thumbnails VALUES(1,?,1)', (self.base.read_bytes(),))
                    db.execute('INSERT INTO CustomIdentifiers VALUES(?,1)', ('detail:'+str(self.root/'Filters/IMG_20300101-100000-a.jpg')+'?rect=18,8-24x24',))
                cached = cache.read_bytes()
                self.assertEqual(self.get('act=timeline_tag_icon&id=9104'), self.base.read_bytes())
                self.assertEqual(cache.read_bytes(), cached)
                # PGF or invalid cached blobs fall back to the original face crop.
                with sqlite_connection(cache) as db:
                    db.execute("UPDATE Thumbnails SET data=x'50474676'")
                self.assertEqual(self.get('act=timeline_tag_icon&id=9104'), cropped)
                with sqlite_connection(self.database) as db:
                    db.execute('DELETE FROM ImageTagProperties WHERE tagid=9104')
                with sqlite_connection(cache) as db:
                    db.execute('UPDATE Thumbnails SET data=?', (self.base.read_bytes(),))
                    db.execute('INSERT INTO FilePaths VALUES(?,1)', (str(self.root/'Filters/IMG_20300101-100000-a.jpg'),))
                self.assertEqual(self.get('act=timeline_tag_icon&id=9104'), self.base.read_bytes())
                # An assigned icon outside rootdir cannot be used to expose other media.
                with sqlite_connection(self.database) as db:
                    outside = db.execute("SELECT id FROM Images WHERE name='outside.jpg' LIMIT 1").fetchone()[0]
                    db.execute('UPDATE Tags SET icon=? WHERE id=9104', (outside,))
                for query, code in [('id=9104',404), ('id=9105',404), ('id=999999',404), ('id=0',400), ('id=1%20OR%201',400)]:
                    with self.assertRaises(urllib.error.HTTPError) as error:
                        self.get('act=timeline_tag_icon&'+query)
                    self.assertEqual(error.exception.code, code)
            finally:
                cache.unlink(missing_ok=True)
                with sqlite_connection(self.database) as db:
                    db.execute('DELETE FROM ImageTagProperties WHERE tagid=9104')

    def test_tag_catalog_hides_digikam_internal_tags(self):
        """Hide technical properties and entire internal branches, retaining public namesakes."""
        try:
            with sqlite_connection(self.database) as db:
                db.executemany('INSERT INTO Tags(id,pid,name) VALUES(?,?,?)', [
                    (9400, 0, '_Digikam_Internal_Tags_'),
                    (9401, 9400, 'Scanned for Faces'), (9402, 9401, 'Nested'),
                    (9403, 0, '_Digikam_root_tag_'), (9404, 0, 'Technical'),
                    (9405, 9404, 'Child'), (9406, 9403, 'Scanned for Faces'),
                ])
                db.execute('INSERT INTO ImageTags(imageid,tagid) SELECT i.id,t.id FROM Images i JOIN Albums a ON a.id=i.album CROSS JOIN Tags t WHERE a.relativePath=\'/Familiar/A beach\' AND i.name=\'same.jpg\' AND t.id BETWEEN 9400 AND 9406')
            # Reserved branch detection also works without a TagProperties table.
            self.assertEqual({t['id'] for t in self.api(act='timeline_tags')['tags']}, {9404, 9405, 9406})
            with sqlite_connection(self.database) as db:
                db.execute('CREATE TABLE TagProperties(tagid INTEGER, property TEXT, value TEXT)')
                db.executemany('INSERT INTO TagProperties VALUES(?,?,?)', [
                    (9404, 'internalTag', None), (9406, 'person', ''), (9406, 'noMetadataTag', ''),
                ])
            original = self.database.read_bytes()
            tags = self.api(act='timeline_tags')['tags']
            self.assertEqual([t['id'] for t in tags], [9406])
            self.assertEqual(tags[0]['path'], 'Scanned for Faces')
            self.assertEqual(self.database.read_bytes(), original)
        finally:
            with sqlite_connection(self.database) as db:
                db.execute('DROP TABLE IF EXISTS TagProperties')
                db.execute('DELETE FROM ImageTags WHERE tagid BETWEEN 9400 AND 9406')
                db.execute('DELETE FROM Tags WHERE id BETWEEN 9400 AND 9406')

    def test_tag_catalog_snapshot_and_rescan(self):
        """SQL uses startup paths until an explicit rescan; digiKam remains read-only."""
        live = self.root/'A beach/same.jpg'
        parked = live.with_suffix('.parked')
        added = self.photo('Tags/new-after-startup.jpg', 1_800_000_000)
        try:
            with sqlite_connection(self.database) as db:
                db.executemany('INSERT INTO Tags(id,pid,name) VALUES(?,?,?)', [
                    (9200, 0, 'Parent'), (9201, 9200, "Live <tag> & ñ"),
                    (9202, 0, 'Missing'), (9203, 0, 'File symlink'),
                    (9204, 0, 'Outside root'), (9205, 0, ' \t\n'),
                    (9206, 0, ''), (9207, 0, 'Unused'), (9208, 0, 'Invalid date'),
                    (9209, 0, 'Directory symlink'), (9210, 0, None),
                    (9211, 0, 'New file'),
                ])
                for tagid, album, name in [
                    (9201, '/Familiar/A beach', 'same.jpg'),
                    (9202, '/Familiar', 'missing.jpg'),
                    (9203, '/Familiar', 'escape.jpg'),
                    (9204, '/FamiliarExtra', 'outside.jpg'),
                    (9205, '/Familiar/A beach', 'same.jpg'),
                    (9206, '/Familiar/A beach', 'same.jpg'),
                    (9208, '/Familiar', 'invalid-date.jpg'),
                    (9209, '/Familiar/loop/A beach', 'same.jpg'),
                    (9210, '/Familiar/A beach', 'same.jpg'),
                    (9211, '/Familiar/Tags', added.name),
                ]:
                    db.execute('INSERT INTO ImageTags(imageid,tagid) SELECT i.id,? FROM Images i JOIN Albums a ON a.id=i.album WHERE a.relativePath=? AND i.name=?',
                               (tagid, album, name))
            original = self.database.read_bytes()
            catalog = self.api(act='timeline_tags')['tags']
            self.assertEqual({t['id'] for t in catalog}, {9201, 9208})
            self.assertEqual(next(t['path'] for t in catalog if t['id'] == 9201), 'Parent / Live <tag> & ñ')
            self.assertTrue(all(t['count'] == 1 for t in catalog))
            # A normal timeline refresh must not initiate a gallery scan.
            live.rename(parked)
            self.refresh()
            self.assertEqual(self.api(act='timeline_tags')['tags'], catalog)
            updated = self.refresh('timeline_tags_refresh')
            self.assertGreater(updated['scanned_files'], 0)
            self.assertEqual({t['id'] for t in updated['tags']}, {9208, 9211})
            self.assertEqual(self.api(act='timeline_tags')['tags'], updated['tags'])
            self.assertEqual(self.database.read_bytes(), original)
            for action in ('timeline_tags_refresh',):
                with self.assertRaises(urllib.error.HTTPError) as error: self.api(act=action)
                self.assertEqual(error.exception.code, 405)
                error.exception.close()
                request = urllib.request.Request(self.url+'?act='+action, method='POST')
                with self.assertRaises(urllib.error.HTTPError) as error: urllib.request.urlopen(request)
                self.assertEqual(error.exception.code, 401)
                error.exception.close()
        finally:
            if parked.exists(): parked.rename(live)
            added.unlink(missing_ok=True)
            with sqlite_connection(self.database) as db:
                db.execute('DELETE FROM ImageTags WHERE tagid BETWEEN 9200 AND 9211')
                db.execute('DELETE FROM Tags WHERE id BETWEEN 9200 AND 9211')
                db.execute("DELETE FROM Images WHERE album IN (SELECT id FROM Albums WHERE relativePath='/Familiar/Tags')")
            self.refresh('timeline_tags_refresh')

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
