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
      init: async () => { inits++; },
      refreshRealtime: async () => { refreshes++; },
      connections: async (input) => {
        assert.equal(input.from, route.from);
        assert.equal(input.limit, 6);
        return [fixture()];
      }
    };
  }});
  const board = await query(route);
  assert.equal(board.journeys[0].train, 'IC 123');
  assert.equal(board.updated, clock / 1000);
  await query(route);
  assert.equal(inits, 1);
  assert.equal(refreshes, 0);
  clock += 31000;
  await query(route);
  assert.equal(refreshes, 1);
  clock += 6 * 3600 * 1000;
  await query(route);
  assert.equal(inits, 2);
});

test('invalid routes do not initialize the planner and failed loads can be retried', async () => {
  let inits = 0;
  const query = createPhonePlanner({ makePlanner: () => ({
    init: async () => { if (++inits === 1) throw new Error('Offline'); },
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
      return { init: () => first ? fetch(DATA_URL) : Promise.resolve(), connections: async () => [] };
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
