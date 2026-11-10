import { build } from 'esbuild';
import { readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';

const [keysPath, clayPath, outputPath] = process.argv.slice(2);
if (!outputPath) throw new Error('Usage: build-phone.mjs MESSAGE_KEYS CLAY_JS OUTPUT');
const keys = JSON.parse(await readFile(keysPath, 'utf8'));
const result = await build({
  entryPoints: ['src/pkjs/index.js'],
  outfile: outputPath,
  bundle: true,
  platform: 'browser',
  format: 'iife',
  target: 'chrome100',
  sourcemap: true,
  minify: true,
  metafile: true,
  legalComments: 'inline',
  plugins: [{
    name: 'pebble-packages',
    setup(build) {
      build.onResolve({ filter: /^message_keys$/ }, () => ({ path: 'message_keys', namespace: 'pebble' }));
      build.onLoad({ filter: /.*/, namespace: 'pebble' }, () => ({ contents: `module.exports = ${JSON.stringify(keys)};` }));
      build.onResolve({ filter: /^@rebble\/clay$/ }, () => ({ path: path.resolve(clayPath), namespace: 'clay' }));
      build.onLoad({ filter: /.*/, namespace: 'clay' }, async ({ path }) => ({
        // Clay's Browserify bundle delegates its message_keys import to require.
        contents: `var require = function(name) { if (name === 'message_keys') return ${JSON.stringify(keys)}; throw new Error('Unknown Clay module: ' + name); };\n` + await readFile(path, 'utf8')
      }));
    }
  }]
});
await writeFile(path.join(path.dirname(outputPath), 'phone-metafile.json'), JSON.stringify(result.metafile, null, 2));
console.log(`Phone companion: ${(result.metafile.outputs[path.relative(process.cwd(), outputPath)].bytes / 1024).toFixed(0)} KiB, browser bundle`);
