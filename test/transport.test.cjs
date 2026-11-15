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

test('changing settings during a request skips obsolete results and plans the new route', function () {
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
  assert.deepEqual(h.messages.filter(function (m) { return m.Kind === 1; }).map(function (m) { return m.From; }), ['Leuven']);
});

test('without a saved route the phone plans Ghent to Brussels South', function () {
  var requested;
  var transport = core.createTransport({
    storage: { getItem: function () { return undefined; }, setItem: function () {} },
    send: function (message, ok) { ok(); },
    request: function (route, done) { requested = route; done(null, { updated: 1, journeys: [] }); }
  });
  transport.refresh(false);
  assert.deepEqual(requested, { from: 'Gent-Sint-Pieters', to: 'Brussel-Zuid' });
});

test('a route picked on the watch is saved, planned and sends layovers with each journey', function () {
  var requested;
  var h = harness(function (route, done) {
    requested = route;
    done(null, { updated: 1794304800, from: route.from, to: route.to, journeys: [
      { departure: 1794305000, arrival: 1794308000, delay: 0, platform: '3', train: 'IC 1', transfers: 2,
        canceled: false, via: ['Brussel-Zuid', 'Leuven'] },
      { departure: 1794306000, arrival: 1794309000, delay: 0, platform: '4', train: 'IC 2', transfers: 0, canceled: false }
    ] });
  });
  h.transport.select('Antwerpen-Centraal', 'Luik-Guillemins');
  assert.deepEqual(requested, { from: 'Antwerpen-Centraal', to: 'Luik-Guillemins' });
  assert.equal(JSON.parse(h.values.route).to, 'Luik-Guillemins');
  var rows = h.messages.filter(function (m) { return m.Kind === 2; });
  assert.equal(rows[0].Via, 'Brussel-Zuid, Leuven');
  assert.equal(rows[1].Via, '');
});

test('an invalid pick from the watch is reported and keeps the saved route', function () {
  var h = harness(function () { assert.fail('must not plan'); });
  h.transport.select('Leuven', 'leuven');
  assert.equal(h.messages.at(-1).Status, 'Choose two different stations');
  assert.equal(JSON.parse(h.values.route).from, 'Gent-Sint-Pieters');
});

function overviewJourney(stopCount) {
  var stops = [];
  for (var i = 0; i < stopCount; i++) stops.push({ name: 'Station ' + i, time: 1794305000 + i * 60 });
  return { departure: 1794305000, arrival: 1794308000, delay: 0, platform: '3', train: 'IC 1', transfers: 1, canceled: false,
    legs: [
      { train: 'IC 1', from: 'Gent-Sint-Pieters', to: 'Brussel-Noord', departure: 1794305000, arrival: 1794306000, delay: 60,
        platform: '7', toPlatform: '3', stops: stops },
      { train: 'IC 2', from: 'Brussel-Noord', to: 'Leuven', departure: 1794306400, arrival: 1794308000, delay: 0,
        platform: '5', toPlatform: '2', stops: [] }
    ] };
}

test('the phone sends the trains and stops of a journey the watch asks for', function () {
  var h = harness(function (route, done) { done(null, { updated: 1, journeys: [overviewJourney(3)] }); });
  h.transport.refresh(false);
  var token = h.messages.find(function (m) { return m.Kind === 1; }).Token;
  h.messages.length = 0;
  h.transport.details(token, 0);
  assert.deepEqual(h.messages.map(function (m) { return m.Kind; }), [4, 5, 6, 5, 7]);
  assert.equal(h.messages[0].Count, 2);
  assert.equal(h.messages[1].From, 'Gent-Sint-Pieters');
  assert.equal(h.messages[1].ToPlatform, '3');
  assert.equal(h.messages[1].Delay, 60);
  assert.equal(h.messages[2].Stops, '1794305000 Station 0\n1794305060 Station 1\n1794305120 Station 2');
  assert.ok(h.messages.every(function (m) { return m.Token === token && m.Index === 0; }));
});

test('long stop lists are split into messages that fit the watch inbox and capped', function () {
  var chunks = core.stopChunks(overviewJourney(60).legs[0].stops);
  assert.ok(chunks.length > 1);
  assert.ok(chunks.every(function (chunk) { return Buffer.byteLength(chunk) < 700; }));
  var lines = chunks.join('\n').split('\n');
  assert.equal(lines.length, 40);
  assert.equal(lines[0], '1794305000 Station 0');
});

test('details of an unknown or expired board are refused instead of guessed', function () {
  var h = harness(function (route, done) { done(null, { updated: 1, journeys: [overviewJourney(1)] }); });
  h.transport.refresh(false);
  h.messages.length = 0;
  h.transport.details(999, 0);
  assert.deepEqual(h.messages.map(function (m) { return m.Kind; }), [8]);
  assert.match(h.messages[0].Status, /unavailable/);
  h.messages.length = 0;
  h.transport.details(h.values.token || 1, 5);
  assert.equal(h.messages[0].Kind, 8);
});

