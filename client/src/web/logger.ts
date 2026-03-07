// Structured logging for realtime ASR debugging

interface LogEntry {
  seq: number;
  ts: string;
  event: string;
  [key: string]: unknown;
}

const logs: LogEntry[] = [];
let seq = 0;

export function rtLog(event: string, payload: Record<string, unknown> = {}) {
  const entry: LogEntry = {
    seq: ++seq,
    ts: new Date().toISOString(),
    event,
    ...payload,
  };
  logs.push(entry);
  if (logs.length > 2000) logs.shift();
  console.log('[ASR-RT]', entry);
}

export function errorMessage(err: unknown): string {
  if (!err) return '';
  if (typeof err === 'string') return err;
  if (err instanceof Error) return err.message;
  return String(err);
}

export function wsReadyStateName(state: number): string {
  switch (state) {
    case WebSocket.CONNECTING: return 'CONNECTING';
    case WebSocket.OPEN: return 'OPEN';
    case WebSocket.CLOSING: return 'CLOSING';
    case WebSocket.CLOSED: return 'CLOSED';
    default: return 'UNKNOWN';
  }
}

// Expose debug helpers on window
(window as any).getAsrRealtimeLogs = () => JSON.stringify(logs, null, 2);
(window as any).clearAsrRealtimeLogs = () => { logs.length = 0; seq = 0; };
export function getDebugState(extra: Record<string, unknown>): string {
  return JSON.stringify({
    generated_at: new Date().toISOString(),
    location: window.location.href,
    user_agent: navigator.userAgent,
    ...extra,
    logs,
  }, null, 2);
}
