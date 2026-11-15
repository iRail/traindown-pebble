// Exercise the built companion without any browser globals, using the shape
// of the iOS XHR response (base64 decoded to Uint8Array in the JS context).
import vm from 'node:vm';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

const events = {}, messages = [], values = new Map();
let configUrl;
const context = vm.createContext({
  console, setTimeout, clearTimeout, atob, btoa, navigator: { language: 'en' },
  localStorage: {
    getItem: k => values.get(k) ?? null,
    setItem: (k, v) => values.set(k, String(v))
  },
  Pebble: {
    addEventListener: (key, fn) => { events[key] = fn; },
    getActiveWatchInfo: () => ({ platform: 'flint' }),
    getAccountToken: () => '', getWatchToken: () => '',
    openURL: url => { configUrl = url; },
    sendAppMessage: (msg, ok) => { messages.push(msg); setTimeout(ok, 1); }
  },
  fetchBytes: async (url, method) => {
    const response = await fetch(url, { method });
    return { status: response.status, headers: Object.fromEntries(response.headers),
      body: Buffer.from(await response.arrayBuffer()).toString('base64') };
  }
});
vm.runInContext(`
  globalThis.window = globalThis;
  delete Uint8Array.fromBase64;
  globalThis.XMLHttpRequest = class {
    open(method, url) { this.method = method; this.url = url; }
    abort() { this.onabort(); }
    getResponseHeader(key) { return this.headers[key.toLowerCase()] ?? null; }
    async send() {
      try {
        const reply = await fetchBytes(this.url, this.method);
        this.status = reply.status;
        this.headers = reply.headers;
        this.response = Uint8Array.fromBase64(reply.body);
        this.onload();
      } catch (error) { console.error(error); this.onerror(); }
    }
  };
`, context);
const keys = JSON.parse(await readFile('build/js/message_keys.json', 'utf8'));
vm.runInContext(await readFile('build/pebble-js-app.js', 'utf8'), context);
// Large station metadata must decode even on engines with a small call stack.
// Run with node --stack-size=256 to reproduce the original iPhone failure.
const decoded = vm.runInContext(`
  const stationNames = 'Liège 🚆 '.repeat(20000);
  new TextDecoder().decode(new TextEncoder().encode(stationNames)) === stationNames;
`, context);
assert.equal(decoded, true, 'Large UTF-8 metadata must round-trip');
events.showConfiguration();
assert.match(configUrl, /^data:text\/html/);
console.log('Companion loaded and generated its settings page.');
if (process.argv.includes('--live')) {
  events.ready();
  const until = Date.now() + 55000;
  while (!messages.some(m => m[keys.Kind] === 3) && Date.now() < until) {
    await new Promise(resolve => setTimeout(resolve, 100));
    const failure = messages.find(m => /timed out|Cannot load/i.test(m[keys.Status]));
    assert.ok(!failure, JSON.stringify(failure));
  }
  assert.ok(messages.some(m => m[keys.Kind] === 2), 'Expected a journey');
  assert.ok(messages.some(m => m[keys.Kind] === 3), 'Expected a completed board');
  console.log('Received a live journey board.');
}
