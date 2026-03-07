import type { ClientEvent, ServerEvent, SessionConfig } from "./protocol";

export interface RealtimeClientOptions {
  url: string;
  sessionConfig?: Partial<SessionConfig>;
  connectTimeoutMs?: number;
  pingIntervalMs?: number;
  verbose?: boolean;
}

type EventHandler = (event: ServerEvent) => void;

export class RealtimeClient {
  private ws: WebSocket | null = null;
  private pingTimer: ReturnType<typeof setInterval> | null = null;
  private handlers = new Map<string, EventHandler[]>();
  private opts: Required<RealtimeClientOptions>;

  // Stats
  eventsSent = 0;
  eventsReceived = 0;
  appendsSent = 0;
  bytesAppended = 0;

  constructor(opts: RealtimeClientOptions) {
    this.opts = {
      connectTimeoutMs: 5000,
      pingIntervalMs: 10000,
      verbose: false,
      sessionConfig: {},
      ...opts,
    };
  }

  on(type: string, handler: EventHandler): this {
    const list = this.handlers.get(type) ?? [];
    list.push(handler);
    this.handlers.set(type, list);
    return this;
  }

  private emit(event: ServerEvent) {
    const handlers = this.handlers.get(event.type);
    if (handlers) {
      for (const h of handlers) h(event);
    }
    const allHandlers = this.handlers.get("*");
    if (allHandlers) {
      for (const h of allHandlers) h(event);
    }
  }

  private log(...args: unknown[]) {
    if (this.opts.verbose) {
      console.error("[rt]", ...args);
    }
  }

  async connect(): Promise<void> {
    return new Promise((resolve, reject) => {
      this.log(`connecting to ${this.opts.url}`);
      const ws = new WebSocket(this.opts.url);
      this.ws = ws;
      let settled = false;

      const timeout = setTimeout(() => {
        if (!settled) {
          settled = true;
          ws.close();
          reject(new Error("Connection timeout"));
        }
      }, this.opts.connectTimeoutMs);

      ws.addEventListener("open", () => {
        clearTimeout(timeout);
        this.log("connected");
        this.startPing();

        if (this.opts.sessionConfig && Object.keys(this.opts.sessionConfig).length > 0) {
          this.send({
            type: "transcription_session.update",
            session: this.opts.sessionConfig,
          });
        }

        if (!settled) {
          settled = true;
          resolve();
        }
      });

      ws.addEventListener("message", (e) => {
        this.eventsReceived++;
        try {
          const data = typeof e.data === "string" ? e.data : new TextDecoder().decode(e.data as ArrayBuffer);
          const event = JSON.parse(data) as ServerEvent;
          this.log(`<- ${event.type}`, event.type === "error" ? (event as any).error : "");
          this.emit(event);
        } catch (err) {
          console.error("[rt] parse error:", err);
        }
      });

      ws.addEventListener("error", (e) => {
        this.log("ws error", e);
        if (!settled) {
          settled = true;
          clearTimeout(timeout);
          reject(new Error("WebSocket error"));
        }
      });

      ws.addEventListener("close", (e) => {
        this.stopPing();
        this.ws = null;
        this.log(`closed code=${e.code} reason=${e.reason}`);
        if (!settled) {
          settled = true;
          clearTimeout(timeout);
          reject(new Error(`WebSocket closed: ${e.code}`));
        }
        this.emit({ type: "error", event_id: "", error: { type: "close", code: String(e.code), message: e.reason || "closed", param: null, event_id: null } });
      });
    });
  }

  send(event: ClientEvent): boolean {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return false;
    this.ws.send(JSON.stringify(event));
    this.eventsSent++;
    return true;
  }

  appendAudio(pcm16Base64: string): boolean {
    this.appendsSent++;
    this.bytesAppended += pcm16Base64.length;
    return this.send({ type: "input_audio_buffer.append", audio: pcm16Base64 });
  }

  commit(): boolean {
    return this.send({ type: "input_audio_buffer.commit" });
  }

  clear(): boolean {
    return this.send({ type: "input_audio_buffer.clear" });
  }

  close() {
    this.stopPing();
    if (this.ws) {
      this.ws.close();
      this.ws = null;
    }
  }

  get connected(): boolean {
    return this.ws !== null && this.ws.readyState === WebSocket.OPEN;
  }

  private startPing() {
    this.stopPing();
    this.pingTimer = setInterval(() => {
      this.send({ type: "ping" });
    }, this.opts.pingIntervalMs);
  }

  private stopPing() {
    if (this.pingTimer !== null) {
      clearInterval(this.pingTimer);
      this.pingTimer = null;
    }
  }
}
