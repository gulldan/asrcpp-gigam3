// Browser RealtimeClient with auto-reconnection, keepalive, and structured logging

import {
  REALTIME_CONNECT_TIMEOUT_MS,
  REALTIME_RECONNECT_BASE_DELAY_MS,
  REALTIME_RECONNECT_MAX_DELAY_MS,
  REALTIME_PING_INTERVAL_MS,
  type SessionConfig,
} from './types';
import { rtLog, errorMessage, wsReadyStateName } from './logger';

const textDecoder = new TextDecoder('utf-8');

async function parseWSData(payload: unknown): Promise<any> {
  if (typeof payload === 'string') return JSON.parse(payload);
  if (payload instanceof ArrayBuffer) return JSON.parse(textDecoder.decode(payload));
  if (payload instanceof Blob) return JSON.parse(textDecoder.decode(await payload.arrayBuffer()));
  return null;
}

export class RealtimeClient {
  ws: WebSocket | null = null;
  private connectPromise: Promise<void> | null = null;
  intentionalClose = false;
  reconnectAttempts = 0;
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  private pingTimer: ReturnType<typeof setInterval> | null = null;
  private pendingSessionConfig: Partial<SessionConfig> | null = null;

  eventsSent = 0;
  eventsReceived = 0;
  appendSent = 0;
  appendBytesSent = 0;
  pingsSent = 0;
  lastReceivedType = '';

  onConnected: (() => void) | null = null;
  onDisconnected: ((intentional: boolean) => void) | null = null;
  onReconnectScheduled: ((delayMs: number, attempt: number) => void) | null = null;
  onTranscript: ((text: string, event: any) => void) | null = null;
  onSpeechState: ((active: boolean, event: any) => void) | null = null;
  onError: ((err: Error) => void) | null = null;

  constructor(private url: string) {}

  async connect(): Promise<void> {
    if (this.ws?.readyState === WebSocket.OPEN) return;
    if (this.connectPromise) return this.connectPromise;

    this.connectPromise = new Promise<void>((resolve, reject) => {
      rtLog('rt_ws_connect_start', { url: this.url, reconnect_attempt: this.reconnectAttempts + 1 });
      const ws = new WebSocket(this.url);
      this.ws = ws;
      let settled = false;

      const timeout = setTimeout(() => {
        if (ws.readyState === WebSocket.CONNECTING) ws.close();
        if (!settled) {
          settled = true;
          this.connectPromise = null;
          rtLog('rt_ws_connect_timeout', { timeout_ms: REALTIME_CONNECT_TIMEOUT_MS });
          reject(new Error('Таймаут подключения Realtime WebSocket'));
        }
      }, REALTIME_CONNECT_TIMEOUT_MS);

      ws.onopen = () => {
        clearTimeout(timeout);
        this.reconnectAttempts = 0;
        this.clearReconnectTimer();
        this.startKeepalive();
        rtLog('rt_ws_open', { has_pending_session: !!this.pendingSessionConfig });
        if (this.pendingSessionConfig) {
          this.send({ type: 'transcription_session.update', session: this.pendingSessionConfig });
        }
        this.onConnected?.();
        if (!settled) { settled = true; this.connectPromise = null; resolve(); }
      };

      ws.onmessage = async (e) => {
        try {
          const event = await parseWSData(e.data);
          if (event) this.handleEvent(event);
        } catch (err) {
          rtLog('rt_ws_message_parse_error', { error: errorMessage(err) });
          this.onError?.(err as Error);
        }
      };

      ws.onerror = () => {
        rtLog('rt_ws_error', { ready_state: wsReadyStateName(ws.readyState) });
        if (!settled) {
          clearTimeout(timeout);
          settled = true;
          this.connectPromise = null;
          reject(new Error('Ошибка Realtime WebSocket'));
        }
      };

      ws.onclose = (event) => {
        clearTimeout(timeout);
        this.stopKeepalive();
        if (this.ws === ws) this.ws = null;
        rtLog('rt_ws_close', {
          code: event?.code, reason: event?.reason || '',
          clean: !!event?.wasClean, intentional: this.intentionalClose,
        });
        this.onDisconnected?.(this.intentionalClose);
        if (!this.intentionalClose) this.scheduleReconnect();
      };
    });

    return this.connectPromise;
  }

