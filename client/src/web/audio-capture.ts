// Browser microphone capture, PCM16 buffering, diagnostics, and health monitoring

import { REALTIME_AUDIO_FLUSH_MS, REALTIME_AUDIO_FLUSH_BYTES } from './types';
import { rtLog, errorMessage } from './logger';
import type { RealtimeClient } from './realtime';

type CaptureMode = 'none' | 'audio_worklet' | 'script_processor';

const MIC_ZERO_RMS_THRESHOLD = 0.0005;
const MIC_SIGNAL_DETECTED_RMS = 0.0015;
const MIC_SILENCE_ALERT_MS = 4000;
const MIC_STATS_INTERVAL_MS = 1000;
const MIC_MUTED_RECOVERY_MS = 2500;

function float32ToPcm16Buffer(float32: Float32Array): ArrayBuffer {
  const pcm16 = new Int16Array(float32.length);
  for (let i = 0; i < float32.length; i++) {
    const s = Math.max(-1, Math.min(1, float32[i]));
    pcm16[i] = s < 0 ? s * 32768 : s * 32767;
  }
  return pcm16.buffer;
}

function arrayBufferToBase64(buffer: ArrayBuffer): string {
  const bytes = new Uint8Array(buffer);
  let binary = '';
  for (let i = 0; i < bytes.length; i++) {
    binary += String.fromCharCode(bytes[i]);
  }
  return btoa(binary);
}

function computeRms(samples: Float32Array): number {
  if (samples.length === 0) return 0;
  let sum = 0;
  for (let i = 0; i < samples.length; i++) {
    const s = samples[i];
    sum += s * s;
  }
  return Math.sqrt(sum / samples.length);
}

function downmixInputBuffer(inputBuffer: AudioBuffer): Float32Array {
  const channels = inputBuffer.numberOfChannels;
  const frames = inputBuffer.length;
  if (frames === 0 || channels === 0) return new Float32Array(0);

  if (channels === 1) {
    return new Float32Array(inputBuffer.getChannelData(0));
  }

  const mixed = new Float32Array(frames);
  for (let ch = 0; ch < channels; ch++) {
    const channel = inputBuffer.getChannelData(ch);
    for (let i = 0; i < frames; i++) {
      mixed[i] += channel[i];
    }
  }

  const norm = 1 / channels;
  for (let i = 0; i < frames; i++) {
    mixed[i] *= norm;
  }
  return mixed;
}

export interface AudioSignalStats {
  mode: CaptureMode;
  chunk_rms: number;
  avg_rms: number;
  max_rms: number;
  chunk_count: number;
  zero_chunk_count: number;
  silent_for_ms: number;
  signal_detected: boolean;
  track_muted: boolean;
}

export interface AudioProblem {
  code: 'no_input_signal' | 'track_ended' | 'track_muted';
  message: string;
  stats: AudioSignalStats;
}

export interface AudioSetupOptions {
  deviceId?: string;
}

export class AudioCapture {
  audioContext: AudioContext | null = null;
  private mediaStream: MediaStream | null = null;
  private sourceNode: MediaStreamAudioSourceNode | null = null;
  private processor: AudioWorkletNode | ScriptProcessorNode | null = null;
  analyser: AnalyserNode | null = null;

  private pcmChunks: Uint8Array[] = [];
  private pcmBytes = 0;
  private flushTimer: ReturnType<typeof setInterval> | null = null;
  private healthTimer: ReturnType<typeof setInterval> | null = null;
  private statsTimer: ReturnType<typeof setInterval> | null = null;
  private flushCount = 0;
  private queuedBytesTotal = 0;
  private queueCount = 0;

  private realtime: RealtimeClient | null = null;
  private captureMode: CaptureMode = 'none';
  private chunkCount = 0;
  private zeroChunkCount = 0;
  private rmsSum = 0;
  private maxRms = 0;
  private lastChunkRms = 0;
  private lastNonZeroAt = 0;
  private silentProblemEmitted = false;
  private signalDetected = false;
  private requestedDeviceId = '';
  private activeDeviceId = '';
  private mutedSince = 0;
  private mutedRecoveryRequested = false;

  onRecoveryNeeded: ((reason: string) => void) | null = null;
  onAudioProblem: ((problem: AudioProblem) => void) | null = null;
  onAudioStats: ((stats: AudioSignalStats) => void) | null = null;

