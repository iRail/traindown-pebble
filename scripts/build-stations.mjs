// Builds resources/stations.txt, the station list embedded in the watch app.
//
// The names come from @irail/planner's stations() (the iRail CDN's station
// table), labelled in Dutch like the journeys, so every name resolves in the
// planner. Usage (needs internet): node scripts/build-stations.mjs
//
// The file has one name per line, sorted by the "folded" name (lower case,
// accents removed). The watch relies on that order: every typed prefix then
// matches one contiguous range of lines.
import { writeFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { createPlanner } from '@irail/planner';

const DATA_URL = 'https://irail.github.io/iRail-cdn/manifest.jsonld';

// Must match fold_next() in src/c/stations.c: ASCII letters lower-cased and each
// Latin-1 letter (U+00C0..U+00FF) reduced to a base letter.
const LATIN1_BASE = 'aaaaaaaceeeeiiiidnoooooxouuuuyts';
export function fold(name) {
  return [...name].map((char) => {
    const code = char.charCodeAt(0);
    if (code >= 0xc0 && code <= 0xff) return LATIN1_BASE[(code - 0xc0) & 31];
    return char >= 'A' && char <= 'Z' ? char.toLowerCase() : char;
  }).join('');
}

// Names the watch can store and draw: printable ASCII and Latin-1 letters.
const SUPPORTED = /^[\x20-\x7eÀ-ÿ]{1,40}$/;

export function stationNames(stations) {
  const names = new Set();
  const skipped = [];
  for (const station of stations) {
    const name = String(station.name).trim();
    if (SUPPORTED.test(name)) names.add(name); else skipped.push(name);
  }
  const keyed = [...names].map((name) => ({ name, key: fold(name) }));
  keyed.sort((a, b) => (a.key < b.key ? -1 : a.key > b.key ? 1 : a.name < b.name ? -1 : 1));
  return { names: keyed.map((entry) => entry.name), skipped };
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  const planner = createPlanner({ dataUrl: DATA_URL });
  await planner.init(undefined, { realtime: false });
  const { names, skipped } = stationNames(await planner.stations({ language: 'nl' }));
  await writeFile('resources/stations.txt', names.join('\n') + '\n', 'utf8');
  console.log(`Wrote ${names.length} stations; skipped ${skipped.length}: ${skipped.join(', ')}`);
  process.exit(0); // the planner keeps worker threads alive
}
