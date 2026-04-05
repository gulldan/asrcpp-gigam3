import { existsSync, watch } from 'fs';
import { join } from 'path';
import { buildWebFrontend, STATIC_DIR, WEB_SRC } from './bundler';

const RELEVANT_EXTENSIONS = new Set(['.ts', '.html', '.css', '.js']);
const DEBOUNCE_MS = 120;
const DEV_PORT = Number(process.env.PORT || 4173);
const DEV_HOST = process.env.HOST || '0.0.0.0';
const BACKEND_HTTP_ORIGIN = process.env.ASR_BACKEND_ORIGIN || 'http://127.0.0.1:8081';
const BACKEND_WS_ORIGIN = BACKEND_HTTP_ORIGIN.replace(/^http/, 'ws');
const LOCAL_FILES = new Set(['/', '/index.html', '/app.js', '/app.js.map', '/styles.css', '/audio-processor.js']);

type ProxySocketData = {
  targetUrl: string;
  backlog: Array<string | ArrayBuffer | Uint8Array>;
  upstream: WebSocket | null;
};

let rebuildTimer: ReturnType<typeof setTimeout> | null = null;
let buildInProgress = false;
let rebuildQueued = false;

async function runBuild(reason: string) {
  if (buildInProgress) {
    rebuildQueued = true;
    return;
  }

  buildInProgress = true;
  const startedAt = performance.now();
  console.log(`[web] rebuilding (${reason})...`);

  try {
    const result = await buildWebFrontend();
    if (!result.success) {
      console.error('[web] build failed:');
      for (const log of result.logs) {
        console.error(log);
      }
      return;
    }

    const elapsed = (performance.now() - startedAt).toFixed(0);
    console.log(`[web] ready in ${elapsed} ms`);
  } finally {
    buildInProgress = false;
    if (rebuildQueued) {
      rebuildQueued = false;
      void runBuild('queued changes');
    }
  }
}

function scheduleBuild(reason: string) {
  if (rebuildTimer) clearTimeout(rebuildTimer);
  rebuildTimer = setTimeout(() => {
    rebuildTimer = null;
    void runBuild(reason);
  }, DEBOUNCE_MS);
}

function isRelevantFile(filename: string) {
  const dot = filename.lastIndexOf('.');
  if (dot === -1) return false;
  return RELEVANT_EXTENSIONS.has(filename.slice(dot));
}

function localFilePath(pathname: string) {
  const normalized = pathname === '/' ? '/index.html' : pathname;
  if (!LOCAL_FILES.has(normalized)) return null;
  return join(STATIC_DIR, normalized.slice(1));
}

async function serveLocal(pathname: string) {
  const path = localFilePath(pathname);
  if (!path || !existsSync(path)) return null;
  return new Response(Bun.file(path));
}

function proxyRequest(req: Request) {
  const url = new URL(req.url);
  const target = new URL(url.pathname + url.search, BACKEND_HTTP_ORIGIN);
  return fetch(new Request(target, req));
}

const server = Bun.serve<ProxySocketData>({
  hostname: DEV_HOST,
  port: DEV_PORT,
  async fetch(req, serverRef) {
    const url = new URL(req.url);

    if (req.headers.get('upgrade') === 'websocket' && url.pathname === '/v1/realtime') {
      const upgraded = serverRef.upgrade(req, {
        data: {
          targetUrl: `${BACKEND_WS_ORIGIN}${url.pathname}${url.search}`,
          backlog: [],
          upstream: null,
        },
      });
      return upgraded ? undefined : new Response('WebSocket upgrade failed', { status: 500 });
    }

    const local = await serveLocal(url.pathname);
    if (local) return local;

    return proxyRequest(req);
  },
  websocket: {
    open(client) {
      const upstream = new WebSocket(client.data.targetUrl);
      upstream.binaryType = 'arraybuffer';
      client.data.upstream = upstream;

      upstream.addEventListener('open', () => {
        const backlog = client.data.backlog.splice(0);
        for (const message of backlog) {
          upstream.send(message);
        }
      });

      upstream.addEventListener('message', (event) => {
        client.send(event.data);
      });

      upstream.addEventListener('close', (event) => {
        client.close(event.code, event.reason);
      });

      upstream.addEventListener('error', () => {
        client.close(1011, 'Upstream WebSocket error');
      });
    },
    message(client, message) {
      const upstream = client.data.upstream;
      if (upstream && upstream.readyState === WebSocket.OPEN) {
        upstream.send(message);
        return;
      }

      client.data.backlog.push(message);
    },
    close(client, code, reason) {
      const upstream = client.data.upstream;
      if (!upstream) return;
      if (upstream.readyState === WebSocket.OPEN || upstream.readyState === WebSocket.CONNECTING) {
        upstream.close(code, reason);
      }
    },
  },
});

console.log(`[web] watching ${WEB_SRC}`);
console.log(`[web] dev server: http://localhost:${server.port}`);
console.log(`[web] backend proxy: ${BACKEND_HTTP_ORIGIN}`);
await runBuild('startup');

const watcher = watch(WEB_SRC, { recursive: true }, (_eventType, filename) => {
  if (!filename) return;
  if (!isRelevantFile(filename)) return;
  scheduleBuild(filename);
});

process.on('SIGINT', () => {
  watcher.close();
  server.stop(true);
  process.exit(0);
});

await new Promise(() => {});
