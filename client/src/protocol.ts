// Realtime API protocol types

export interface SessionConfig {
  input_audio_format: "pcm16" | "g711_ulaw" | "g711_alaw";
  input_sample_rate: number;
  input_audio_transcription?: {
    model?: string;
    language?: string;
    prompt?: string;
  };
  turn_detection?: {
    type: "server_vad";
    threshold?: number;
    prefix_padding_ms?: number;
    silence_duration_ms?: number;
  } | null;
}

// Client -> Server events
export interface SessionUpdateEvent {
  type: "transcription_session.update" | "session.update";
  event_id?: string;
  session: Partial<SessionConfig>;
}

export interface AudioAppendEvent {
  type: "input_audio_buffer.append";
  event_id?: string;
  audio: string; // base64
}

export interface AudioCommitEvent {
  type: "input_audio_buffer.commit";
  event_id?: string;
}

export interface AudioClearEvent {
  type: "input_audio_buffer.clear";
  event_id?: string;
}

export interface PingEvent {
  type: "ping";
  event_id?: string;
}

export type ClientEvent =
  | SessionUpdateEvent
  | AudioAppendEvent
  | AudioCommitEvent
  | AudioClearEvent
  | PingEvent;

// Server -> Client events
export interface SessionCreatedEvent {
  type: "session.created";
  event_id: string;
  session: Record<string, unknown>;
}

export interface SessionUpdatedEvent {
  type: "transcription_session.updated";
  event_id: string;
  session: Record<string, unknown>;
}

export interface SpeechStartedEvent {
  type: "input_audio_buffer.speech_started";
  event_id: string;
  audio_start_ms: number;
  item_id: string;
}

export interface SpeechStoppedEvent {
  type: "input_audio_buffer.speech_stopped";
  event_id: string;
  audio_end_ms: number;
  item_id: string;
}

export interface BufferCommittedEvent {
  type: "input_audio_buffer.committed";
  event_id: string;
  item_id: string;
  previous_item_id: string | null;
}

export interface BufferClearedEvent {
  type: "input_audio_buffer.cleared";
  event_id: string;
}

export interface TranscriptionDeltaEvent {
  type: "conversation.item.input_audio_transcription.delta";
  event_id: string;
  item_id: string;
  content_index: number;
  delta: string;
}

export interface TranscriptionCompletedEvent {
  type: "conversation.item.input_audio_transcription.completed";
  event_id: string;
  item_id: string;
  content_index: number;
  transcript: string;
}

export interface ErrorEvent {
  type: "error";
  event_id: string;
  error: {
    type: string;
    code: string;
    message: string;
    param: string | null;
    event_id: string | null;
  };
}

export type ServerEvent =
  | SessionCreatedEvent
  | SessionUpdatedEvent
  | SpeechStartedEvent
  | SpeechStoppedEvent
  | BufferCommittedEvent
  | BufferClearedEvent
  | TranscriptionDeltaEvent
  | TranscriptionCompletedEvent
  | ErrorEvent;
