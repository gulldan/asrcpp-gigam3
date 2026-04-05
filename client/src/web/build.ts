// Build script: bundles web frontend into static/ directory

import { buildWebFrontend } from './bundler';

const result = await buildWebFrontend();

if (!result.success) {
  console.error('Build failed:');
  for (const log of result.logs) {
    console.error(log);
  }
  process.exit(1);
}

console.log('Web frontend built to static/');
for (const output of result.outputs) {
  console.log(`  ${output.path} (${(output.size / 1024).toFixed(1)} KB)`);
}
