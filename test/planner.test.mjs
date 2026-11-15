import assert from 'node:assert/strict';
import { test } from 'node:test';
import { normalizeConnection } from '../src/pkjs/journeys.mjs';
import { createPhonePlanner, DATA_URL } from '../src/pkjs/planner.mjs';

const fixture = () => ({
  departure: new Date('2026-11-10T10:00:00Z'), arrival: new Date('2026-11-10T10:30:00Z'),
  departureDelay: 180, arrivalDelay: 300, transfers: 0,
  legs: [{ type: 'ride', vehicle: { name: 'IC 123' },
    from: { delay: 180, platform: '4', canceled: false }, to: { delay: 300, canceled: false } }]
});

test('expected departure and arrival include each delay exactly once', () => {
  const result = normalizeConnection(fixture());
  assert.equal(result.departure, Date.parse('2026-11-10T10:03:00Z') / 1000);
  assert.equal(result.arrival, Date.parse('2026-11-10T10:35:00Z') / 1000);
  assert.equal(result.delay, 180);
  assert.equal(result.platform, '4');
});

test('canceled connecting train marks the entire journey canceled', () => {
  const connection = fixture();
  connection.legs.push({ type: 'ride', from: { canceled: true }, to: {} });
  connection.legs[0].from.platform = null;
  const result = normalizeConnection(connection);
  assert.equal(result.canceled, true);
  assert.equal(result.platform, '?');
});

const route = { from: 'Gent-Sint-Pieters', to: 'Brussel-Centraal' };

test('phone initializes once, reuses its planner and refreshes real-time data on demand', async () => {
  let clock = 1794304800000, inits = 0, refreshes = 0;
  const query = createPhonePlanner({ now: () => clock, makePlanner: (options) => {
    assert.equal(options.dataUrl, DATA_URL);
    assert.equal(options.cacheBytes, 16 * 1024 * 1024);
    return {
      init: async (url, options) => { assert.deepEqual(options, { realtime: false }); inits++; },
      refreshRealtime: async () => { refreshes++; return [{ supported: true, timestamp: clock / 1000 - 10 }]; },
      connections: async (input) => {
        assert.equal(input.from, route.from);
        assert.equal(input.limit, 6);
        return [fixture()];
      }
    };
  }});
  const board = await query(route);
  assert.equal(board.journeys[0].train, 'IC 123');
  assert.equal(board.updated, clock / 1000 - 10);
  assert.equal(board.cached, false);
  await query(route);
  assert.equal(inits, 1);
  assert.equal(refreshes, 1);
  clock += 31000;
  await query(route);
  assert.equal(refreshes, 2);
  clock += 6 * 3600 * 1000;
  await query(route);
  assert.equal(inits, 2);
});

test('invalid routes do not initialize the planner and failed loads can be retried', async () => {
  let inits = 0;
  const query = createPhonePlanner({ makePlanner: () => ({
    init: async () => { if (++inits === 1) throw new Error('Offline'); },
    refreshRealtime: async () => [],
    connections: async () => []
  })});
  await assert.rejects(query({ from: 'Gent' }), /both station/);
  await assert.rejects(query({ from: 'Gent', to: 'gent' }), /different/);
  assert.equal(inits, 0);
  await assert.rejects(query(route), /Offline/);
  assert.deepEqual((await query(route)).journeys, []);
  assert.equal(inits, 2);
});

test('concurrent requests share one initialization and execute sequentially', async () => {
  let inits = 0, running = 0, peak = 0;
  const query = createPhonePlanner({ makePlanner: () => ({
    init: async () => { inits++; },
    refreshRealtime: async () => [],
    connections: async () => {
      peak = Math.max(peak, ++running);
      await new Promise(resolve => setTimeout(resolve, 5));
      running--;
      return [];
    }
  })});
  await Promise.all([query(route), query(route)]);
  assert.equal(inits, 1);
  assert.equal(peak, 1);
});

test('timeout aborts the download and the next attempt gets a new planner', async () => {
  let planners = 0;
  const query = createPhonePlanner({ timeoutMs: 10,
    fetcher: (url, { signal }) => new Promise((resolve, reject) => {
      signal.addEventListener('abort', () => reject(new Error('Aborted')), { once: true });
    }),
    makePlanner: ({ fetch }) => {
      const first = ++planners === 1;
      return { init: () => first ? fetch(DATA_URL) : Promise.resolve(),
        refreshRealtime: async () => [], connections: async () => [] };
    }
  });
  await assert.rejects(query(route), /timed out/);
  assert.deepEqual((await query(route)).journeys, []);
  assert.equal(planners, 2);
});

