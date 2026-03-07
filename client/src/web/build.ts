// Build script: bundles web frontend into static/ directory

import { copyFileSync, mkdirSync } from 'fs';
import { join, dirname } from 'path';

const ROOT = join(dirname(import.meta.dir), '..', '..');
const STATIC_DIR = join(ROOT, 'static');
const WEB_SRC = import.meta.dir;

mkdirSync(STATIC_DIR, { recursive: true });

// Bundle TypeScript modules into a single JS file
const result = await Bun.build({
  entrypoints: [join(WEB_SRC, 'app.ts')],
  outdir: STATIC_DIR,
  minify: true,
  sourcemap: 'external',
  target: 'browser',
  naming: '[name].[ext]',
});

if (!result.success) {
  console.error('Build failed:');
  for (const log of result.logs) {
    console.error(log);
  }
  process.exit(1);
}

// Copy static assets
copyFileSync(join(WEB_SRC, 'index.html'), join(STATIC_DIR, 'index.html'));
copyFileSync(join(WEB_SRC, 'styles.css'), join(STATIC_DIR, 'styles.css'));
copyFileSync(join(WEB_SRC, 'audio-processor.js'), join(STATIC_DIR, 'audio-processor.js'));

console.log('Web frontend built to static/');
for (const output of result.outputs) {
  console.log(`  ${output.path} (${(output.size / 1024).toFixed(1)} KB)`);
}
