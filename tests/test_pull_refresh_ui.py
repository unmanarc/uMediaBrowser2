"""Touch refresh integration. Run: python3 tests/test_pull_refresh_ui.py [binary]."""
import os
import unittest
from contextlib import ExitStack

from playwright.sync_api import sync_playwright
import test_api_key as api_key


class PullRefreshUITests(unittest.TestCase):
    server = api_key.APIKeyTests.server
    request = api_key.APIKeyTests.request

    def test_touch_refresh_in_both_views(self):
        with self.server(None), sync_playwright() as playwright, ExitStack() as cleanup:
            browser = playwright.chromium.launch(
                executable_path=os.environ.get('CHROME_BINARY', '/usr/bin/google-chrome'),
                headless=True, args=['--no-sandbox', '--disable-dev-shm-usage'])
            cleanup.callback(browser.close)
            context = browser.new_context(viewport={'width': 390, 'height': 844},
                                          is_mobile=True, has_touch=True)
            page = context.new_page()
            errors = []
            page.on('pageerror', lambda error: errors.append(str(error)))
            session = context.new_cdp_session(page)
            for path, selector, action in (('/', '#zoneD', 'ls'),
                                            ('/?view=timeline', '#timelineMain', 'timeline_refresh')):
                with self.subTest(path=path):
                    page.goto(f'http://127.0.0.1:{self.port}{path}')
                    if action == 'ls':
                        page.wait_for_function('state.flags.version !== ""')
                    else:
                        page.wait_for_function('!feed.busy')
                    surface = page.locator(selector)
                    box = surface.bounding_box()
                    x, y = box['x'] + box['width'] / 2, box['y'] + 45

                    def gesture(dx=0, dy=110, cancel=False):
                        session.send('Input.dispatchTouchEvent', {'type': 'touchStart',
                                     'touchPoints': [{'x': x, 'y': y}]})
                        for step in range(1, 9):
                            session.send('Input.dispatchTouchEvent', {'type': 'touchMove',
                                         'touchPoints': [{'x': x + dx * step / 8,
                                                          'y': y + dy * step / 8}]})
                        session.send('Input.dispatchTouchEvent',
                                     {'type': 'touchCancel' if cancel else 'touchEnd', 'touchPoints': []})

                    refreshes = []
                    def record(request):
                        if f'act={action}' in request.url:
                            refreshes.append(request.method)
                    page.on('request', record)
                    # Short, horizontal and cancelled drags must not refresh.
                    gesture(dy=35)
                    gesture(dx=100, dy=20)
                    gesture(cancel=True)
                    page.wait_for_timeout(100)
                    self.assertEqual(refreshes, [])
                    with page.expect_response(lambda response: f'act={action}' in response.url):
                        gesture()
                    page.wait_for_function('document.querySelector(".pull-refresh-indicator").hidden')
                    self.assertEqual(refreshes, ['GET' if action == 'ls' else 'POST'])

                    # Starting below the top keeps ordinary scrolling intact.
                    surface.evaluate('''element => {
                        const filler = document.createElement('div');
                        filler.style.height = '2000px'; element.appendChild(filler);
                        element.scrollTop = 300;
                    }''')
                    gesture()
                    page.wait_for_timeout(100)
                    self.assertEqual(len(refreshes), 1)
                    self.assertLess(surface.evaluate('element => element.scrollTop'), 300)
                    page.remove_listener('request', record)
            self.assertEqual(errors, [])


if __name__ == '__main__':
    unittest.main()
