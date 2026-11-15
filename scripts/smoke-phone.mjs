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
  // Hold the actual real-time feed: timetable planning must finish before it.
  const manifestUrl = 'https://irail.github.io/iRail-cdn/manifest.jsonld';
  const manifest = await (await page.request.get(manifestUrl)).json();
  const realtimeUrls = new Set([].concat(manifest.realtime || []).map(feed => new URL(feed['@id'], manifestUrl).href));
  assert(realtimeUrls.size, 'The live manifest should advertise a real-time feed');
  let releaseFeed, feedRequested, mode = 'hold', feedRequests = 0;
  const held = new Promise(resolve => { releaseFeed = resolve; });
  const requested = new Promise(resolve => { feedRequested = resolve; });
  await page.route(url => realtimeUrls.has(url.href), async request => {
    feedRequests++;
    feedRequested();
    if (mode === 'hold') await held;
    if (mode === 'fail') await request.abort(); else await request.continue();
  });
  // Install a controllable clock before the planner captures Date.now.
  // Reload after installing so the companion also uses this clock.
  await page.addInitScript(() => {
    const realNow = Date.now;
    window.clockOffset = 0;
    Date.now = () => realNow() + window.clockOffset;
  });
  await page.reload();
  async function completedBoards() {
    return page.evaluate(keys => messages.filter(m => m[keys.Kind] === 1 &&
      messages.some(end => end[keys.Kind] === 3 && end[keys.Token] === m[keys.Token])), keys);
  }
  async function waitForBoard(after, updating) {
    await page.waitForFunction(({keys, after, updating}) => {
      const boards = messages.filter(m => m[keys.Kind] === 1 &&
        messages.some(end => end[keys.Kind] === 3 && end[keys.Token] === m[keys.Token]));
      return boards.slice(after).some(m => (m[keys.Freshness] === 2) === updating);
    }, {keys, after, updating}, {timeout: 100000});
    return (await completedBoards()).slice(after).find(m => (m[keys.Freshness] === 2) === updating);
  }
  const started = Date.now();
  await page.evaluate(() => fire('ready'));
  const preview = await waitForBoard(0, true);
  const coldMs = Date.now() - started;
  assert.equal(preview[keys.Updated], 0, 'Timetable-only results have no real-time timestamp');
  await requested;
  // No second board can arrive while the feed is held. Details must still work.
  assert.equal((await completedBoards()).length, 1);
  await page.evaluate(({keys, token}) => fire('appmessage', { payload: {
    [keys.Command]: 4, [keys.Token]: token, [keys.Index]: 0
  }}), {keys, token: preview[keys.Token]});
  await page.waitForFunction(({keys, token}) => messages.some(m => m[keys.Kind] === 7 &&
    m[keys.Token] === token), {keys, token: preview[keys.Token]}, {timeout: 10000});
  mode = 'pass';
  releaseFeed();
  const final = await waitForBoard(1, false);
  assert(final[keys.Updated] > 0, 'A successful live feed should supply its data timestamp');
  const first = await page.evaluate(() => ({ messages, configUrl }));
  const rows = first.messages.filter(m => m[keys.Kind] === 2 && m[keys.Token] === preview[keys.Token]);
  assert(rows.length > 0 && rows.length <= 6, 'Timetable query should find up to six trains');
  const coldBytes = transferred;

  // Save settings only after the first request finishes, so this cannot be
  // mistaken for the explicit refresh tested next.
  let count = (await completedBoards()).length;
  await page.evaluate(() => {
    fire('showConfiguration');
    fire('webviewclosed', { response: encodeURIComponent(JSON.stringify({
      from: { value: 'Gent-Sint-Pieters' }, to: { value: 'Brussel-Centraal' }
    })) });
  });
  const configured = await waitForBoard(count, false);
  assert.equal(configured[keys.To], 'Brussel-Centraal');
  const settings = await page.evaluate(() => ({ configUrl, route: JSON.parse(localStorage.getItem('route')) }));
  assert(settings.configUrl.startsWith('data:text/html'));
  assert.equal(settings.route.to, 'Brussel-Centraal');
  assert(!('backend' in settings.route));

  count = (await completedBoards()).length;
  const before = feedRequests, warmStart = Date.now();
  await page.evaluate(keys => {
    window.clockOffset += 31000;
    fire('appmessage', { payload: { [keys.Command]: 1 } });
  }, keys);
  const warmPreview = await waitForBoard(count, true);
  const warmMs = Date.now() - warmStart;
  assert.equal(warmPreview[keys.Updated], final[keys.Updated], 'Replanning must retain the old feed age');
  const refreshed = await waitForBoard(count + 1, false);
  assert(feedRequests > before, 'Explicit refresh should check the real-time feed');

  // A failed live refresh must leave its preview and original data age intact.
  mode = 'fail';
  count = (await completedBoards()).length;
  const messageCount = await page.evaluate(() => messages.length);
  await page.evaluate(keys => {
    window.clockOffset += 31000;
    fire('appmessage', { payload: { [keys.Command]: 1 } });
  }, keys);
  const failedPreview = await waitForBoard(count, true);
  await page.waitForFunction(({keys, after}) => messages.slice(after).some(m =>
    m[keys.Kind] === 0 && /Cannot load/.test(m[keys.Status])), {keys, after: messageCount}, {timeout: 60000});
  assert.equal((await completedBoards()).length, count + 1, 'Failed refresh must keep the preview');
  assert.equal(failedPreview[keys.Updated], refreshed[keys.Updated]);
  assert.deepEqual(errors, []);
  const summary = { journeys: rows.length, coldMs, warmMs, previewBeforeRealtime: true,
    detailsDuringRefresh: true, explicitRealtimeRefresh: true, failedRefreshRetainsPreview: true,
    downloadedMiB: Number((coldBytes / 1024 / 1024).toFixed(2)), hosts: [...hosts], errors };
  await writeFile('build/browser-smoke.json', JSON.stringify({ ...summary, sampleMessages: first.messages }, null, 2));
  console.log(JSON.stringify(summary, null, 2));
} finally {
  await browser.close();
}