  async setup(realtime: RealtimeClient, options: AudioSetupOptions = {}): Promise<void> {
    this.realtime = realtime;
    this.resetSignalStats();
    this.requestedDeviceId = options.deviceId ?? '';

    const constraints: MediaTrackConstraints = {
      channelCount: 1,
      echoCancellation: false,
      noiseSuppression: false,
      autoGainControl: false,
    };
    if (this.requestedDeviceId) {
      constraints.deviceId = { exact: this.requestedDeviceId };
    }

    try {
      this.mediaStream = await navigator.mediaDevices.getUserMedia({ audio: constraints });
    } catch (err) {
      if (!this.requestedDeviceId) throw err;
      const errName = (err as { name?: string })?.name ?? '';
      const canFallback = errName === 'OverconstrainedError' || errName === 'NotFoundError';
      if (!canFallback) throw err;

      rtLog('mic_device_open_failed_fallback_default', {
        requested_device_id: this.requestedDeviceId,
        error: errorMessage(err),
      });

      this.mediaStream = await navigator.mediaDevices.getUserMedia({
        audio: {
          channelCount: 1,
          echoCancellation: false,
          noiseSuppression: false,
          autoGainControl: false,
        },
      });
    }

    rtLog('mic_stream_acquired', { track_count: this.mediaStream.getAudioTracks().length });
    const trackSettings = this.mediaStream.getAudioTracks()[0]?.getSettings?.();
    if (trackSettings) {
      this.activeDeviceId = String(trackSettings.deviceId ?? '');
      rtLog('mic_track_settings', trackSettings as Record<string, unknown>);
    }

    this.mediaStream.getAudioTracks().forEach((track, index) => {
      track.addEventListener('ended', () => {
        const message = `Микрофонный трек завершился (track ${index}).`;
        rtLog('mic_track_ended', { track_id: track.id, track_index: index });
        this.onAudioProblem?.({
          code: 'track_ended',
          message,
          stats: this.snapshotSignalStats(),
        });
        this.onRecoveryNeeded?.(`track_ended_${index}`);
      });

      track.addEventListener('mute', () => {
        this.mutedSince = Date.now();
        rtLog('mic_track_muted', { track_id: track.id, track_index: index });
      });

      track.addEventListener('unmute', () => {
        this.mutedSince = 0;
        this.mutedRecoveryRequested = false;
        rtLog('mic_track_unmuted', { track_id: track.id, track_index: index });
      });
    });

    this.audioContext = new AudioContext();
    if (this.audioContext.state === 'suspended') {
      try {
        await this.audioContext.resume();
      } catch (e) {
        rtLog('audio_context_resume_failed', { error: errorMessage(e) });
      }
    }

    this.audioContext.addEventListener('statechange', () => {
      const state = this.audioContext?.state;
      rtLog('audio_context_statechange', { state });
      if (state === 'suspended') {
        this.audioContext?.resume().catch((e) => {
          rtLog('audio_context_auto_resume_failed', { error: errorMessage(e) });
        });
      }
    });

    rtLog('audio_context_ready', {
      state: this.audioContext.state,
      sample_rate: this.audioContext.sampleRate,
    });

    this.sourceNode = this.audioContext.createMediaStreamSource(this.mediaStream);

    realtime.updateSession({
      input_audio_format: 'pcm16',
      input_sample_rate: this.audioContext.sampleRate,
      input_audio_transcription: { model: 'default' },
    });

    this.analyser = this.audioContext.createAnalyser();
    this.analyser.fftSize = 256;
    this.sourceNode.connect(this.analyser);

    await this.setupCaptureProcessor();
    this.startSignalStats();
  }

  private async setupCaptureProcessor() {
    if (!this.audioContext || !this.sourceNode) return;

    if (this.audioContext.audioWorklet) {
      try {
        await this.audioContext.audioWorklet.addModule('audio-processor.js');
        const worklet = new AudioWorkletNode(this.audioContext, 'asr-audio-processor');
        worklet.port.onmessage = (e) => {
          if (!e.data) return;
          this.handleFloatChunk(new Float32Array(e.data));
        };
        this.sourceNode.connect(worklet);
        worklet.connect(this.audioContext.destination);
        this.processor = worklet;
        this.captureMode = 'audio_worklet';
        rtLog('mic_capture_mode', { mode: this.captureMode });
        return;
      } catch (err) {
        rtLog('audio_worklet_failed', { error: errorMessage(err) });
      }
    }

    this.setupScriptProcessor();
  }

  private setupScriptProcessor() {
    if (!this.audioContext || !this.sourceNode) return;

    const sp = this.audioContext.createScriptProcessor(2048, 1, 1);
    this.sourceNode.connect(sp);
    sp.connect(this.audioContext.destination);
    sp.onaudioprocess = (e) => {
      this.handleFloatChunk(downmixInputBuffer(e.inputBuffer));
    };

    this.processor = sp;
    this.captureMode = 'script_processor';
    rtLog('mic_capture_mode', { mode: this.captureMode });
  }

