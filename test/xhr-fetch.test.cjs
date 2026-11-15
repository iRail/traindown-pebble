const { test, afterEach } = require('node:test');
const assert = require('node:assert/strict');
const fetchWithXHR = require('../src/pkjs/xhr-fetch');
let request;
global.XMLHttpRequest = class {
  constructor() { request = this; }
  open(method, url) { Object.assign(this, { method, url }); }
  getResponseHeader(key) { return key === 'etag' ? 'sample' : null; }
  send() {}
  abort() { this.onabort(); }
};
afterEach(() => { request = undefined; });

test('decodes UTF-8 JSON from an offset binary view without surrounding bytes', async () => {
  const pending = fetchWithXHR('https://example.org');
  const bytes = new TextEncoder().encode('x{"station":"Liège"}x');
  request.response = bytes.subarray(1, bytes.length - 1);
  request.status = 200;
  request.onload();
  const response = await pending;
  assert.equal(response.ok, true);
  assert.equal(response.headers.get('ETag'), 'sample');
  assert.deepEqual(await response.json(), { station: 'Liège' });
});
test('preserves HEAD and HTTP error status', async () => {
  const pending = fetchWithXHR('https://example.org', { method: 'HEAD' });
  assert.equal(request.method, 'HEAD');
  request.status = 404;
  request.onload();
  const response = await pending;
  assert.equal(response.status, 404);
  assert.equal(response.ok, false);
  assert.equal((await response.arrayBuffer()).byteLength, 0);
});
test('rejects on cancellation before and during a request', async () => {
  const controller = new AbortController();
  const pending = fetchWithXHR('https://example.org', { signal: controller.signal });
  controller.abort();
  await assert.rejects(pending, { name: 'AbortError' });
  await assert.rejects(fetchWithXHR('https://example.org', { signal: controller.signal }), { name: 'AbortError' });
});
