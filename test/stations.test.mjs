import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import { fold, stationNames } from '../scripts/build-stations.mjs';

const names = readFileSync(new URL('../resources/stations.txt', import.meta.url), 'utf8').trim().split('\n');

test('the embedded station list is sorted by folded name, which the watch relies on', () => {
  const keys = names.map(fold);
  assert.deepEqual(keys, [...keys].sort());
  assert.equal(new Set(names).size, names.length);
});

test('the default route and typical stations are in the list and fit the watch buffers', () => {
  for (const name of ['Gent-Sint-Pieters', 'Brussel-Zuid', 'Leuven', 'Luik-Guillemins']) assert.ok(names.includes(name), name);
  assert.ok(names.every((name) => Buffer.byteLength(name) <= 43));
  assert.ok(Buffer.byteLength(names.join('\n')) < 16 * 1024);
});

test('folding drops accents the way the watch does', () => {
  assert.equal(fold('Liège'), 'liege');
  assert.equal(fold("'s Gravenbrakel"), "'s gravenbrakel");
});

test('stations the watch fonts cannot draw are skipped', () => {
  const { names: kept, skipped } = stationNames([{ name: 'Leuven' }, { name: 'Ville-Pommerœul' }]);
  assert.deepEqual(kept, ['Leuven']);
  assert.deepEqual(skipped, ['Ville-Pommerœul']);
});
