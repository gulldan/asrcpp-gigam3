// Main entry point: wires all modules together

import { REALTIME_WS_URL } from './types';
import { dom, initTabs, setStatus, addResult, setMicUI, setMicDeviceNote } from './ui';
import { rtLog, errorMessage } from './logger';
import { getDebugState } from './logger';
import { RealtimeClient } from './realtime';
import { AudioCapture, type AudioProblem, type AudioSignalStats } from './audio-capture';
import { Visualizer } from './visualizer';
import { initFileUpload } from './file-upload';

let realtimeClient: RealtimeClient | null = null;
let audioCapture = new AudioCapture();
let visualizer: Visualizer;
let isRecording = false;
let micRecoveryInProgress = false;
let micTeardownPromise: Promise<void> = Promise.resolve();
let screenWakeLock: WakeLockSentinel | null = null;
let noSignalActive = false;
let lastProblemCode: AudioProblem['code'] | '' = '';
let lastProblemAt = 0;
const MIC_DEVICE_STORAGE_KEY = 'asr_selected_mic_device_id';
let selectedMicDeviceId = '';
let selectedMicDeviceLabel = '';
let knownMicDevices: MediaDeviceInfo[] = [];

function ensureMicEnvironment() {
  if (!navigator.mediaDevices?.getUserMedia) {
    throw new Error('Браузер не поддерживает доступ к микрофону (getUserMedia).');
  }

  if (window.isSecureContext) return;

  const host = window.location.hostname;
  const isLoopback = host === 'localhost' || host === '127.0.0.1' || host === '::1';
  if (isLoopback) return;

  throw new Error(
    'Микрофон в браузере работает только на HTTPS или localhost. Откройте страницу через https://<ваш-домен> или http://localhost:8081.'
  );
}

function getStoredMicDeviceId(): string {
  try {
    return localStorage.getItem(MIC_DEVICE_STORAGE_KEY) ?? '';
  } catch {
    return '';
  }
}

function setStoredMicDeviceId(deviceId: string) {
  try {
    if (deviceId) {
      localStorage.setItem(MIC_DEVICE_STORAGE_KEY, deviceId);
    } else {
      localStorage.removeItem(MIC_DEVICE_STORAGE_KEY);
    }
  } catch {
    // ignore
  }
}

function getCaptureDeviceId(): string {
  if (!selectedMicDeviceId || selectedMicDeviceId === 'default') return '';
  return selectedMicDeviceId;
}

function safeDeviceLabel(device: MediaDeviceInfo, idx: number): string {
  const label = device.label?.trim();
  if (label) return label;
  return device.deviceId === 'default' ? 'Микрофон по умолчанию' : `Микрофон ${idx + 1}`;
}

function renderMicDeviceList(devices: MediaDeviceInfo[]) {
  dom.micDeviceSelect.innerHTML = '';

  if (devices.length === 0) {
    const option = document.createElement('option');
    option.value = '';
    option.textContent = 'Нет доступных микрофонов';
    dom.micDeviceSelect.append(option);
    dom.micDeviceSelect.disabled = true;
    selectedMicDeviceId = '';
    selectedMicDeviceLabel = '';
    setStoredMicDeviceId('');
    setMicDeviceNote('Аудио-входы не найдены в браузере.');
    return;
  }

  dom.micDeviceSelect.disabled = false;
  const stored = getStoredMicDeviceId();
  const hasStored = stored && devices.some((d) => d.deviceId === stored);
  const currentValid = selectedMicDeviceId && devices.some((d) => d.deviceId === selectedMicDeviceId);
  const preferred = hasStored ? stored : currentValid ? selectedMicDeviceId : devices[0].deviceId;

  devices.forEach((device, idx) => {
    const option = document.createElement('option');
    option.value = device.deviceId;
    option.textContent = safeDeviceLabel(device, idx);
    dom.micDeviceSelect.append(option);
  });

  dom.micDeviceSelect.value = preferred;
  selectedMicDeviceId = preferred;
  setStoredMicDeviceId(selectedMicDeviceId);
  const selectedIdx = devices.findIndex((d) => d.deviceId === selectedMicDeviceId);
  const idx = selectedIdx >= 0 ? selectedIdx : 0;
  selectedMicDeviceLabel = safeDeviceLabel(devices[idx], idx);
  setMicDeviceNote(`Текущее устройство: ${selectedMicDeviceLabel}`);
}