test('only the last three boards can be asked for details', function () {
  var h = harness(function (route, done) { done(null, { updated: 1, journeys: [overviewJourney(1)] }); });
  var tokens = [];
  for (var i = 0; i < 4; i++) {
    h.transport.refresh(false);
    tokens.push(h.messages.filter(function (m) { return m.Kind === 1; }).at(-1).Token);
  }
  h.messages.length = 0;
  h.transport.details(tokens[0], 0);
  assert.equal(h.messages[0].Kind, 8);
  h.messages.length = 0;
  h.transport.details(tokens[3], 0);
  assert.equal(h.messages[0].Kind, 4);
});

test('preview and refreshed boards commit separately and keep distinct detail snapshots', async function () {
  var complete, preview;
  var h = harness(function (route, done, partial) { complete = done; preview = partial; });
  h.transport.refresh(false);
  await preview({ updated: 100, updating: true, cached: true, journeys: [overviewJourney(1)] });
  var first = h.messages.find(function (m) { return m.Kind === 1; });
  assert.equal(first.Updated, 100);
  assert.equal(first.Freshness, 2);
  assert.deepEqual(h.messages.map(function (m) { return m.Kind; }), [0, 1, 2, 3]);
  var updated = overviewJourney(1);
  updated.legs[0].platform = '9';
  complete(null, { updated: 110, updating: false, cached: false, journeys: [updated] });
  var boards = h.messages.filter(function (m) { return m.Kind === 1; });
  assert.notEqual(boards[0].Token, boards[1].Token);
  assert.equal(boards[1].Freshness, 0);
  h.messages.length = 0;
  h.transport.details(first.Token, 0);
  assert.equal(h.messages[1].Platform, '7');
  h.messages.length = 0;
  h.transport.details(boards[1].Token, 0);
  assert.equal(h.messages[1].Platform, '9');
});

test('a refresh failure preserves the committed preview and its data age', async function () {
  var complete, preview;
  var h = harness(function (route, done, partial) { complete = done; preview = partial; });
  h.transport.refresh(false);
  await preview({ updated: 100, updating: true, journeys: [overviewJourney(1)] });
  complete(new Error('Offline'));
  assert.equal(h.messages.filter(function (m) { return m.Kind === 3; }).length, 1);
  assert.equal(h.messages.find(function (m) { return m.Kind === 1; }).Updated, 100);
  assert.equal(h.messages.at(-1).Status, 'Offline');
});

test('preview awaits the final Bluetooth acknowledgment and a failed row rejects it', async function () {
  var complete, preview, ack, kinds = [];
  var h = harness(function (route, done, partial) { complete = done; preview = partial; }, function (message, ok, fail) {
    kinds.push(message.Kind);
    if (message.Kind === 3) ack = ok; else ok();
  });
  h.transport.refresh(false);
  var committed = false;
  var pending = preview({ updated: 0, updating: true, journeys: [] }).then(function () { committed = true; });
  await Promise.resolve();
  assert.equal(committed, false);
  ack();
  await pending;
  assert.equal(committed, true);
  complete(new Error('Offline'));
  assert.deepEqual(kinds, [0, 1, 3, 0]);

  h = harness(function (route, done, partial) { preview = partial; }, function (message, ok, fail) {
    if (message.Kind === 2) fail(); else ok();
  });
  h.transport.refresh(false);
  await assert.rejects(preview({ updated: 0, updating: true, journeys: [overviewJourney(1)] }), /Transfer failed/);
});

test('obsolete previews skip their transfer and cannot replace the newly selected route', async function () {
  var requests = [], cancels = 0;
  var values = {}, messages = [];
  var transport = core.createTransport({
    storage: { getItem: function (k) { return values[k]; }, setItem: function (k, v) { values[k] = v; } },
    send: function (message, ok) { messages.push(message); ok(); },
    cancel: function () { cancels++; },
    request: function (route, done, partial) { requests.push({ route: route, done: done, preview: partial }); }
  });
  transport.refresh(false);
  transport.select('Leuven', 'Gent');
  assert.equal(cancels, 1);
  assert.equal(await requests[0].preview({ updated: 0, updating: true, journeys: [] }), false);
  requests[0].done(null, { updated: 110, journeys: [] });
  assert.equal(messages.some(function (m) { return m.Kind === 1; }), false);
  requests[1].done(null, { updated: 120, cached: true, journeys: [] });
  var board = messages.find(function (m) { return m.Kind === 1; });
  assert.equal(board.From, 'Leuven');
  assert.equal(board.Freshness, 1);
});

test('reversing a route during a refresh saves and plans the reversal', function () {
  var requests = [];
  var h = harness(function (route, done) { requests.push({ route: route, done: done }); });
  h.transport.refresh(false);
  h.transport.refresh(true);
  requests[0].done(null, { updated: 123, journeys: [] });
  assert.deepEqual(requests[1].route, { from: 'Liège-Guillemins', to: 'Gent-Sint-Pieters' });
  requests[1].done(null, { updated: 124, journeys: [] });
});
