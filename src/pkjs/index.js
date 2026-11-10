var Clay = require('@rebble/clay');
var clay = new Clay(require('./config.json'), null, { autoHandleEvents: false });
var core = require('./transport');
var keys = require('message_keys');
var query = require('./planner.mjs').createPhonePlanner();
var transport = core.createTransport({
  send: function (message, ok, fail) {
    var payload = {};
    Object.keys(message).forEach(function (key) { payload[keys[key]] = message[key]; });
    Pebble.sendAppMessage(payload, ok, fail);
  },
  request: function (route, done) {
    query(route).then(function (board) { done(null, board); }, function (error) {
      console.error('iRail planner:', error);
      var message = error.status === 400 || error.status === 404 || /timed out/.test(error.message) ?
        error.message : 'Cannot load trains; check internet';
      done(new Error(message));
    });
  },
  storage: localStorage
});

Pebble.addEventListener('ready', function () { transport.refresh(false); });
Pebble.addEventListener('appmessage', function (event) {
  var command = event.payload.Command || event.payload[keys.Command];
  if (command) transport.refresh(command === 2);
});
Pebble.addEventListener('showConfiguration', function () {
  // Route reversal also updates the form the next time it is opened.
  try { clay.setSettings(JSON.parse(localStorage.getItem('route') || '{}')); } catch (error) {}
  Pebble.openURL(clay.generateUrl());
});
Pebble.addEventListener('webviewclosed', function (event) {
  if (!event.response || event.response === 'CANCELLED') return;
  try {
    var settings = clay.getSettings(event.response, false);
    Object.keys(settings).forEach(function (key) {
      if (settings[key] && typeof settings[key] === 'object') settings[key] = settings[key].value;
    });
    transport.configure({ from: settings.from, to: settings.to });
    transport.refresh(false);
  } catch (error) { transport.status(error.message || 'Please check settings'); }
});