test('layover stations are the stops where each train but the last ends', () => {
  const connection = fixture();
  connection.legs[0].to.station = { name: 'Brussel-Zuid' };
  connection.legs.push({ type: 'ride', vehicle: { name: 'IC 9' }, from: { delay: 0 }, to: { delay: 0, station: { name: 'Leuven' } } });
  connection.transfers = 1;
  assert.deepEqual(normalizeConnection(connection).via, ['Brussel-Zuid']);
  assert.deepEqual(normalizeConnection(fixture()).via, []);
});

test('legs carry expected times, platforms and intermediate stops for the trip overview', () => {
  const station = (name) => ({ name });
  const connection = {
    departure: new Date('2026-11-10T10:00:00Z'), arrival: new Date('2026-11-10T11:00:00Z'), transfers: 1,
    legs: [
      { type: 'ride', vehicle: { name: 'IC 1' },
        from: { station: station('Gent-Sint-Pieters'), time: '2026-11-10T10:00:00Z', delay: 120, platform: '7' },
        to: { station: station('Brussel-Noord'), time: '2026-11-10T10:30:00Z', delay: 60, platform: '3' },
        stops: [{ station: station('Lokeren'), arrival: '2026-11-10T10:10:00Z', arrivalDelay: 120 },
                { station: station('Broken'), arrival: 'not a time' }] },
      { type: 'ride', vehicle: { name: 'IC 2' },
        from: { station: station('Brussel-Noord'), time: '2026-11-10T10:37:00Z', delay: 0, platform: null },
        to: { station: station('Leuven'), time: '2026-11-10T11:00:00Z', delay: 0, platform: '2' }, stops: [] }
    ]
  };
  const [first, second] = normalizeConnection(connection).legs;
  assert.equal(first.departure, Date.parse('2026-11-10T10:02:00Z') / 1000);
  assert.equal(first.arrival, Date.parse('2026-11-10T10:31:00Z') / 1000);
  assert.equal(first.delay, 120);
  assert.deepEqual([first.platform, first.toPlatform], ['7', '3']);
  assert.deepEqual(first.stops, [{ name: 'Lokeren', time: Date.parse('2026-11-10T10:12:00Z') / 1000 }]);
  assert.equal(second.platform, '?');
  assert.equal(second.train, 'IC 2');
});

test('a journey whose legs lack times still normalizes, without an overview', () => {
  assert.deepEqual(normalizeConnection(fixture()).legs, []);
});

const deferred = () => {
  let resolve, reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
};

test('a timetable board reaches the watch before a blocked real-time download starts', async () => {
  const seen = [], transfer = deferred(), refreshing = deferred(), download = deferred();
  const query = createPhonePlanner({ now: () => 1794304800000, makePlanner: () => ({
    init: async (url, options) => { assert.equal(options.realtime, false); seen.push('init'); },
    connections: async () => { seen.push('plan'); return [fixture()]; },
    refreshRealtime: () => { seen.push('refresh'); refreshing.resolve(); return download.promise; }
  }) });
  let preview;
  const pending = query(route, async board => { preview = board; seen.push('preview'); await transfer.promise; });
  // The callback is entered before a refresh can start, even with asynchronous Bluetooth.
  while (!preview) await new Promise(resolve => setImmediate(resolve));
  assert.deepEqual(seen, ['init', 'plan', 'preview']);
  assert.equal(preview.updated, 0);
  assert.equal(preview.cached, true);
  assert.equal(preview.updating, true);
  assert.equal(preview.journeys.length, 1);
  transfer.resolve();
  await refreshing.promise;
  download.resolve([{ supported: true, timestamp: 1794304790 }]);
  const final = await pending;
  assert.deepEqual(seen, ['init', 'plan', 'preview', 'refresh', 'plan']);
  assert.equal(final.updated, 1794304790);
  assert.equal(final.updating, false);
  assert.equal(final.cached, false);
});

test('changing route reuses cached data and its original feed age before refreshing', async () => {
  let clock = 1794304800000;
  const query = createPhonePlanner({ now: () => clock, makePlanner: () => ({
    init: async () => {}, connections: async () => [fixture()],
    refreshRealtime: async () => [{ supported: true, timestamp: clock / 1000 - 20 }]
  }) });
  const first = await query(route);
  clock += 31000;
  const nextRoute = { from: 'Leuven', to: 'Gent-Sint-Pieters' };
  let preview;
  const final = await query(nextRoute, board => { preview = board; });
  assert.equal(preview.from, nextRoute.from);
  assert.equal(preview.updated, first.updated);
  assert.equal(preview.updating, true);
  assert.equal(final.updated, clock / 1000 - 20);
});

test('a failed refresh leaves a usable preview and keeps timetable pages for another route', async () => {
  let planners = 0, inits = 0, fail = true;
  const query = createPhonePlanner({ makePlanner: () => {
    planners++;
    return { init: async () => { inits++; }, connections: async () => [fixture()],
      refreshRealtime: async () => { if (fail) throw new Error('Offline'); return []; } };
  } });
  let preview;
  await assert.rejects(query(route, board => { preview = board; }), /Offline/);
  assert.equal(preview.journeys.length, 1);
  assert.equal(preview.updated, 0);
  fail = false;
  assert.equal((await query({ from: 'Leuven', to: 'Gent' })).journeys.length, 1);
  assert.equal(planners, 1);
  assert.equal(inits, 1);
});

