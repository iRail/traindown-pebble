// Install missing Web APIs before Clay, the planner or Arrow can evaluate.
const runtime = globalThis;
if (!runtime.TextEncoder || !runtime.TextDecoder) {
  const encoding = require('text-encoding-utf-8');
  runtime.TextEncoder ??= encoding.TextEncoder;
  runtime.TextDecoder ??= encoding.TextDecoder;
}
if (!runtime.URL) require('core-js/modules/web.url.constructor');
if (!Uint8Array.fromBase64) require('core-js/modules/es.uint8-array.from-base64');
if (!runtime.Event || !runtime.EventTarget) {
  const events = require('event-target-shim');
  runtime.Event ??= events.Event;
  runtime.EventTarget ??= events.EventTarget;
}
if (!runtime.AbortController) {
  const abort = require('abort-controller/dist/abort-controller.js');
  runtime.AbortController = abort.AbortController;
  runtime.AbortSignal ??= abort.AbortSignal;
}
if (!runtime.performance) {
  const origin = Date.now();
  runtime.performance = { now: () => Date.now() - origin };
}
if (!runtime.fetch) runtime.fetch = require('./xhr-fetch');
