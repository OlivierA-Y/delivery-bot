import { build } from 'esbuild';
import { readFile, writeFile } from 'node:fs/promises';
const bundle = await build({entryPoints: ['viewer.js'], bundle: true, minify: true,
  write: false, format: 'iife', legalComments: 'inline'});
const template = await readFile('viewer-template.html', 'utf8');
await writeFile('motor-mount-3d.html', template.replace('/* INLINE_MODEL_SCRIPT */', () =>
  bundle.outputFiles[0].text.replaceAll('</script', '<\\/script')));
console.log('Built standalone motor-mount-3d.html (no network or server required).');