async function refreshMicDevices() {
  if (!navigator.mediaDevices?.enumerateDevices) return;

  try {
    const devices = await navigator.mediaDevices.enumerateDevices();
    knownMicDevices = devices.filter((d) => d.kind === 'audioinput');
    renderMicDeviceList(knownMicDevices);
    rtLog('mic_devices_refreshed', {
      count: knownMicDevices.length,
      selected_device_id: selectedMicDeviceId,
      selected_device_label: selectedMicDeviceLabel,
    });
  } catch (err) {
    rtLog('mic_devices_refresh_failed', { error: errorMessage(err) });
    setMicDeviceNote('Не удалось получить список микрофонов.');
  }
}

function onAudioProblem(problem: AudioProblem) {
  const now = Date.now();
  if (problem.code === lastProblemCode && now - lastProblemAt < 10000) {
    return;
  }
  lastProblemCode = problem.code;
  lastProblemAt = now;

  rtLog('ui_audio_problem', {
    code: problem.code,
    message: problem.message,
    mode: problem.stats.mode,
    avg_rms: problem.stats.avg_rms,
    silent_for_ms: problem.stats.silent_for_ms,
  });

  if (!isRecording) return;

  noSignalActive = problem.code === 'no_input_signal';
  setStatus('error', problem.code === 'no_input_signal' ? 'Микрофон: нет сигнала' : 'Микрофон недоступен');
  if (problem.code === 'no_input_signal') {
    setMicDeviceNote('Сигнал не идет. Выберите другой источник в списке выше.');
  }
  setMicUI(true, problem.message);
  addResult(`[Диагностика] ${problem.message}`);
}

function onAudioStats(stats: AudioSignalStats) {
  if (!isRecording) return;

  const hasSignal = stats.avg_rms > 0.001 || stats.max_rms > 0.002;
  if (!hasSignal && stats.silent_for_ms >= 2500) {
    if (!noSignalActive) {
      noSignalActive = true;
      setStatus('error', 'Микрофон: нет сигнала');
      setMicUI(true, 'Нет входного сигнала. Проверьте микрофон и разрешения браузера.');
    }
    return;
  }

  if (noSignalActive && hasSignal) {
    noSignalActive = false;
    setStatus('connected');
    setMicUI(true, 'Слушаю...');
    if (selectedMicDeviceLabel) {
      setMicDeviceNote(`Текущее устройство: ${selectedMicDeviceLabel}`);
    }
    rtLog('ui_audio_signal_restored', { avg_rms: stats.avg_rms, max_rms: stats.max_rms });
  }
}

function attachAudioCaptureHandlers(capture: AudioCapture) {
  capture.onRecoveryNeeded = scheduleMicRecovery;
  capture.onAudioProblem = onAudioProblem;
  capture.onAudioStats = onAudioStats;
}

attachAudioCaptureHandlers(audioCapture);

