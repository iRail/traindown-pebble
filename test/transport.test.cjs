var assert = require('node:assert/strict');
var test = require('node:test');
var core = require('../src/pkjs/transport');

function harness(request, send) {
  var values = {}, messages = [];
  var transport = core.createTransport({
    storage: { getItem: function (key) { return values[key]; }, setItem: function (key, value) { values[key] = value; } },
    send: send || function (message, ok) { messages.push(message); ok(); }, request: request
  });
  transport.configure({ from: 'Gent-Sint-Pieters', to: 'Liège-Guillemins' });
  return { transport: transport, messages: messages, values: values };
}

test('phone sends an atomic board in order and reverses the saved route', function () {
  var requested;
  var h = harness(function (route, done) {
    requested = route;
    done(null, { updated: 1794304800, journeys: [{ departure: 1794305000, arrival: 1794308000,
      delay: 120, platform: '8', train: 'IC 123', transfers: 1, canceled: true }] });
  });
  h.transport.refresh(true);
  assert.deepEqual(requested, { from: 'Liège-Guillemins', to: 'Gent-Sint-Pieters' });
  assert.deepEqual(h.messages.map(function (m) { return m.Kind; }), [0, 1, 2, 3]);
  assert.equal(h.messages[2].Canceled, 1);
  assert.equal(h.messages[2].Departure, 1794305000);
  assert.equal(h.messages[1].Token, h.messages[3].Token);
  assert.equal(JSON.parse(h.values.route).from, 'Liège-Guillemins');
});

test('a failed Bluetooth row never commits a partial board', function () {
  var kinds = [];
  var h = harness(function (url, done) {
    done(null, { updated: 123, journeys: [{ departure: 200, arrival: 300, delay: 0, transfers: 0 }] });
  }, function (message, ok, fail) {
    kinds.push(message.Kind);
    if (message.Kind === 2) fail(); else ok();
  });
  h.transport.refresh(false);
  assert.equal(kinds.includes(3), false);
  assert.deepEqual(kinds, [0, 1, 2, 0]);
});

test('network errors and malformed data retain the previous watch board', function () {
  var h = harness(function (url, done) { done(new Error('Offline')); });
  h.transport.refresh(false);
  assert.equal(h.messages.at(-1).Status, 'Offline');
  assert.equal(h.messages.some(function (m) { return m.Kind === 1; }), false);
  h = harness(function (url, done) { done(null, { updated: 123, journeys: [{}] }); });
  h.transport.refresh(false);
  assert.equal(h.messages.at(-1).Status, 'Invalid journey response');
});

test('station labels fit C buffers without splitting UTF-8 characters', function () {
  var result = core.shortText('é'.repeat(30), 47);
  assert.equal(Buffer.byteLength(result), 46);
  assert.equal(result, 'é'.repeat(23));
  assert.equal(core.shortText('Liège', 47), 'Liège');
});

test('phone rejects missing and identical stations', function () {
  var h = harness(function () {});
  assert.throws(function () { h.transport.configure({ from: 'Gent' }); }, /both station/);
  assert.throws(function () { h.transport.configure({ from: 'Gent', to: 'gent' }); }, /different/);
});

test('changing settings during a request refreshes the new route after committing the old one', function () {
  var callbacks = [], routes = [];
  var h = harness(function (route, done) { routes.push(route); callbacks.push(done); });
  h.transport.refresh(false);
  h.transport.configure({ from: 'Leuven', to: 'Gent' });
  h.transport.refresh(false);
  assert.equal(routes.length, 1);
  callbacks[0](null, { updated: 123, journeys: [] });
  assert.equal(routes.length, 2);
  assert.deepEqual(routes[1], { from: 'Leuven', to: 'Gent' });
  callbacks[1](null, { updated: 124, journeys: [] });
  assert.deepEqual(h.messages.filter(function (m) { return m.Kind === 1; }).map(function (m) { return m.From; }), ['Gent-Sint-Pieters', 'Leuven']);
});
