import { copyFileSync, mkdirSync } from 'fs';
import { dirname, join } from 'path';

export const ROOT = join(dirname(import.meta.dir), '..', '..');
export const STATIC_DIR = join(ROOT, 'static');
export const WEB_SRC = import.meta.dir;

export async function buildWebFrontend() {
  mkdirSync(STATIC_DIR, { recursive: true });

  const result = await Bun.build({
    entrypoints: [join(WEB_SRC, 'app.ts')],
    outdir: STATIC_DIR,
    minify: true,
    sourcemap: 'external',
    target: 'browser',
    naming: '[name].[ext]',
  });

  if (!result.success) {
    return result;
  }

  copyFileSync(join(WEB_SRC, 'index.html'), join(STATIC_DIR, 'index.html'));
  copyFileSync(join(WEB_SRC, 'styles.css'), join(STATIC_DIR, 'styles.css'));
  copyFileSync(join(WEB_SRC, 'audio-processor.js'), join(STATIC_DIR, 'audio-processor.js'));

  return result;
}
