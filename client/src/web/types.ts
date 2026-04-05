// Shared constants and types for the web frontend

export const REALTIME_WS_URL = `${window.location.protocol === 'https:' ? 'wss' : 'ws'}://${window.location.host}/v1/realtime`;
export const REALTIME_CONNECT_TIMEOUT_MS = 8000;
export const REALTIME_RECONNECT_BASE_DELAY_MS = 1000;
export const REALTIME_RECONNECT_MAX_DELAY_MS = 10000;
export const REALTIME_AUDIO_FLUSH_MS = 40;
export const REALTIME_AUDIO_FLUSH_BYTES = 4096;
export const REALTIME_PING_INTERVAL_MS = 10000;

export interface TranscriptResult {
  text: string;
  duration: number | null;
  timestamp: Date;
}

export interface SessionConfig {
  input_audio_format: string;
  input_sample_rate: number;
  input_audio_transcription?: { model?: string; language?: string; prompt?: string };
  turn_detection?: {
    type: 'server_vad';
    threshold?: number;
    prefix_padding_ms?: number;
    silence_duration_ms?: number;
  } | null;
}
