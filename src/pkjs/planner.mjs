import { createPlanner } from '@irail/planner';
import { normalizeConnection, parseRoute } from './journeys.mjs';

export const DATA_URL = 'https://irail.github.io/iRail-cdn/manifest.jsonld';

export function createPhonePlanner({ makePlanner = createPlanner, fetcher = globalThis.fetch,
                                     now = Date.now, timeoutMs = 45000 } = {}) {
  let planner, initialized = null, refreshed = 0, activeSignal;
  let queue = Promise.resolve();
  // The package's main entry runs directly in this WebView, without Node or a
  // Worker. Apache Arrow's browser entry is selected by the build's conditions.
  function fetchData(url, options = {}) {
    return fetcher(url, { ...options, signal: activeSignal });
  }
  async function run(route) {
    const input = parseRoute(route);
    const controller = new AbortController();
    activeSignal = controller.signal;
    const timer = setTimeout(() => controller.abort(), timeoutMs);
    try {
      if (!planner) planner = makePlanner({ dataUrl: DATA_URL, cacheBytes: 16 * 1024 * 1024, fetch: fetchData });
      if (initialized === null || now() - initialized >= 6 * 3600 * 1000) {
        await planner.init();
        initialized = refreshed = now();
      } else if (now() - refreshed >= 30000) {
        await planner.refreshRealtime();
        refreshed = now();
      }
      const connections = await planner.connections({ ...input, time: new Date(now()) });
      if (controller.signal.aborted) throw new Error('Planning timed out; press Select');
      return {
        from: input.from, to: input.to, updated: Math.floor(now() / 1000),
        journeys: connections.slice(0, 6).map(normalizeConnection)
      };
    } catch (error) {
      if (controller.signal.aborted) {
        // A timed-out load may leave partially populated data. Start fresh on retry.
        planner = undefined;
        initialized = null;
        throw new Error('Planning timed out; press Select');
      }
      throw error;
    } finally {
      clearTimeout(timer);
      activeSignal = undefined;
    }
  }
  return function query(route) {
    const task = queue.then(() => run(route));
    queue = task.catch(() => {});
    return task;
  };
}