  private scheduleReconnect() {
    if (this.reconnectTimer !== null || this.intentionalClose) return;
    this.reconnectAttempts += 1;
    const delay = Math.min(
      REALTIME_RECONNECT_MAX_DELAY_MS,
      REALTIME_RECONNECT_BASE_DELAY_MS * (2 ** Math.max(0, this.reconnectAttempts - 1))
    );
    this.onReconnectScheduled?.(delay, this.reconnectAttempts);
    rtLog('rt_ws_reconnect_scheduled', { attempt: this.reconnectAttempts, delay_ms: delay });
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      this.connect().catch(() => {
        rtLog('rt_ws_reconnect_failed', { attempt: this.reconnectAttempts });
        this.scheduleReconnect();
      });
    }, delay);
  }

  private clearReconnectTimer() {
    if (this.reconnectTimer !== null) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
  }

  close() {
    this.intentionalClose = true;
    this.clearReconnectTimer();
    this.stopKeepalive();
    rtLog('rt_ws_close_requested', {
      has_ws: !!this.ws,
      ready_state: this.ws ? wsReadyStateName(this.ws.readyState) : 'none',
    });
    if (this.ws && (this.ws.readyState === WebSocket.CONNECTING || this.ws.readyState === WebSocket.OPEN)) {
      this.ws.close();
    }
    this.ws = null;
  }

  send(event: Record<string, unknown>): boolean {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
      const type = (event?.type as string) || '<unknown>';
      if (type !== 'input_audio_buffer.append') {
        rtLog('rt_ws_send_dropped', { type, reason: 'socket_not_open' });
      }
      return false;
    }
    this.ws.send(JSON.stringify(event));
    this.eventsSent += 1;
    return true;
  }

  updateSession(config: Partial<SessionConfig>) {
    this.pendingSessionConfig = config;
    rtLog('rt_session_update_sent', {
      input_audio_format: config?.input_audio_format || '',
      input_sample_rate: config?.input_sample_rate || 0,
      turn_detection: config?.turn_detection?.type || 'null',
    });
    this.send({ type: 'transcription_session.update', session: config });
  }

  appendAudio(base64Pcm16: string): boolean {
    this.appendSent += 1;
    this.appendBytesSent += base64Pcm16?.length ?? 0;
    const sent = this.send({ type: 'input_audio_buffer.append', audio: base64Pcm16 });
    if (this.appendSent === 1 || this.appendSent % 100 === 0 || !sent) {
      rtLog('rt_audio_append_sent', {
        append_count: this.appendSent,
        base64_len: base64Pcm16?.length ?? 0,
        sent,
      });
    }
    return sent;
  }

  commitBuffer(): boolean {
    const sent = this.send({ type: 'input_audio_buffer.commit' });
    rtLog('rt_audio_commit_sent', { sent });
    return sent;
  }

  clearBuffer(): boolean {
    const sent = this.send({ type: 'input_audio_buffer.clear' });
    rtLog('rt_audio_clear_sent', { sent });
    return sent;
  }

  private startKeepalive() {
    this.stopKeepalive();
    this.pingTimer = setInterval(() => {
      const sent = this.send({ type: 'ping' });
      if (sent) this.pingsSent += 1;
      if (this.pingsSent === 1 || this.pingsSent % 6 === 0 || !sent) {
        rtLog('rt_ping_sent', { count: this.pingsSent, sent });
      }
    }, REALTIME_PING_INTERVAL_MS);
  }

  private stopKeepalive() {
    if (this.pingTimer !== null) {
      clearInterval(this.pingTimer);
      this.pingTimer = null;
    }
  }

  private handleEvent(event: any) {
    this.eventsReceived += 1;
    this.lastReceivedType = event?.type || '<missing>';

    switch (event.type) {
      case 'session.created':
        rtLog('rt_event_session_created', { event_id: event.event_id || '' });
        return;
      case 'transcription_session.updated':
        rtLog('rt_event_session_updated', { event_id: event.event_id || '', sample_rate: event?.session?.input_sample_rate || 0 });
        return;
      case 'input_audio_buffer.committed':
        rtLog('rt_event_buffer_committed', { event_id: event.event_id || '', item_id: event.item_id || '' });
        return;
      case 'input_audio_buffer.cleared':
        rtLog('rt_event_buffer_cleared', { event_id: event.event_id || '' });
        return;
      case 'input_audio_buffer.speech_started':
        rtLog('rt_event_speech_started', { item_id: event.item_id || '', audio_start_ms: event.audio_start_ms || 0 });
        this.onSpeechState?.(true, event);
        return;
      case 'input_audio_buffer.speech_stopped':
        rtLog('rt_event_speech_stopped', { item_id: event.item_id || '', audio_end_ms: event.audio_end_ms || 0 });
        this.onSpeechState?.(false, event);
        return;
      case 'conversation.item.input_audio_transcription.delta':
        rtLog('rt_event_transcription_delta', { item_id: event.item_id || '', delta_len: (event.delta || '').length });
        return;
      case 'conversation.item.input_audio_transcription.completed':
        rtLog('rt_event_transcription_completed', { item_id: event.item_id || '', transcript_len: (event.transcript || '').length });
        this.onTranscript?.(event.transcript || '', event);
        return;
      case 'error':
        rtLog('rt_event_error', { code: event?.error?.code || '', message: event?.error?.message || '' });
        this.onError?.(new Error(event?.error?.message || 'Realtime error'));
        return;
      default:
        rtLog('rt_event_unknown', { type: event.type || '<missing>' });
    }
  }

  getDebugInfo() {
    return {
      ready_state: this.ws ? wsReadyStateName(this.ws.readyState) : 'none',
      reconnect_attempts: this.reconnectAttempts,
      append_sent: this.appendSent,
      append_bytes_sent: this.appendBytesSent,
      events_sent: this.eventsSent,
      events_received: this.eventsReceived,
      pings_sent: this.pingsSent,
      last_received_type: this.lastReceivedType,
    };
  }
}
