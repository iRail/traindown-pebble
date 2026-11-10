// Exercises the actual bundled PKJS code in Chromium with Pebble's bridge
// simulated. All timetable/network/Arrow/planning code runs in the browser.
import { chromium } from 'playwright-core';
import { readFile, writeFile } from 'node:fs/promises';
import { pathToFileURL } from 'node:url';
import path from 'node:path';
import assert from 'node:assert/strict';

const keys = JSON.parse(await readFile('build/js/message_keys.json', 'utf8'));
const html = `<!doctype html><meta charset="utf-8"><title>Traindown phone smoke test</title>
<script>
localStorage.clear();
window.events = {}; window.messages = []; window.configUrl = null;
window.Worker = function () { throw new Error('This companion must work without Workers'); };
window.Pebble = {
  addEventListener: (name, callback) => (events[name] ||= []).push(callback),
  sendAppMessage: (message, ok) => { messages.push(message); setTimeout(ok, 5); },
  getActiveWatchInfo: () => ({ platform: 'emery' }),
  getAccountToken: () => '', getWatchToken: () => '',
  openURL: (url) => { configUrl = url; }
};
window.fire = (name, event = {}) => events[name].forEach(callback => callback(event));
</script>
<script src="pebble-js-app.js"></script>`;
await writeFile('build/phone-smoke.html', html);
const browser = await chromium.launch({
  executablePath: process.env.CHROMIUM_PATH || chromium.executablePath(),
  headless: true
});
try {
  const page = await browser.newPage();
  const errors = [], hosts = new Set();
  let transferred = 0;
  page.on('pageerror', error => errors.push(error.message));
  page.on('console', message => { if (message.type() === 'error') console.error(message.text()); });
  page.on('request', request => {
    if (request.url().startsWith('https://')) hosts.add(new URL(request.url()).host);
  });
  const cdp = await page.context().newCDPSession(page);
  await cdp.send('Network.enable');
  cdp.on('Network.loadingFinished', ({ encodedDataLength }) => { transferred += encodedDataLength; });
  await page.goto(pathToFileURL(path.resolve('build/phone-smoke.html')).href);
  assert.deepEqual(errors, [], 'Phone bundle must load without Node globals');
  const started = Date.now();
  await page.evaluate(() => {
    fire('ready');
    fire('showConfiguration');
    fire('webviewclosed', { response: encodeURIComponent(JSON.stringify({
      from: { value: 'Gent-Sint-Pieters' }, to: { value: 'Brussel-Centraal' }
    })) });
  });
  await page.waitForFunction(keys => messages.some(message => message[keys.Kind] === 3) ||
    messages.some(message => message[keys.Kind] === 0 && /Cannot|timed out|Unknown/.test(message[keys.Status])), keys, { timeout: 60000 });
  const first = await page.evaluate(() => ({ messages, configUrl, route: JSON.parse(localStorage.getItem('route')) }));
  assert.deepEqual(errors, []);
  assert(first.messages.some(m => m[keys.Kind] === 3), JSON.stringify(first.messages));
  const rows = first.messages.filter(m => m[keys.Kind] === 2);
  assert(rows.length > 0 && rows.length <= 6, 'Live query should find up to six trains');
  assert.equal(first.route.from, 'Gent-Sint-Pieters');
  assert(!('backend' in first.route));
  assert(first.configUrl.startsWith('data:text/html'));
  const coldMs = Date.now() - started;
  const coldBytes = transferred;
  const warmStart = Date.now();
  await page.evaluate(keys => fire('appmessage', { payload: { [keys.Command]: 1 } }), keys);
  await page.waitForFunction(keys => messages.filter(m => m[keys.Kind] === 3).length >= 2, keys, { timeout: 60000 });
  const summary = { journeys: rows.length, coldMs, warmMs: Date.now() - warmStart,
    downloadedMiB: Number((coldBytes / 1024 / 1024).toFixed(2)), hosts: [...hosts], errors };
  await writeFile('build/browser-smoke.json', JSON.stringify({ ...summary, sampleMessages: first.messages }, null, 2));
  console.log(JSON.stringify(summary, null, 2));
} finally {
  await browser.close();
}