test('a timed-out refresh preserves the initialized planner and last feed timestamp', async () => {
  let clock = 1794304800000, planners = 0, refreshes = 0;
  const query = createPhonePlanner({ timeoutMs: 10, now: () => clock,
    fetcher: (url, { signal }) => new Promise((resolve, reject) => {
      signal.addEventListener('abort', () => reject(new Error('Aborted')), { once: true });
    }),
    makePlanner: ({ fetch }) => {
      planners++;
      return { init: async () => {}, connections: async () => [fixture()],
        refreshRealtime: async () => {
          if (++refreshes === 2) return fetch(DATA_URL);
          return [{ supported: true, timestamp: clock / 1000 }];
        } };
    }
  });
  const first = await query(route);
  clock += 31000;
  let preview;
  await assert.rejects(query(route, board => { preview = board; }), /timed out/);
  assert.equal(preview.updated, first.updated);
  assert.equal((await query(route)).updated, clock / 1000);
  assert.equal(planners, 1);
});

test('failed refreshes do not renew the age of the retained real-time overlay', async () => {
  let clock = 1794304800000, refreshes = 0;
  const query = createPhonePlanner({ now: () => clock, makePlanner: () => ({
    init: async () => {}, connections: async () => [fixture()],
    refreshRealtime: async () => {
      if (++refreshes > 1) throw new Error('Offline');
      return [{ supported: true, timestamp: clock / 1000 - 20 }];
    }
  }) });
  const first = await query(route);
  clock += 180000;
  let preview;
  await assert.rejects(query(route, board => { preview = board; }), /Offline/);
  assert.equal(preview.updated, first.updated);
  assert.equal(preview.cached, true);
});

test('old or unknown feed timestamps stay cached even after a successful refresh', async () => {
  for (const statuses of [[], [{ supported: false }], [{ supported: true }],
    [{ supported: true, timestamp: 1794304500 }],
    [{ supported: true, timestamp: 1794304800 }, { supported: true, timestamp: 1794304500 }]]) {
    const query = createPhonePlanner({ now: () => 1794304800000, makePlanner: () => ({
      init: async () => {}, connections: async () => [fixture()], refreshRealtime: async () => statuses
    }) });
    assert.equal((await query(route)).cached, true);
  }
});

test('HTTP publication time dates feeds without an embedded timestamp', async () => {
  const query = createPhonePlanner({ now: () => 1794304800000, makePlanner: () => ({
    init: async () => {}, connections: async () => [],
    refreshRealtime: async () => [{ supported: true, publishedAt: 1794304790000 }]
  }) });
  const board = await query(route);
  assert.equal(board.updated, 1794304790);
  assert.equal(board.cached, false);
});

test('reloading timetable metadata resets old freshness before publishing the next preview', async () => {
  let clock = 1794304800000;
  const query = createPhonePlanner({ now: () => clock, makePlanner: () => ({
    init: async () => {}, connections: async () => [fixture()],
    refreshRealtime: async () => [{ supported: true, timestamp: clock / 1000 }]
  }) });
  await query(route);
  clock += 6 * 3600 * 1000;
  let preview;
  await query(route, board => { preview = board; });
  assert.equal(preview.updated, 0);
  assert.equal(preview.cached, true);
});

test('an obsolete preview skips real-time work and a failed transfer never starts it', async () => {
  let refreshes = 0;
  const query = createPhonePlanner({ makePlanner: () => ({
    init: async () => {}, connections: async () => [],
    refreshRealtime: async () => { refreshes++; return []; }
  }) });
  await query(route, () => false);
  await assert.rejects(query(route, () => { throw new Error('Transfer failed'); }), /Transfer failed/);
  assert.equal(refreshes, 0);
});

test('canceling an in-flight refresh releases a queued route without discarding its timetable', async () => {
  let refreshes = 0, inits = 0;
  const refreshing = deferred();
  const query = createPhonePlanner({
    fetcher: (url, { signal }) => new Promise((resolve, reject) => {
      signal.addEventListener('abort', () => reject(new Error('Aborted')), { once: true });
      refreshing.resolve();
    }),
    makePlanner: ({ fetch }) => ({
      init: async () => { inits++; }, connections: async () => [fixture()],
      refreshRealtime: async () => ++refreshes === 1 ? fetch(DATA_URL) : []
    })
  });
  const old = query(route, () => {});
  const rejection = assert.rejects(old);
  await refreshing.promise;
  query.cancel();
  const next = query({ from: 'Leuven', to: 'Gent' });
  await rejection;
  assert.equal((await next).from, 'Leuven');
  assert.equal(inits, 1);
});
