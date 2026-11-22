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

    def add_rule(self, field, value, op=None, negate=False, group=None):
        if group is None: self.page.locator('#filterAdd').click()
        else: group.locator(':scope > .filter-group-actions').get_by_role('button', name='+ Add condition', exact=True).click()
        row = self.page.locator('.filter-rule').last if group is None else group.locator(':scope > .filter-group-rules > .filter-rule').last
        row.get_by_label('Field', exact=True).select_option(field)
        if op: row.get_by_label('Comparison', exact=True).select_option(op)
        row.get_by_label('Value', exact=True).fill(value)
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
        self.add_rule('tag', '<script>does not exist</script>')
        self.apply()
        self.assertIn('No pictures or videos match these filters', self.page.locator('#pageMessage').inner_text())
        self.assertEqual(self.page.locator('.story').count(), 0)
        self.page.reload()
        self.page.wait_for_function('feed.loaded')
        self.open_editor()
        self.assertEqual(self.page.locator('.filter-rule input').input_value(), '<script>does not exist</script>')

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
