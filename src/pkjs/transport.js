// Phone-to-watch transport; planning is supplied by the Android companion.
function shortText(value, max) {
  // Limit by UTF-8 bytes without splitting a code point (AppMessage C buffers).
  var chars = unescape(encodeURIComponent(String(value || '')));
  var bytes = Math.min(chars.length, max);
  while (bytes && (chars.charCodeAt(bytes) & 0xC0) === 0x80) bytes--;
  return decodeURIComponent(escape(chars.slice(0, bytes)));
}

function createTransport(options) {
  var busy = false, token = 0, pendingRefresh = false;
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
    if (pendingRefresh) {
      pendingRefresh = false;
      refresh(false);
    }
  }
  function settings() {
    try { return JSON.parse(options.storage.getItem('route') || '{}'); }
    catch (error) { return {}; }
  }
  function status(message) { send({ Kind: 0, Status: shortText(message, 63) }, function () {}, function () {}); }
  function configure(route) {
    var from = String(route.from || '').trim(), to = String(route.to || '').trim();
    if (!from || !to || from.length > 100 || to.length > 100) throw new Error('Enter both station names');
    if (from.toLowerCase() === to.toLowerCase()) throw new Error('Choose two different stations');
    options.storage.setItem('route', JSON.stringify({ from: from, to: to }));
  }
  function sendAll(messages, index) {
    if (index === messages.length) { finish(); return; }
    send(messages[index], function () { sendAll(messages, index + 1); }, function () {
      status('Transfer failed; press Select');
      finish();
    });
  }
  function refresh(reverse) {
    if (busy) { pendingRefresh = true; return; }
    var route = settings();
    if (!route.from || !route.to) { status('Open settings in the phone app'); return; }
    if (reverse) {
      var previous = route.from;
      route.from = route.to;
      route.to = previous;
      options.storage.setItem('route', JSON.stringify(route));
    }
    busy = true;
    status('Updating...');
    var activeToken = ++token;
    options.request({ from: route.from, to: route.to }, function (error, data) {
      if (error) { status(error.message); finish(); return; }
      try {
        if (!data || !Array.isArray(data.journeys) || data.journeys.length > 6 || !isFinite(data.updated)) throw new Error('Invalid planner response');
        var messages = [{ Kind: 1, Token: activeToken, Count: data.journeys.length,
          From: shortText(data.from || route.from, 47), To: shortText(data.to || route.to, 47), Updated: data.updated }];
        data.journeys.forEach(function (journey, index) {
          if (!isFinite(journey.departure) || !isFinite(journey.arrival) || !isFinite(journey.delay) ||
              !isFinite(journey.transfers)) throw new Error('Invalid journey response');
          messages.push({ Kind: 2, Token: activeToken, Index: index, Departure: journey.departure,
            Arrival: journey.arrival, Delay: journey.delay, Platform: shortText(journey.platform || '?', 11),
            Train: shortText(journey.train, 23), Transfers: journey.transfers, Canceled: journey.canceled ? 1 : 0 });
        });
        messages.push({ Kind: 3, Token: activeToken });
        sendAll(messages, 0);
      } catch (invalid) { status(invalid.message); finish(); }
    });
  }
  return { refresh: refresh, configure: configure, status: status };
}

module.exports = { createTransport: createTransport, shortText: shortText };
