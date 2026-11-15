// The planner needs GET and HEAD, JSON and binary responses. Pebble's iOS
// bridge returns a Uint8Array even when its responseType is "arraybuffer".
module.exports = function fetchWithXHR(input, { method = 'GET', signal, headers = {} } = {}) {
  return new Promise((resolve, reject) => {
    const url = String(input);
    const xhr = new XMLHttpRequest();
    function abortError() {
      const error = new Error('Request aborted');
      error.name = 'AbortError';
      return error;
    }
    function abort() {
      try { xhr.abort(); } finally { fail(abortError()); }
    }
    function cleanup() { signal?.removeEventListener('abort', abort); }
    function fail(error) { cleanup(); reject(error); }
    if (signal?.aborted) return fail(abortError());
    xhr.open(method, url);
    xhr.responseType = 'arraybuffer';
    for (const [key, value] of Object.entries(headers)) xhr.setRequestHeader(key, value);
    xhr.onload = () => {
      cleanup();
      const body = xhr.response;
      async function arrayBuffer() {
        if (body instanceof ArrayBuffer) return body;
        if (!body) return new ArrayBuffer(0);
        if (body.byteOffset === 0 && body.byteLength === body.buffer.byteLength) return body.buffer;
        return body.buffer.slice(body.byteOffset, body.byteOffset + body.byteLength);
      }
      resolve({
        ok: xhr.status >= 200 && xhr.status < 300,
        status: xhr.status,
        url: xhr.responseURL || url,
        headers: { get: key => xhr.getResponseHeader(key.toLowerCase()) },
        arrayBuffer,
        json: async () => JSON.parse(new TextDecoder().decode(await arrayBuffer()))
      });
    };
    xhr.onerror = () => fail(new TypeError('Network request failed'));
    xhr.ontimeout = () => fail(new Error('Request timed out'));
    xhr.onabort = () => fail(abortError());
    signal?.addEventListener('abort', abort);
    try { xhr.send(null); } catch (error) { fail(error); }
  });
};