function ensureRealtimeClient(): RealtimeClient {
  if (realtimeClient) return realtimeClient;

  realtimeClient = new RealtimeClient(REALTIME_WS_URL);

  realtimeClient.onConnected = () => {
    rtLog('ui_realtime_connected', {});
    setStatus('connected');
    if (isRecording && !noSignalActive) setMicUI(true, 'Слушаю...');
  };

  realtimeClient.onDisconnected = (intentional) => {
    rtLog('ui_realtime_disconnected', { intentional });
    if (intentional) {
      setStatus('disconnected');
      return;
    }
    setStatus('error', 'Переподключение...');
  };

  realtimeClient.onReconnectScheduled = (delayMs, attempt) => {
    rtLog('ui_realtime_reconnect_scheduled', { delay_ms: delayMs, attempt });
    if (!isRecording) return;
    setStatus('error', `Переподключение (${attempt})...`);
    setMicUI(true, `Потеряно соединение. Повтор через ${Math.ceil(delayMs / 1000)} с`);
  };

  realtimeClient.onTranscript = (text) => {
    const trimmed = text.trim();
    if (!trimmed) return;
    rtLog('ui_transcript_added', { text_len: trimmed.length });
    addResult(trimmed);
  };

  realtimeClient.onSpeechState = (active) => {
    rtLog('ui_speech_state', { active });
    if (!isRecording || noSignalActive) return;
    setMicUI(true, active ? 'Речь обнаружена...' : 'Слушаю...');
  };

  realtimeClient.onError = (err) => {
    rtLog('ui_realtime_error', { error: errorMessage(err) });
    console.warn('Realtime client error', err);
  };

  return realtimeClient;
}

async function acquireWakeLock() {
  if (!('wakeLock' in navigator)) return;
  try {
    if (screenWakeLock) return;
    screenWakeLock = await navigator.wakeLock.request('screen');
    screenWakeLock.addEventListener('release', () => {
      screenWakeLock = null;
    });
  } catch {
    // ignore
  }
}

async function releaseWakeLock() {
  if (!screenWakeLock) return;
  try {
    await screenWakeLock.release();
  } catch {
    // ignore
  } finally {
    screenWakeLock = null;
  }
}

function scheduleMicRecovery(reason: string) {
  if (!isRecording || micRecoveryInProgress) return;
  micRecoveryInProgress = true;
  setMicUI(true, 'Микрофон переподключается...');
  rtLog('mic_recovery_started', { reason });

  const realtime = ensureRealtimeClient();
  (async () => {
    try {
      await audioCapture.teardown();
      audioCapture = new AudioCapture();
      attachAudioCaptureHandlers(audioCapture);
      await realtime.connect();
      await audioCapture.setup(realtime, { deviceId: getCaptureDeviceId() });
      await refreshMicDevices();
      audioCapture.startFlushTimer();
      audioCapture.startHealthCheck(() => isRecording);
      if (isRecording && !noSignalActive) setMicUI(true, 'Слушаю...');
      micRecoveryInProgress = false;
      rtLog('mic_recovery_completed', { reason });
    } catch (err) {
      rtLog('mic_recovery_failed', { reason, error: errorMessage(err) });
      if (!isRecording) {
        micRecoveryInProgress = false;
        return;
      }
      setMicUI(true, 'Пробую восстановить микрофон...');
      setTimeout(() => {
        micRecoveryInProgress = false;
        if (isRecording) scheduleMicRecovery('retry_after_failure');
      }, 1500);
    }
  })();
}

async function startRecording() {
  if (isRecording) return;

  try {
    ensureMicEnvironment();
    rtLog('recording_start_requested', {});
    await micTeardownPromise;

    const realtime = ensureRealtimeClient();
    realtime.intentionalClose = false;
    await realtime.connect();

    isRecording = true;
    micRecoveryInProgress = false;
    noSignalActive = false;
    lastProblemCode = '';
    lastProblemAt = 0;

    audioCapture = new AudioCapture();
    attachAudioCaptureHandlers(audioCapture);
    await audioCapture.setup(realtime, { deviceId: getCaptureDeviceId() });
    await refreshMicDevices();
    audioCapture.startFlushTimer();
    audioCapture.startHealthCheck(() => isRecording);

    setMicUI(true, 'Слушаю...');
    await acquireWakeLock();

    rtLog('recording_started', {
      audio_sample_rate: audioCapture.audioContext?.sampleRate || 0,
      audio_state: audioCapture.audioContext?.state || 'none',
    });

    if (audioCapture.analyser && audioCapture.audioContext) {
      visualizer.start(audioCapture.analyser, audioCapture.audioContext);
    }
  } catch (err) {
    isRecording = false;
    micRecoveryInProgress = false;
    noSignalActive = false;
    await audioCapture.teardown();
    realtimeClient?.close();
    rtLog('recording_start_failed', { error: errorMessage(err) });
    setMicUI(false, 'Ошибка: ' + (err instanceof Error ? err.message : String(err)));
    await releaseWakeLock();
  }
}

