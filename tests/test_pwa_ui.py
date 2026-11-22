"""Chrome install UI tests. Run with playwright available:
python3 tests/test_pwa_ui.py [binary]
"""
import json
import os
import tempfile
import unittest

from playwright.sync_api import sync_playwright
import test_api_key as api_key


class PWAUITests(unittest.TestCase):
    server = api_key.APIKeyTests.server
    request = api_key.APIKeyTests.request

    def test_chrome_manifest_and_install_button_lifecycle(self):
        with self.server(None), tempfile.TemporaryDirectory(prefix="umedia-chrome-") as profile, sync_playwright() as playwright:
            context = playwright.chromium.launch_persistent_context(
                user_data_dir=profile, viewport={"width": 390, "height": 844}, is_mobile=True, has_touch=True,
                executable_path=os.environ.get('CHROME_BINARY', '/usr/bin/google-chrome'),
                headless=True, args=['--no-sandbox', '--disable-dev-shm-usage'])
            try:
                page = context.new_page()
                # Defer native promotion so the synthetic lifecycle stays deterministic.
                page.add_init_script("""window.addEventListener('beforeinstallprompt', event => {
                    if (event.isTrusted) { event.preventDefault(); event.stopImmediatePropagation(); }
                });""")
                errors = []
                page.on('pageerror', lambda error: errors.append(str(error)))
                for path in ('/', '/?view=timeline'):
                    page.goto(f'http://127.0.0.1:{self.port}{path}')
                    button = page.get_by_role('button', name='Install app')
                    self.assertTrue(button.is_hidden())
                    session = context.new_cdp_session(page)
                    manifest = session.send('Page.getAppManifest')
                    self.assertEqual(manifest['errors'], [])
                    self.assertEqual(json.loads(manifest['data'])['display'], 'standalone')
                    self.assertEqual(session.send('Page.getInstallabilityErrors')['installabilityErrors'], [])
                    session.detach()
                    # Exercise the browser event contract without opening an OS install dialog.
                    page.evaluate('''() => {
                        window.installPromptCalls = 0;
                        const event = new Event('beforeinstallprompt', {cancelable:true});
                        event.prompt = async () => { window.installPromptCalls++; };
                        event.userChoice = Promise.resolve({outcome:'dismissed'});
                        window.dispatchEvent(event);
                        if (!event.defaultPrevented) throw new Error('Install event was not deferred');
                    }''')
                    self.assertTrue(button.is_visible())
                    button.click()
                    self.assertEqual(page.evaluate('window.installPromptCalls'), 1)
                    self.assertTrue(button.is_hidden())
                    page.evaluate('''() => {
                        window.dispatchEvent(new Event('beforeinstallprompt', {cancelable:true}));
                        window.dispatchEvent(new Event('appinstalled'));
                    }''')
                    self.assertTrue(button.is_hidden())
                self.assertEqual(errors, [])
            finally:
                context.close()


if __name__ == '__main__':
    unittest.main()