  private resetSignalStats() {
    this.chunkCount = 0;
    this.zeroChunkCount = 0;
    this.rmsSum = 0;
    this.maxRms = 0;
    this.lastChunkRms = 0;
    this.lastNonZeroAt = Date.now();
    this.silentProblemEmitted = false;
    this.signalDetected = false;
    this.mutedSince = 0;
    this.mutedRecoveryRequested = false;
  }

  private snapshotSignalStats(): AudioSignalStats {
    const now = Date.now();
    return {
      mode: this.captureMode,
      chunk_rms: this.lastChunkRms,
      avg_rms: this.chunkCount > 0 ? this.rmsSum / this.chunkCount : 0,
      max_rms: this.maxRms,
      chunk_count: this.chunkCount,
      zero_chunk_count: this.zeroChunkCount,
      silent_for_ms: Math.max(0, now - this.lastNonZeroAt),
      signal_detected: this.signalDetected,
      track_muted: !!this.mediaStream?.getAudioTracks().some((t) => t.muted),
    };
  }

  private startSignalStats() {
    this.stopSignalStats();
    this.statsTimer = setInterval(() => {
      this.onAudioStats?.(this.snapshotSignalStats());
    }, MIC_STATS_INTERVAL_MS);
  }

  private stopSignalStats() {
    if (this.statsTimer !== null) {
      clearInterval(this.statsTimer);
      this.statsTimer = null;
    }
  }

  private emitNoSignalProblem(silentForMs: number) {
    if (this.silentProblemEmitted) return;
    this.silentProblemEmitted = true;

    const stats = this.snapshotSignalStats();
    const message = `Нет входного сигнала с микрофона (${(silentForMs / 1000).toFixed(1)}с, rms=${stats.avg_rms.toFixed(5)}).`;
    rtLog('mic_no_signal_problem', {
      silent_for_ms: silentForMs,
      mode: stats.mode,
      avg_rms: stats.avg_rms,
      max_rms: stats.max_rms,
      zero_chunk_count: stats.zero_chunk_count,
    });
    this.onAudioProblem?.({
      code: 'no_input_signal',
      message,
      stats,
    });
  }

  private handleFloatChunk(float32: Float32Array) {
    if (!this.realtime || float32.length === 0) return;

    const rms = computeRms(float32);
    this.lastChunkRms = rms;
    this.chunkCount += 1;
    this.rmsSum += rms;
    this.maxRms = Math.max(this.maxRms, rms);
    if (!this.signalDetected && rms >= MIC_SIGNAL_DETECTED_RMS) {
      this.signalDetected = true;
      rtLog('mic_signal_detected', { rms });
    }

    if (rms <= MIC_ZERO_RMS_THRESHOLD) {
      this.zeroChunkCount += 1;
    } else {
      this.lastNonZeroAt = Date.now();
      this.silentProblemEmitted = false;
    }

    const silentForMs = Date.now() - this.lastNonZeroAt;
    // Only report "no input signal" at startup before first real audio appears.
    if (!this.signalDetected && silentForMs >= MIC_SILENCE_ALERT_MS) {
      this.emitNoSignalProblem(silentForMs);
    }

    const pcm16 = float32ToPcm16Buffer(float32);
    this.queuePcm16(pcm16);
  }

  private queuePcm16(buffer: ArrayBuffer) {
    if (!buffer || !this.realtime) return;
    const bytes = new Uint8Array(buffer);
    if (bytes.byteLength === 0) return;

    this.pcmChunks.push(bytes);
    this.pcmBytes += bytes.byteLength;
    this.queueCount += 1;

    if (this.queueCount === 1 || this.queueCount % 250 === 0) {
      rtLog('mic_pcm_queue', {
        queue_count: this.queueCount,
        queued_bytes: this.pcmBytes,
      });
    }

    if (this.pcmBytes >= REALTIME_AUDIO_FLUSH_BYTES) {
      this.flush();
    }
  }

  flush() {
    if (!this.realtime || this.pcmBytes === 0) return;

    const merged = new Uint8Array(this.pcmBytes);
    let offset = 0;
    for (const chunk of this.pcmChunks) {
      merged.set(chunk, offset);
      offset += chunk.length;
    }

    const queuedBytes = this.pcmBytes;
    this.pcmChunks = [];
    this.pcmBytes = 0;

    const base64 = arrayBufferToBase64(merged.buffer);
    const sent = this.realtime.appendAudio(base64);
    this.flushCount += 1;
    this.queuedBytesTotal += queuedBytes;

    if (this.flushCount === 1 || this.flushCount % 50 === 0 || !sent) {
      rtLog('mic_pcm_flush', {
        flush_count: this.flushCount,
        queued_bytes: queuedBytes,
        base64_len: base64.length,
        total_queued_bytes: this.queuedBytesTotal,
        sent,
      });
    }
  }