function stopRecording(reason: string) {
  if (!isRecording) return;
  if (reason === 'unknown') {
    rtLog('recording_stop_ignored', { reason: 'unknown' });
    return;
  }

  rtLog('recording_stop_requested', { reason });
  isRecording = false;
  micRecoveryInProgress = false;
  noSignalActive = false;
  visualizer.stop();

  if (realtimeClient) {
    audioCapture.flush();
    realtimeClient.commitBuffer();
    realtimeClient.close();
  }

  micTeardownPromise = audioCapture.teardown();
  setMicUI(false);
  releaseWakeLock();
  rtLog('recording_stopped', { reason });
}

// Debug bundle on window
(window as any).getAsrRealtimeDebugBundle = () =>
  getDebugState({
    recording: isRecording,
    mic_recovery_in_progress: micRecoveryInProgress,
    no_signal_active: noSignalActive,
    selected_mic_device_id: selectedMicDeviceId,
    selected_mic_device_label: selectedMicDeviceLabel,
    known_mic_devices: knownMicDevices.map((d, idx) => ({
      device_id: d.deviceId,
      group_id: d.groupId,
      label: safeDeviceLabel(d, idx),
    })),
    status_text: dom.statusText.textContent || '',
    mic_status_text: dom.micStatus.textContent || '',
    ...audioCapture.getDebugInfo(),
    websocket: realtimeClient?.getDebugInfo() ?? null,
  });

export function init() {
  initTabs();
  initFileUpload();
  visualizer = new Visualizer(dom.canvas);
  selectedMicDeviceId = getStoredMicDeviceId();
  refreshMicDevices();

  dom.micDeviceSelect.addEventListener('change', () => {
    selectedMicDeviceId = dom.micDeviceSelect.value;
    setStoredMicDeviceId(selectedMicDeviceId);
    const selectedIdx = knownMicDevices.findIndex((d) => d.deviceId === selectedMicDeviceId);
    selectedMicDeviceLabel =
      selectedIdx >= 0 ? safeDeviceLabel(knownMicDevices[selectedIdx], selectedIdx) : '';
    setMicDeviceNote(
      selectedMicDeviceLabel ? `Текущее устройство: ${selectedMicDeviceLabel}` : 'Устройство не выбрано.'
    );
    rtLog('mic_device_selected', {
      selected_device_id: selectedMicDeviceId,
      selected_device_label: selectedMicDeviceLabel,
      recording: isRecording,
    });
    if (isRecording) {
      scheduleMicRecovery('device_changed');
    }
  });

  dom.refreshDevicesBtn.addEventListener('click', () => {
    refreshMicDevices();
  });

  navigator.mediaDevices?.addEventListener?.('devicechange', () => {
    refreshMicDevices();
  });

  dom.micBtn.addEventListener('click', () => {
    if (isRecording) {
      stopRecording('mic_button_click');
    } else {
      startRecording();
    }
  });

  document.addEventListener('visibilitychange', () => {
    if (!isRecording) return;
    if (document.visibilityState === 'visible') acquireWakeLock();
  });

  window.addEventListener('beforeunload', (event) => {
    if (!isRecording) return;
    event.preventDefault();
    event.returnValue = '';
  });
}
