# Traindown for Pebble 2 Duo and Pebble Time 2

A train countdown app inspired by [iRail Traindown](https://github.com/iRail/fitbit-traindown) and NextTrain. One installable bundle supports both **Flint (Pebble 2 Duo, 144 × 168 monochrome)** and **Emery (Pebble Time 2, 200 × 228 color)**. The Time 2 uses a larger countdown and color indicators alongside the text labels for delays, cancellations and cached data.

**Journey planning runs on your Android phone inside the Pebble app. No hosted backend or separate phone app is required.** The companion includes `@irail/planner` 0.1.3, downloads timetables and real-time updates directly from iRail's CDN, computes journeys locally, and sends compact results to the native C watch interface.

## Features

- One saved station-to-station route, configured in the Pebble phone app.
- Browse the next six journeys, including canceled journeys.
- Countdown, expected departure and arrival, first train/platform, delay and number of changes.
- Refresh on opening and every minute while the watch app is running.
- Persistent last-successful results, marked **Cached** after a failed refresh, disconnection, or two minutes without fresh data.
- Atomic Bluetooth updates: an incomplete transfer leaves the previous board intact.

Times on the watch follow the watch's timezone. Set it to Belgium's timezone for Belgian local train times. Delays are included once in expected times; the extra `+N min` label describes that same delay. A platform shown as `?` is unknown. Cached results do not receive further delay or cancellation updates without a connection.

## Phone requirements

Use the current official Pebble Android app and an up-to-date Android System WebView. Its PebbleKit JS runtime is a Chromium WebView. The browser bundle targets Chromium 100+ and uses modern JavaScript, typed arrays, `fetch`, `AbortController`, `Intl`, and the browser build of Apache Arrow. The planner's main entry runs without Web Workers or Node.js APIs.

The phone keeps an initialized planner while the companion is running, with a **16 MiB connection-page cache** (not a limit on total process memory). It refreshes real-time data on demand at most every 30 seconds and reloads timetable metadata after six hours. Closing the app can discard the phone's in-memory timetable cache; reopening may require fresh downloads. Initial loading uses more data and time than subsequent refreshes. Network operations are canceled after 45 seconds so the watch can offer a retry.

This version targets Android. The iOS Pebble app and older desktop PebbleKit JS simulators have different runtimes and are not supported by this companion build.

## Build and install

Install Node.js **22.12+**, npm, Python 3, and the Pebble SDK with Flint and Emery support, then:

```sh
npm ci
pebble build
pebble install --phone PHONE_IP build/pebble.pbw
```

Enable the phone app's Developer Connection for local installation. Alternatively, sideload the generated `.pbw` through the Pebble mobile app. The bundle includes both Flint and Emery; installation selects the matching binary. Native C supports both watches using one codebase.

The `wscript` bundles the phone code with esbuild, including the compiled Clay settings component and SDK-generated numeric message keys. `enableMultiJS` is intentionally false because the SDK's older webpack cannot parse the planner's modern ESM dependencies. The custom build includes those keys explicitly.

Open Traindown's settings in the mobile app, enter departure/destination station names, and save. The settings page is bundled using `@rebble/clay`. Previously saved routes continue working; any old service URL is ignored.

## Watch controls

| Button | Action |
| --- | --- |
| Up / Down | Previous / next journey |
| Select | Refresh |
| Hold Select | Reverse and save the route, then refresh |
| Back | Exit |

The app needs a connected Pebble phone companion and internet for fresh data. It refreshes only while open. This version does not include background alerts, automatic nearest-station selection, or transfer-by-transfer detail screens.

## Validate

```sh
npm test
pebble build
```

Tests cover delay conversion, cancellation propagation, planner initialization/refresh/retry/timeout behavior, phone message ordering and failure handling, route reversal, and UTF-8 buffer limits.

To exercise the actual bundled companion in Chromium against the live iRail CDN, install a Playwright Chromium browser (`npx playwright-core install chromium`) or set `CHROMIUM_PATH` to an existing Chromium executable, then run:

```sh
npm run test:browser
```

This test simulates the Pebble bridge, opens the bundled settings, saves a route, computes real journeys inside the browser, and checks the resulting watch messages. It reports initial and repeat query timing and download volume in `build/browser-smoke.json`. It does not replace testing the Android app, configuration webview and Bluetooth connection on your phone and watch. Watch layouts can be checked separately in the Flint and Emery emulators with sample AppMessages.
