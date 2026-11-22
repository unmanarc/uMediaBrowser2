"""Browser integration for the timeline editor against a real digiKam fixture.

Run: python3 tests/test_timeline_filters_ui.py build/timeline/uMediaBrowser2
Requires playwright, Google Chrome (or CHROME_BINARY), convert and node.
"""
import json
import os
import unittest
import urllib.parse

from playwright.sync_api import sync_playwright
import test_timeline as timeline


class TimelineFilterUITests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        """Reuse the read-only integration fixture without inheriting its tests."""
        cls.server = timeline.TimelineTests
        cls.server.setUpClass()
        cls.addClassCleanup(cls.server.tearDownClass)
        cls.fixture = cls.server().filter_library()
        cls.fixture.__enter__()
        cls.addClassCleanup(cls.fixture.__exit__, None, None, None)
        cls.server.refresh('timeline_tags_refresh')
        cls.playwright = sync_playwright().start()
        cls.addClassCleanup(cls.playwright.stop)
        cls.browser = cls.playwright.chromium.launch(
            executable_path=os.environ.get('CHROME_BINARY', '/usr/bin/google-chrome'),
            headless=True, args=['--no-sandbox', '--disable-dev-shm-usage'])
        cls.addClassCleanup(cls.browser.close)

    def setUp(self):
        self.context = self.browser.new_context(viewport=dict(width=1440, height=1000))
        self.page = self.context.new_page()
        self.page.set_default_timeout(10000)
        self.errors = []
        self.page.on('pageerror', lambda error: self.errors.append(str(error)))
        self.page.goto(self.server.url+'?view=timeline')
        self.page.wait_for_function('feed.loaded')

    def tearDown(self):
        self.context.close()
        self.assertEqual(self.errors, [])

    def open_editor(self):
        self.page.locator('#btnTimelineFilter').click()
        self.assertTrue(self.page.locator('#timelineFilterDialog').is_visible())
        self.page.wait_for_function('filterTags !== null && !filterTagsRequest')
        self.page.wait_for_function('filterValues !== null && !filterValuesRequest')

    def add_rule(self, field, value, op=None, negate=False, group=None):
        if group is None: self.page.locator('#filterAdd').click()
        else: group.locator(':scope > .filter-group-actions').get_by_role('button', name='+ Add condition', exact=True).click()
        row = self.page.locator('.filter-rule').last if group is None else group.locator(':scope > .filter-group-rules > .filter-rule').last
        row.get_by_label('Field', exact=True).select_option(field)
        if op: row.get_by_label('Comparison', exact=True).select_option(op)
        if field in ('tag', 'make', 'model', 'lens'): row.get_by_label('Value', exact=True).select_option(value)
        else: row.get_by_label('Value', exact=True).fill(value)
        if negate: row.get_by_label('Include or exclude condition').select_option('true')
        return row

    def add_group(self, parent=None, match='any'):
        if parent is None:
            self.page.locator('#filterAddGroup').click()
            group = self.page.locator('#filterRules > .filter-group').last
        else:
            parent.locator(':scope > .filter-group-actions').get_by_role('button', name='+ Add group', exact=True).click()
            group = parent.locator(':scope > .filter-group-rules > .filter-group').last
        group.locator(':scope > .filter-group-top').get_by_label('Group match').select_option(match)
        return group

    def apply(self):
        with self.page.expect_response(lambda response: 'act=timeline&' in response.url and 'filter=' in response.url):
            self.page.get_by_role('button', name='Apply & refresh timeline').click()
        self.page.wait_for_function('feed.loaded')
        self.assertFalse(self.page.locator('#timelineFilterDialog').is_visible())

    def test_timeline_order_persists_through_filters_and_navigation(self):
        newest = self.page.locator('.story').first.evaluate('(card) => card._event.end')
        self.page.get_by_label('Order', exact=True).select_option('asc')
        self.page.wait_for_function("feed.loaded && feed.order === 'asc'")
        oldest = self.page.locator('.story').first.evaluate('(card) => card._event.end')
        self.assertLess(oldest, newest)
        self.assertIn('Oldest first', self.page.locator('#timelineStatus').inner_text())
        self.assertIn('order=asc', self.page.url)
        self.page.locator('.story-foot a').first.click()
        self.page.wait_for_function('eventView && eventView.info')
        self.page.locator('.event-tools a').click()
        self.page.wait_for_function("mode === 'feed'")
        self.assertEqual(self.page.locator('#timelineOrder').input_value(), 'asc')
        self.open_editor()
        self.add_rule('tag', 'Holiday')
        self.apply()
        self.assertIn('order=asc', self.page.url)
        self.page.reload()
        self.page.wait_for_function('feed.loaded')
        self.assertEqual(self.page.locator('#timelineOrder').input_value(), 'asc')
        self.page.get_by_label('Order', exact=True).select_option('desc')
        self.page.wait_for_function("feed.loaded && feed.order === 'desc'")
        self.assertIn('Newest first', self.page.locator('#timelineStatus').inner_text())
        self.assertIn('filter=', self.page.url)

    def test_apply_cancel_remove_reset_and_event_navigation(self):
        self.open_editor()
        box = self.page.locator('#timelineFilterDialog').bounding_box()
        self.assertEqual((box['x'],box['y'],box['width'],box['height']), (0,0,1440,1000))
        self.add_rule('tag', 'Holiday')
        self.add_rule('sensitivity', '400', 'gt')
        self.page.screenshot(path='/tmp/umedia-timeline-filters-desktop.png', full_page=True)
        self.apply()
        self.assertIn('1 events · 1 items', self.page.locator('#timelineStatus').inner_text())
        self.assertEqual(self.page.locator('#filterBadge').inner_text(), '2')
        self.assertEqual(self.page.locator('.story-photo img').get_attribute('alt'), 'b.jpg')
        self.assertIn('filter=', self.page.url)
        self.page.locator('.story-foot a').click()
        self.page.wait_for_function('eventView && eventView.info')
        self.assertEqual(self.page.locator('#eventItems .name').inner_text(), 'b.jpg')
        self.page.locator('#eventItems .item').click()
        self.page.wait_for_function('albumOpen()')
        self.page.keyboard.press('Escape')
        event_url = self.page.url
        self.page.reload()
        self.page.wait_for_function('eventView && eventView.info')
        self.assertEqual(self.page.locator('#filterBadge').inner_text(), '2')
        self.open_editor()
        self.assertEqual(self.page.locator('.filter-rule').count(), 2)
        self.page.locator('.filter-rule').first.get_by_role('button', name='Remove condition').click()
        self.page.locator('#filterCancel').click()
        self.assertEqual(self.page.url, event_url)
        self.open_editor()
        self.assertEqual(self.page.locator('.filter-rule').count(), 2)
        self.page.keyboard.press('Escape')
        self.assertFalse(self.page.locator('#timelineFilterDialog').is_visible())
        self.open_editor()
        with self.page.expect_response(lambda response: 'act=timeline&' in response.url and 'filter=' not in response.url):
            self.page.locator('#filterReset').click()
        self.page.wait_for_function('feed.loaded')
        self.assertNotIn('filter=', self.page.url)
        self.assertTrue(self.page.locator('#filterBadge').is_hidden())
        self.assertGreater(self.page.locator('.story').count(), 1)

    def test_mobile_dates_negation_validation_and_empty_results(self):
        self.page.set_viewport_size(dict(width=390, height=844))
        self.open_editor()
        self.add_rule('creationDate', '2020-02-29T00:00:00', 'gt')
        self.add_rule('creationDate', '2020-03-01T00:00:00', 'lt')
        self.add_rule('make', 'Nikon', negate=True)
        self.page.screenshot(path='/tmp/umedia-timeline-filters-mobile.png', full_page=True)
        self.assertTrue(self.page.evaluate('document.querySelector("#filterForm").scrollWidth <= innerWidth'))
        self.apply()
        self.assertIn('1 items', self.page.locator('#timelineStatus').inner_text())
        self.open_editor()
        # Required input prevents a blank draft from replacing the applied filter.
        row = self.add_rule('tag', '')
        self.page.get_by_role('button', name='Apply & refresh timeline').click()
        self.assertTrue(self.page.locator('#timelineFilterDialog').is_visible())
        row.get_by_role('button', name='Remove condition').click()
        while self.page.locator('.filter-rule').count():
            self.page.locator('.filter-rule').first.get_by_role('button', name='Remove condition').click()
        # Old URLs may refer to a removed tag; retain it as disabled context.
        query = self.server.filter_json(dict(field='tag',op='eq',value='<script>does not exist</script>'))
        self.page.goto(self.server.url+'?'+urllib.parse.urlencode(dict(view='timeline',filter=query)))
        self.page.wait_for_function('feed.loaded')
        self.assertIn('No pictures or videos match these filters', self.page.locator('#pageMessage').inner_text())
        self.assertEqual(self.page.locator('.story').count(), 0)
        self.page.reload()
        self.page.wait_for_function('feed.loaded')
        self.open_editor()
        self.assertEqual(self.page.locator('.filter-tag-select').input_value(), '<script>does not exist</script>')
        self.assertTrue(self.page.locator('.filter-tag-select option:checked').is_disabled())

    def test_metadata_dropdowns_and_saved_text_filters(self):
        self.open_editor()
        for field, selected, choices in [
            ('make', 'Nikon', ['CANON', 'Canon', 'Nikon', 'Sony']),
            ('model', 'Z_8', ['EOS R5', 'EOS R6', 'ILCE', 'Z_8']),
            ('lens', '50 mm', ['35 mm', '50 mm', 'RF 24-70%']),
        ]:
            row = self.add_rule(field, selected)
            self.assertEqual(row.locator('input').count(), 0)
            self.assertEqual(row.locator('.filter-metadata-select option').all_text_contents(), ['Choose a value…'] + choices)
            self.assertEqual(row.get_by_label('Comparison', exact=True).input_value(), 'eq')
        self.apply()
        self.assertIn('1 items', self.page.locator('#timelineStatus').inner_text())
        self.page.reload(); self.page.wait_for_function('feed.loaded')
        self.open_editor()
        self.assertEqual(self.page.locator('.filter-metadata-select').evaluate_all('(els) => els.map(el => el.value)'), ['Nikon', 'Z_8', '50 mm'])
        query = self.server.filter_json(dict(field='model', op='contains', value='EOS'))
        self.page.goto(self.server.url+'?'+urllib.parse.urlencode(dict(view='timeline', filter=query)))
        self.page.wait_for_function('feed.loaded')
        self.open_editor()
        self.assertEqual(self.page.locator('.filter-metadata-select').input_value(), 'EOS')
        self.assertEqual(self.page.get_by_label('Comparison', exact=True).input_value(), 'contains')
        self.apply()
        self.assertIn('2 items', self.page.locator('#timelineStatus').inner_text())
        self.open_editor()
        self.page.locator('.filter-metadata-select').select_option('EOS R5')
        self.assertEqual(self.page.get_by_label('Comparison', exact=True).input_value(), 'eq')
        self.apply()
        self.assertIn('1 items', self.page.locator('#timelineStatus').inner_text())

    def test_metadata_loading_failure_empty_and_unavailable_values(self):
        self.page.route('**/*act=timeline_filter_values', lambda route: route.fulfill(
            status=503, content_type='application/json', body=json.dumps(dict(ok=False, error='Choices failed'))))
        self.page.locator('#btnTimelineFilter').click()
        self.page.wait_for_function('!filterValuesRequest && document.querySelector("#filterMetadataStatus").textContent.includes("Choices failed")')
        self.page.locator('#filterAdd').click()
        row = self.page.locator('.filter-rule').last
        row.get_by_label('Field', exact=True).select_option('model')
        self.assertEqual(row.locator('input').count(), 0)
        self.page.unroute('**/*act=timeline_filter_values')
        row.get_by_role('button', name='Rescan gallery for camera and lens options').click()
        self.page.wait_for_function('filterValues !== null && !filterTagsRequest && !filterValuesRequest')
        row.locator('.filter-metadata-select').select_option('EOS R5')
        self.apply()
        # Metadata choices removed since a saved filter are kept as disabled context.
        query = self.server.filter_json(dict(field='lens', op='eq', value='<missing lens>'))
        self.page.goto(self.server.url+'?'+urllib.parse.urlencode(dict(view='timeline', filter=query)))
        self.page.wait_for_function('feed.loaded')
        self.open_editor()
        self.assertEqual(self.page.locator('.filter-metadata-select').input_value(), '<missing lens>')
        self.assertTrue(self.page.locator('.filter-metadata-select option:checked').is_disabled())
        self.page.route('**/*act=timeline_filter_values', lambda route: route.fulfill(
            content_type='application/json', body=json.dumps(dict(ok=True, data=dict(values=dict(make=[], model=[], lens=[]))))))
        self.page.locator('#filterCancel').click()
        self.open_editor()
        self.page.get_by_label('Field', exact=True).select_option('make')
        self.assertEqual(self.page.locator('.filter-metadata-select option').all_text_contents(), ['No values with existing files'])
        self.page.get_by_role('button', name='Apply & refresh timeline').click()
        self.assertTrue(self.page.locator('#timelineFilterDialog').is_visible())

    def test_tag_list_manual_rescan_and_failure_retry(self):
        self.open_editor()
        row = self.add_rule('tag', 'Holiday')
        self.assertEqual(row.locator('input').count(), 0)
        self.assertIn("O'Brien %_ ñ", row.locator('.filter-tag-select').inner_text())
        path = self.server.photo('Tag picker/new.jpg', 1_800_000_000)
        name = '<tag> & new ñ'
        try:
            with timeline.sqlite_connection(self.server.database) as db:
                db.execute('INSERT INTO Tags(id,pid,name) VALUES(9300,0,?)', (name,))
                db.execute("INSERT INTO ImageTags(imageid,tagid) SELECT i.id,9300 FROM Images i JOIN Albums a ON a.id=i.album WHERE a.relativePath='/Familiar/Tag picker'")
            # Closing/reopening loads the SQL tag list without scanning disk.
            self.page.locator('#filterCancel').click()
            self.open_editor()
            row = self.add_rule('tag', 'Holiday')
            self.assertNotIn(name, row.locator('.filter-tag-select').inner_text())
            self.page.route('**/*act=timeline_tags_refresh', lambda route: route.fulfill(
                status=503, content_type='application/json', body=json.dumps(dict(ok=False,error='Scan failed'))))
            row.get_by_role('button', name='Rescan gallery for tags').click()
            self.page.wait_for_function('!filterTagsRequest')
            self.assertIn('Scan failed', self.page.locator('#filterTagStatus').inner_text())
            self.assertEqual(row.locator('.filter-tag-select').input_value(), 'Holiday')
            self.page.unroute('**/*act=timeline_tags_refresh')
            with self.page.expect_response(lambda response: 'act=timeline_tags_refresh' in response.url):
                row.get_by_role('button', name='Rescan gallery for tags').click()
            self.page.wait_for_function('!filterTagsRequest')
            self.assertIn('Gallery rescanned', self.page.locator('#filterTagStatus').inner_text())
            self.assertEqual(row.locator('.filter-tag-select').input_value(), 'Holiday')
            row.locator('.filter-tag-select').select_option(name)
            self.page.set_viewport_size(dict(width=390,height=844))
            self.assertTrue(self.page.evaluate('document.querySelector("#filterForm").scrollWidth <= innerWidth'))
            self.apply()
            self.assertIn('1 items', self.page.locator('#timelineStatus').inner_text())
            self.assertEqual(self.page.locator('.story-photo img').get_attribute('alt'), 'new.jpg')
        finally:
            path.unlink(missing_ok=True)
            with timeline.sqlite_connection(self.server.database) as db:
                db.execute('DELETE FROM ImageTags WHERE tagid=9300')
                db.execute('DELETE FROM Tags WHERE id=9300')
                db.execute("DELETE FROM Images WHERE album IN (SELECT id FROM Albums WHERE relativePath='/Familiar/Tag picker')")
            self.server.refresh('timeline_tags_refresh')

    def test_tag_click_adds_and_draft_preserving_groups_until_apply(self):
        self.open_editor()
        group = self.add_group()
        self.add_rule('tag', 'Aaron', group=group)
        self.add_rule('tag', 'Esther', group=group)
        self.add_rule('sensitivity', '150', 'gt')
        self.apply()
        original_url = self.page.url
        original_filter = self.page.evaluate('appliedFilter')
        original_status = self.page.locator('#timelineStatus').inner_text()
        feed_requests = []
        self.page.on('request', lambda request: feed_requests.append(request.url)
                     if urllib.parse.parse_qs(urllib.parse.urlsplit(request.url).query).get('act') == ['timeline'] else None)

        def select_tag():
            badge = self.page.locator('.story-foot .tag-badge').filter(has_text='Holiday').first
            badge.focus()
            badge.press('Enter')
            self.assertTrue(self.page.locator('#timelineFilterDialog').is_visible())
            self.page.wait_for_function('filterTags !== null && !filterTagsRequest && !filterValuesRequest')
            self.assertEqual(self.page.locator('#filterRules > .filter-group').count(), 1)
            self.assertEqual(self.page.get_by_label('Group match').input_value(), 'any')
            rules = self.page.locator('#filterRules > .filter-rule')
            self.assertEqual(rules.count(), 2)
            self.assertEqual(rules.last.get_by_label('Field', exact=True).input_value(), 'tag')
            self.assertEqual(rules.last.get_by_label('Value', exact=True).input_value(), 'Holiday')
            self.assertEqual(rules.last.get_by_label('Include or exclude condition').input_value(), 'false')
            self.assertEqual(self.page.url, original_url)
            self.assertEqual(self.page.evaluate('appliedFilter'), original_filter)
            self.assertEqual(self.page.locator('#timelineStatus').inner_text(), original_status)
            self.assertEqual(feed_requests, [])

        select_tag()
        self.page.locator('#filterCancel').click()
        self.open_editor()
        self.assertEqual(self.page.locator('.filter-rule').count(), 3)
        self.page.keyboard.press('Escape')
        select_tag()
        self.apply()
        applied = self.page.evaluate('appliedFilter')
        self.assertEqual(applied['rules'][:2], original_filter['rules'])
        self.assertEqual(applied['rules'][2], dict(field='tag', op='eq', value='Holiday', negate=False))
        self.assertIn('1 items', self.page.locator('#timelineStatus').inner_text())
        self.assertEqual(self.page.locator('.story-photo img').get_attribute('alt'), 'b.jpg')

    def test_mobile_event_and_carousel_tags_open_unapplied_drafts(self):
        self.page.set_viewport_size(dict(width=390, height=844))
        story = self.page.locator('.story').filter(has=self.page.locator('.tag-badge').filter(has_text='Holiday')).first
        story.locator('.story-foot a').click()
        self.page.wait_for_function('eventView && eventView.info')
        event_url = self.page.url
        self.page.locator('#eventDetails .tag-badge').filter(has_text='Holiday').first.click()
        self.assertTrue(self.page.locator('#timelineFilterDialog').is_visible())
        self.page.wait_for_function('filterTags !== null && !filterTagsRequest')
        self.assertEqual(self.page.locator('.filter-tag-select').input_value(), 'Holiday')
        self.assertEqual(self.page.url, event_url)
        self.assertEqual(self.page.evaluate('appliedFilter.rules'), [])
        self.page.locator('#filterCancel').click()
        self.page.locator('#eventItems .item').first.click()
        self.page.wait_for_function('albumOpen() && document.querySelector("#albumImg").complete')
        box = self.page.locator('#albumImg').bounding_box()
        self.page.mouse.move(box['x'] + box['width']/2, box['y'] + box['height']/2)
        self.page.wait_for_function('getComputedStyle(document.querySelector("#albumTags")).opacity === "1"')
        badge = self.page.locator('#albumTags .tag-badge').first
        name = badge.inner_text()
        badge.click()
        self.assertTrue(self.page.locator('#timelineFilterDialog').is_visible())
        self.assertFalse(self.page.evaluate('albumOpen()'))
        self.assertEqual(self.page.locator('.filter-tag-select').input_value(), name)
        self.assertEqual(self.page.url, event_url)
        self.assertEqual(self.page.evaluate('appliedFilter.rules'), [])
        self.page.keyboard.press('Escape')
        self.open_editor()
        self.assertEqual(self.page.locator('.filter-rule').count(), 0)

    def test_tag_badges_and_carousel_hover_navigation(self):
        name = '<tag> & ñ'
        try:
            with timeline.sqlite_connection(self.server.database) as db:
                db.executemany('INSERT INTO Tags(id,pid,name) VALUES(?,?,?)', [
                    (9500, 0, 'Travel'), (9501, 9500, name),
                    (9502, 0, 'Family'), (9503, 9502, name),
                ])
                for tagid, album, filename in [(9501, '/Familiar/Large', '000.jpg'),
                                               (9503, '/Familiar/Other', 'duplicate.jpg')]:
                    db.execute('INSERT INTO ImageTags(imageid,tagid) SELECT i.id,? FROM Images i JOIN Albums a ON a.id=i.album WHERE a.relativePath=? AND i.name=?',
                               (tagid, album, filename))
            self.page.reload(); self.page.wait_for_function('feed.loaded')
            story = self.page.locator('.story').filter(has_text='66 items')
            badges = story.locator('.story-foot .tag-badge')
            self.assertEqual(badges.all_text_contents(), [name, name])
            self.assertEqual(badges.first.get_attribute('title'), 'Family / '+name)
            self.assertEqual(badges.last.get_attribute('title'), 'Travel / '+name)
            self.assertEqual(story.locator('button.tag-badge').count(), 2)
            self.assertEqual(story.locator('.tag-badge button, .tag-badge a, .tag-badge img').count(), 0)
            self.page.screenshot(path='/tmp/umedia-story-tags-desktop.png', full_page=True)
            story.locator('.story-foot a').click()
            self.page.wait_for_function('eventView && eventView.info')
            self.assertEqual(self.page.locator('#eventDetails .tag-badge').all_text_contents(), [name, name])
            self.page.screenshot(path='/tmp/umedia-event-tags-desktop.png', full_page=True)
            self.page.locator('#eventItems .item').first.click()
            self.page.wait_for_function('albumCurrentPath === "/Large/000.jpg" && document.querySelector("#albumImg").complete')
            self.page.mouse.move(5, 5)
            self.page.wait_for_function('getComputedStyle(document.querySelector("#albumTags")).visibility === "hidden"')
            box = self.page.locator('#albumImg').bounding_box()
            self.page.mouse.move(box['x']+box['width']/2, box['y']+box['height']/2)
            self.page.wait_for_function('getComputedStyle(document.querySelector("#albumTags")).opacity === "1"')
            badge = self.page.locator('#albumTags .tag-badge')
            self.assertEqual(badge.inner_text(), name)
            self.assertEqual(badge.get_attribute('title'), 'Travel / '+name)
            badge_box = badge.bounding_box()
            self.page.mouse.move(badge_box['x']+badge_box['width']/2, badge_box['y']+badge_box['height']/2, steps=20)
            self.page.wait_for_function('getComputedStyle(document.querySelector("#albumTags")).visibility === "visible"')
            self.assertTrue(badge.is_visible())
            self.page.screenshot(path='/tmp/umedia-carousel-tags-desktop.png', full_page=True)
            # Hovering a badge is informational; image navigation still works.
            self.assertEqual(self.page.evaluate('albumCurrentPath'), '/Large/000.jpg')
            self.page.keyboard.press('ArrowRight')
            self.page.wait_for_function('albumCurrentPath === "/Large/001.jpg"')
            self.assertTrue(self.page.locator('#albumTags').is_hidden())
            # Wrap to a tagged image beyond the initial 60-file page.
            self.page.keyboard.press('ArrowLeft'); self.page.keyboard.press('ArrowLeft')
            self.page.wait_for_function('albumCurrentPath === "/Other/duplicate.jpg"')
            self.assertEqual(self.page.locator('#albumTags .tag-badge').get_attribute('title'), 'Family / '+name)
            self.page.keyboard.press('Escape')
            self.assertEqual(self.page.locator('#albumTags .tag-badge').count(), 0)
            self.page.set_viewport_size(dict(width=390, height=844))
            self.assertTrue(self.page.locator('#eventDetails').is_visible())
            self.assertTrue(self.page.evaluate('document.querySelector("#timelineMain").scrollWidth <= innerWidth'))
            self.page.locator('#eventDetails').scroll_into_view_if_needed()
            self.page.screenshot(path='/tmp/umedia-event-tags-mobile.png', full_page=True)
            touch_context = self.browser.new_context(viewport=dict(width=390, height=844), is_mobile=True, has_touch=True)
            try:
                touch_page = touch_context.new_page()
                touch_page.on('pageerror', lambda error: self.errors.append(str(error)))
                touch_page.goto(self.page.url)
                touch_page.wait_for_function('eventView && eventView.info')
                touch_page.locator('#eventItems .item').first.tap()
                touch_page.wait_for_function('albumCurrentPath === "/Large/000.jpg"')
                touch_page.wait_for_function('getComputedStyle(document.querySelector("#albumTags")).opacity === "1"')
                self.assertEqual(touch_page.locator('#albumTags .tag-badge').inner_text(), name)
                self.assertTrue(touch_page.evaluate('document.querySelector("#albumTags").scrollWidth <= innerWidth'))
                touch_page.screenshot(path='/tmp/umedia-carousel-tags-mobile.png', full_page=True)
            finally:
                touch_context.close()
        finally:
            with timeline.sqlite_connection(self.server.database) as db:
                db.execute('DELETE FROM ImageTags WHERE tagid BETWEEN 9500 AND 9503')
                db.execute('DELETE FROM Tags WHERE id BETWEEN 9500 AND 9503')

    def test_json_offsets_are_preserved_by_editor(self):
        query = self.server.filter_json(dict(field='creationDate',op='gt',value='2020-01-01T00:00:00+03:00'))
        self.page.goto(self.server.url+'?'+urllib.parse.urlencode(dict(view='timeline',filter=query)))
        self.page.wait_for_function('feed.loaded')
        self.open_editor()
        self.assertEqual(self.page.locator('.filter-date-zone').input_value(), '+03:00')
        self.apply()
        applied = json.loads(urllib.parse.parse_qs(urllib.parse.urlsplit(self.page.url).query)['filter'][0])
        self.assertEqual(applied['rules'][0]['value'], '2020-01-01T00:00:00+03:00')

    def test_composed_or_and_groups_and_reload(self):
        self.open_editor()
        self.assertEqual(self.page.locator('#filterMatch').count(), 0)
        group = self.add_group()
        self.add_rule('tag','Aaron',group=group)
        self.add_rule('tag','Esther',group=group)
        self.add_rule('sensitivity','150','gt')
        self.apply()
        self.assertIn('2 items', self.page.locator('#timelineStatus').inner_text())
        self.assertEqual(self.page.locator('#filterBadge').inner_text(), '3')
        applied = json.loads(urllib.parse.parse_qs(urllib.parse.urlsplit(self.page.url).query)['filter'][0])
        self.assertNotIn('match',applied)
        self.assertEqual(applied['rules'][0]['match'],'any')
        self.page.reload(); self.page.wait_for_function('feed.loaded')
        self.open_editor()
        self.assertEqual(self.page.get_by_label('Group match').input_value(), 'any')
        self.page.get_by_label('Group match').select_option('all')
        self.apply()
        self.assertIn('1 items', self.page.locator('#timelineStatus').inner_text())
        self.open_editor()
        group = self.page.locator('.filter-group').first
        self.add_group(group)
        self.page.get_by_role('button', name='Apply & refresh timeline').click()
        self.assertIn('each group', self.page.locator('#filterError').inner_text())
        nested = group.locator(':scope > .filter-group-rules > .filter-group')
        self.add_rule('make','Nikon',group=nested)
        self.page.set_viewport_size(dict(width=390,height=844))
        self.assertTrue(self.page.evaluate('document.querySelector("#filterForm").scrollWidth <= innerWidth'))
        self.apply()
        self.assertIn('1 items', self.page.locator('#timelineStatus').inner_text())
        self.open_editor()
        self.assertEqual(self.page.locator('.filter-group').count(),2)
        self.page.locator('.filter-group').first.locator(':scope > .filter-group-top').get_by_role('button',name='Remove group').click()
        self.assertEqual(self.page.locator('.filter-rule').count(),1)
        self.page.locator('#filterCancel').click()
        self.open_editor()
        self.assertEqual(self.page.locator('.filter-group').count(),2)


if __name__ == '__main__':
    unittest.main()
