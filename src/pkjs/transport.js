// Phone-to-watch transport; planning is supplied by the phone companion.
function shortText(value, max) {
  // Limit by UTF-8 bytes without splitting a code point (AppMessage C buffers).
  var chars = unescape(encodeURIComponent(String(value || '')));
  var bytes = Math.min(chars.length, max);
  while (bytes && (chars.charCodeAt(bytes) & 0xC0) === 0x80) bytes--;
  return decodeURIComponent(escape(chars.slice(0, bytes)));
}

var DEFAULT_ROUTE = { from: 'Gent-Sint-Pieters', to: 'Brussel-Zuid' };
// Limits of the watch's trip overview (MAX_LEGS and MAX_STOPS in src/c/detail.c).
var MAX_LEGS = 6, MAX_STOPS = 40, CHUNK_BYTES = 600;

// The stops of one train as lines of "epoch name", split so each message stays far below
// the watch's 1024-byte AppMessage inbox.
function stopChunks(stops) {
  var chunks = [], current = '';
  stops.slice(0, MAX_STOPS).forEach(function (stop) {
    var line = Math.floor(stop.time) + ' ' + shortText(stop.name, 27);
    if (current && current.length + line.length + 1 > CHUNK_BYTES) { chunks.push(current); current = ''; }
    current += (current ? '\n' : '') + line;
  });
  if (current) chunks.push(current);
  return chunks;
}

// Messages describing one journey in full: Kind 4 starts, 5 is a train, 6 carries its stops,
// 7 ends. The watch ignores anything whose Token and Index are not the ones it asked for.
function detailMessages(token, index, legs) {
  var base = { Token: token, Index: index };
  var messages = [Object.assign({ Kind: 4, Count: Math.min(legs.length, MAX_LEGS) }, base)];
  legs.slice(0, MAX_LEGS).forEach(function (leg, number) {
    messages.push(Object.assign({ Kind: 5, Leg: number, From: shortText(leg.from, 43), To: shortText(leg.to, 43),
      Train: shortText(leg.train, 23), Departure: leg.departure, Arrival: leg.arrival, Delay: leg.delay,
      Platform: shortText(leg.platform, 11), ToPlatform: shortText(leg.toPlatform, 11) }, base));
    stopChunks(leg.stops).forEach(function (chunk) {
      messages.push(Object.assign({ Kind: 6, Leg: number, Stops: chunk }, base));
    });
  });
  messages.push(Object.assign({ Kind: 7 }, base));
  return messages;
}