  startFlushTimer() {
    this.stopFlushTimer();
    this.flushTimer = setInterval(() => this.flush(), REALTIME_AUDIO_FLUSH_MS);
    rtLog('mic_flush_timer_started', { interval_ms: REALTIME_AUDIO_FLUSH_MS });
  }

  stopFlushTimer() {
    if (this.flushTimer !== null) {
      clearInterval(this.flushTimer);
      this.flushTimer = null;
      rtLog('mic_flush_timer_stopped', {});
    }
  }

  startHealthCheck(isRecordingFn: () => boolean) {
    this.stopHealthCheck();
    this.healthTimer = setInterval(() => {
      if (!isRecordingFn()) return;

      if (this.audioContext?.state === 'suspended') {
        rtLog('audio_health_resuming', {});
        this.audioContext.resume().catch(() => {});
      }

      if (this.mediaStream) {
        const tracks = this.mediaStream.getAudioTracks();
        if (tracks.length > 0 && tracks.every((t) => t.readyState === 'ended')) {
          rtLog('audio_health_tracks_dead', {});
          this.onRecoveryNeeded?.('health_check_tracks_ended');
          return;
        }

        const hasMutedTrack = tracks.some((t) => t.readyState === 'live' && t.muted);
        if (hasMutedTrack) {
          if (this.mutedSince === 0) {
            this.mutedSince = Date.now();
          }
          if (!this.mutedRecoveryRequested && Date.now() - this.mutedSince >= MIC_MUTED_RECOVERY_MS) {
            this.mutedRecoveryRequested = true;
            const message = 'Микрофон временно отключен системой (track.muted). Переподключаю...';
            rtLog('audio_health_track_muted_recovery', { muted_for_ms: Date.now() - this.mutedSince });
            this.onAudioProblem?.({
              code: 'track_muted',
              message,
              stats: this.snapshotSignalStats(),
            });
            this.onRecoveryNeeded?.('health_check_track_muted');
          }
        } else {
          this.mutedSince = 0;
          this.mutedRecoveryRequested = false;
        }
      }
    }, 3000);
  }

  stopHealthCheck() {
    if (this.healthTimer !== null) {
      clearInterval(this.healthTimer);
      this.healthTimer = null;
    }
  }

  async teardown() {
    this.stopFlushTimer();
    this.stopHealthCheck();
    this.stopSignalStats();

    if (this.processor) {
      if ('port' in this.processor && typeof this.processor.port.postMessage === 'function') {
        this.processor.port.postMessage('stop');
      }
      this.processor.disconnect();
      this.processor = null;
    }

    if (this.sourceNode) {
      this.sourceNode.disconnect();
      this.sourceNode = null;
    }

    this.analyser = null;

    if (this.mediaStream) {
      this.mediaStream.getTracks().forEach((t) => t.stop());
      this.mediaStream = null;
    }

    if (this.audioContext && this.audioContext.state !== 'closed') {
      await this.audioContext.close();
    }

    this.audioContext = null;
    this.realtime = null;
    this.captureMode = 'none';
    this.requestedDeviceId = '';
    this.activeDeviceId = '';

    this.pcmChunks = [];
    this.pcmBytes = 0;

    this.resetCounters();
    this.resetSignalStats();

    rtLog('mic_capture_torn_down', {});
  }

  resetCounters() {
    this.flushCount = 0;
    this.queuedBytesTotal = 0;
    this.queueCount = 0;
    this.pcmChunks = [];
    this.pcmBytes = 0;
  }

  getDebugInfo() {
    return {
      audio_context_state: this.audioContext?.state || 'none',
      audio_context_sample_rate: this.audioContext?.sampleRate || 0,
      capture_mode: this.captureMode,
      requested_device_id: this.requestedDeviceId,
      active_device_id: this.activeDeviceId,
      media_tracks: this.mediaStream
        ? this.mediaStream.getAudioTracks().map((t) => ({
            id: t.id,
            enabled: t.enabled,
            muted: t.muted,
            ready_state: t.readyState,
          }))
        : [],
      pcm: {
        queued_chunks: this.pcmChunks.length,
        queued_bytes: this.pcmBytes,
        flush_count: this.flushCount,
        queued_bytes_total: this.queuedBytesTotal,
      },
      signal: this.snapshotSignalStats(),
    };
  }
}
