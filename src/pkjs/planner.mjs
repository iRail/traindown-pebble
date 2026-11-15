import { createPlanner } from '@irail/planner';
import { normalizeConnection, parseRoute } from './journeys.mjs';

export const DATA_URL = 'https://irail.github.io/iRail-cdn/manifest.jsonld';

// The oldest supported feed dates the board, not the time we calculated it.
function feedUpdated(statuses) {
  const supported = (statuses || []).filter(status => status.supported);
  if (!supported.length) return 0;
  return Math.floor(Math.min(...supported.map(status =>
    status.timestamp > 0 ? status.timestamp * 1000 : status.publishedAt || 0)) / 1000);
}

export function createPhonePlanner({ makePlanner = createPlanner, fetcher = globalThis.fetch,
                                     now = Date.now, timeoutMs = 45000 } = {}) {
  let planner, initialized = null, refreshed = null, updated = 0, activeController;
  let queue = Promise.resolve();
  function fetchData(url, options = {}) {
    return fetcher(url, { ...options, signal: activeController?.signal });
  }
  // Timetable planning and real-time refresh each get a separate deadline.
  async function timed(operation) {
    const controller = activeController;
    const timer = setTimeout(() => controller.abort(), timeoutMs);
    try {
      const result = await operation();
      if (controller.signal.aborted) throw new Error('Aborted');
      return result;
    } catch (error) {
      if (controller.signal.aborted) throw new Error('Planning timed out; press Select');
      throw error;
    } finally {
      clearTimeout(timer);
    }
  }
  async function board(input, updating) {
    const connections = await planner.connections({ ...input, time: new Date(now()) });
    return {
      from: input.from, to: input.to, updated, updating,
      cached: !updated || now() / 1000 - updated > 120,
      journeys: connections.slice(0, 6).map(normalizeConnection)
    };
  }
  async function run(route, preview) {
    const input = parseRoute(route);
    activeController = new AbortController();
    try {
      let needsRefresh;
      const initial = await timed(async () => {
        if (!planner) planner = makePlanner({ dataUrl: DATA_URL, cacheBytes: 16 * 1024 * 1024, fetch: fetchData });
        if (initialized === null || now() - initialized >= 6 * 3600 * 1000) {
          // init() otherwise waits for real-time data too. A new timetable resets its overlay.
          initialized = refreshed = null;
          updated = 0;
          try {
            await planner.init(undefined, { realtime: false });
          } catch (error) {
            planner = undefined;
            throw error;
          }
          initialized = now();
        }
        needsRefresh = refreshed === null || now() - refreshed >= 30000;
        return board(input, needsRefresh);
      });
      if (!needsRefresh) return initial;
      // Wait for the watch transfer before starting the slow part. Returning false
      // lets the transport drop a request superseded by a different route.
      if (preview && await preview(initial) === false) return initial;
      if (activeController.signal.aborted) throw new Error('Request canceled');
      return await timed(async () => {
        const statuses = await planner.refreshRealtime();
        updated = feedUpdated(statuses);
        refreshed = now();
        return board(input, false);
      });
    } finally {
      // Keep timetable pages and the last overlay after failed/aborted refreshes.
      // An interrupted init is retried because initialized stays null.
      activeController = undefined;
    }
  }
  function query(route, preview) {
    const task = queue.then(() => run(route, preview));
    queue = task.catch(() => {});
    return task;
  }
  query.cancel = () => activeController?.abort();
  return query;
}