function createTransport(options) {
  var busy = false, token = 0, pendingRefresh = false, activeRoute;
  // The last few boards with their trains, so the watch can ask for a journey's details.
  var boards = {}, boardTokens = [];
  var outgoing = [], sending = false;
  function pump() {
    if (sending || !outgoing.length) return;
    sending = true;
    var item = outgoing.shift();
    options.send(item.message, function () {
      sending = false;
      item.ok();
      pump();
    }, function () {
      sending = false;
      item.fail();
      pump();
    });
  }
  function send(message, ok, fail) {
    outgoing.push({ message: message, ok: ok, fail: fail });
    pump();
  }
  function finish() {
    busy = false;
    activeRoute = undefined;
    if (pendingRefresh) {
      pendingRefresh = false;
      refresh(false);
    }
  }
  function settings() {
    var route;
    try { route = JSON.parse(options.storage.getItem('route') || '{}'); }
    catch (error) { route = {}; }
    // Out of the box: Ghent to Brussels South.
    return route.from && route.to ? route : { from: DEFAULT_ROUTE.from, to: DEFAULT_ROUTE.to };
  }
  function status(message) { send({ Kind: 0, Status: shortText(message, 63) }, function () {}, function () {}); }
  function configure(route) {
    var from = String(route.from || '').trim(), to = String(route.to || '').trim();
    if (!from || !to || from.length > 100 || to.length > 100) throw new Error('Enter both station names');
    if (from.toLowerCase() === to.toLowerCase()) throw new Error('Choose two different stations');
    options.storage.setItem('route', JSON.stringify({ from: from, to: to }));
  }
  function sendAll(messages, index, done) {
    if (index === messages.length) { done(); return; }
    send(messages[index], function () { sendAll(messages, index + 1, done); }, function () {
      done(new Error('Transfer failed; press Select'));
    });
  }
  function refresh(reverse) {
    var route = settings();
    if (reverse) {
      var previous = route.from;
      route.from = route.to;
      route.to = previous;
      options.storage.setItem('route', JSON.stringify(route));
    }
    if (busy) {
      pendingRefresh = true;
      if (options.cancel && (route.from !== activeRoute.from || route.to !== activeRoute.to)) options.cancel();
      return;
    }
    busy = true;
    activeRoute = route;
    status('Updating...');
    function superseded() {
      var current = settings();
      return route.from !== current.from || route.to !== current.to;
    }
    function transfer(data, done) {
      if (superseded()) { done(); return; }
      try {
        var activeToken = ++token;
        if (!data || !Array.isArray(data.journeys) || data.journeys.length > 6 || !isFinite(data.updated)) throw new Error('Invalid planner response');
        var messages = [{ Kind: 1, Token: activeToken, Count: data.journeys.length,
          From: shortText(data.from || route.from, 47), To: shortText(data.to || route.to, 47), Updated: data.updated,
          Freshness: data.updating ? 2 : data.cached ? 1 : 0 }];
        data.journeys.forEach(function (journey, index) {
          if (!isFinite(journey.departure) || !isFinite(journey.arrival) || !isFinite(journey.delay) ||
              !isFinite(journey.transfers)) throw new Error('Invalid journey response');
          messages.push({ Kind: 2, Token: activeToken, Index: index, Departure: journey.departure,
            Arrival: journey.arrival, Delay: journey.delay, Platform: shortText(journey.platform || '?', 11),
            Train: shortText(journey.train, 23), Transfers: journey.transfers, Canceled: journey.canceled ? 1 : 0,
            Via: shortText((journey.via || []).join(', '), 47) });
        });
        messages.push({ Kind: 3, Token: activeToken });
        boards[activeToken] = data.journeys;
        boardTokens.push(activeToken);
        while (boardTokens.length > 3) delete boards[boardTokens.shift()];
        sendAll(messages, 0, done);
      } catch (invalid) { done(invalid); }
    }
    options.request({ from: route.from, to: route.to }, function (error, data) {
      if (superseded()) { finish(); return; }
      if (error) { status(error.message); finish(); return; }
      transfer(data, function (error) {
        if (error) status(error.message);
        finish();
      });
    }, function (data) {
      // Two complete atomic transfers, each with its own detail token. Do not
      // release busy between them: the request still owns the real-time phase.
      return new Promise(function (resolve, reject) {
        transfer(data, function (error) {
          if (error) reject(error);
          else resolve(!superseded());
        });
      });
    });
  }
  // The watch wants the trains and stops of one journey of a board it received.
  function details(boardToken, index) {
    var journey = boards[boardToken] && boards[boardToken][index];
    var messages = journey && journey.legs && journey.legs.length ?
      detailMessages(boardToken, index, journey.legs) :
      [{ Kind: 8, Token: boardToken, Index: index, Status: 'Trip details unavailable\nPress Back and refresh' }];
    (function next(position) {
      if (position === messages.length) return;
      send(messages[position], function () { next(position + 1); }, function () {});
    })(0);
  }
  // The watch picked new stations: remember them and plan the new route.
  function select(from, to) {
    try { configure({ from: from, to: to }); }
    catch (error) { status(error.message); return; }
    refresh(false);
  }
  return { refresh: refresh, configure: configure, select: select, details: details, status: status };
}

module.exports = { createTransport: createTransport, shortText: shortText, stopChunks: stopChunks };
