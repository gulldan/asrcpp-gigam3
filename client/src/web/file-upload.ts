// File upload handling: WS streaming and Whisper API modes

import { WS_URL, WS_CONNECT_TIMEOUT_MS, FILE_CHUNK_SIZE, FILE_SEND_DELAY_MS } from './types';
import { addResult, setStatus, dom, formatSize, setFileButtonsLoading, resetFileButtons } from './ui';

const textDecoder = new TextDecoder('utf-8');

let ws: WebSocket | null = null;
let selectedFile: File | null = null;
let fileProcessing: {
  resolve: () => void;
  reject: (err: Error) => void;
  timeoutId: ReturnType<typeof setTimeout>;
} | null = null;

function cleanupFileProcessing() {
  if (!fileProcessing) return;
  clearTimeout(fileProcessing.timeoutId);
  fileProcessing = null;
}

async function parseWSData(payload: unknown): Promise<any> {
  if (typeof payload === 'string') return JSON.parse(payload);
  if (payload instanceof ArrayBuffer) return JSON.parse(textDecoder.decode(payload));
  if (payload instanceof Blob) return JSON.parse(textDecoder.decode(await payload.arrayBuffer()));
  return null;
}

function connectWS(): Promise<void> {
  if (ws?.readyState === WebSocket.OPEN) return Promise.resolve();

  return new Promise((resolve, reject) => {
    const wsInstance = new WebSocket(WS_URL);
    ws = wsInstance;
    ws.binaryType = 'arraybuffer';
    let settled = false;

    const timeout = setTimeout(() => {
      if (wsInstance.readyState === WebSocket.CONNECTING) wsInstance.close();
      if (!settled) {
        settled = true;
        setStatus('error');
        reject(new Error('Таймаут подключения WebSocket'));
      }
    }, WS_CONNECT_TIMEOUT_MS);

    wsInstance.onopen = () => {
      clearTimeout(timeout);
      setStatus('connected');
      if (!settled) { settled = true; resolve(); }
    };

    wsInstance.onmessage = async (e) => {
      try {
        const data = await parseWSData(e.data);
        if (!data) return;
        if (data.type === 'done') {
          if (fileProcessing) {
            const { resolve: res } = fileProcessing;
            cleanupFileProcessing();
            res();
          }
          return;
        }
        if (data.type === 'final' && data.text) {
          addResult(data.text, data.duration);
        }
      } catch (err) {
        console.error('WS parse error', err);
      }
    };

    wsInstance.onerror = () => {
      setStatus('error');
      if (!settled) {
        clearTimeout(timeout);
        settled = true;
        reject(new Error('Ошибка WebSocket'));
      }
    };

    wsInstance.onclose = () => {
      if (ws === wsInstance) ws = null;
      setStatus('disconnected');
      if (!settled) {
        clearTimeout(timeout);
        settled = true;
        reject(new Error('WebSocket закрыт до подключения'));
      }
      if (fileProcessing) {
        const { reject: rej } = fileProcessing;
        cleanupFileProcessing();
        rej(new Error('WebSocket закрыт'));
      }
    };
  });
}

function handleFile(file: File) {
  selectedFile = file;
  dom.fileName.textContent = file.name;
  dom.fileSize.textContent = formatSize(file.size);
  dom.fileInfo.style.display = 'flex';
}

async function processWS() {
  if (!selectedFile) return;
  setFileButtonsLoading(dom.processBtn, 'WS...');
  let audioCtx: AudioContext | null = null;

  try {
    await connectWS();
    const arrayBuffer = await selectedFile.arrayBuffer();
    audioCtx = new AudioContext();
    const audioBuffer = await audioCtx.decodeAudioData(arrayBuffer);

    ws!.send(JSON.stringify({ sample_rate: audioCtx.sampleRate }));

    const timeoutMs = Math.max(15000, Math.ceil(audioBuffer.duration * 1000 * 2));
    const completionPromise = new Promise<void>((resolve, reject) => {
      const timeoutId = setTimeout(() => {
        cleanupFileProcessing();
        reject(new Error('Таймаут ожидания завершения'));
      }, timeoutMs);
      fileProcessing = { resolve, reject, timeoutId };
    });

    const samples = audioBuffer.getChannelData(0);
    for (let i = 0; i < samples.length; i += FILE_CHUNK_SIZE) {
      const chunk = samples.slice(i, i + FILE_CHUNK_SIZE);
      ws!.send(new Float32Array(chunk).buffer);
      if (FILE_SEND_DELAY_MS > 0) {
        await new Promise(r => setTimeout(r, FILE_SEND_DELAY_MS));
      }
    }

    ws!.send('RECOGNIZE');
    await completionPromise;

    if (audioCtx.state !== 'closed') await audioCtx.close();
    ws!.close();
    resetFileButtons();
  } catch (err) {
    const msg = err instanceof Error ? err.message : 'Не удалось обработать файл';
    alert('Ошибка: ' + msg);
    resetFileButtons();
    cleanupFileProcessing();
    if (audioCtx && audioCtx.state !== 'closed') await audioCtx.close();
    if (ws?.readyState === WebSocket.OPEN) ws.close();
  }
}

async function processWhisperAPI() {
  if (!selectedFile) return;
  setFileButtonsLoading(dom.processWhisperBtn, 'Whisper API...');

  try {
    const form = new FormData();
    form.append('file', selectedFile, selectedFile.name);
    form.append('model', 'whisper-1');
    form.append('response_format', 'json');

    const response = await fetch('/v1/audio/transcriptions', { method: 'POST', body: form });
    const contentType = response.headers.get('content-type') || '';

    if (!response.ok) {
      let errMsg = `HTTP ${response.status}`;
      if (contentType.includes('application/json')) {
        try {
          const p = await response.json();
          errMsg = p?.error?.message || p?.detail || errMsg;
        } catch { /* ignore */ }
      } else {
        const text = (await response.text()).trim();
        if (text) errMsg = text;
      }
      throw new Error(errMsg);
    }

    let transcript = '';
    let duration: number | null = null;
    if (contentType.includes('application/json')) {
      const payload = await response.json();
      transcript = (payload?.text || '').trim();
      if (Number.isFinite(payload?.duration)) duration = payload.duration;
    } else {
      transcript = (await response.text()).trim();
    }

    if (!transcript) throw new Error('Пустой ответ транскрипции');

    addResult(transcript, duration);
    setStatus('connected', 'Whisper API OK');
  } catch (err) {
    const msg = err instanceof Error ? err.message : 'Ошибка Whisper API';
    alert('Ошибка: ' + msg);
    setStatus('error');
  } finally {
    resetFileButtons();
  }
}

export function initFileUpload() {
  const { uploadZone, fileInput } = dom;
  uploadZone.addEventListener('click', () => fileInput.click());
  uploadZone.addEventListener('dragover', (e) => { e.preventDefault(); uploadZone.classList.add('dragover'); });
  uploadZone.addEventListener('dragleave', () => uploadZone.classList.remove('dragover'));
  uploadZone.addEventListener('drop', (e) => {
    e.preventDefault();
    uploadZone.classList.remove('dragover');
    if (e.dataTransfer?.files[0]) handleFile(e.dataTransfer.files[0]);
  });
  fileInput.addEventListener('change', () => { if (fileInput.files?.[0]) handleFile(fileInput.files[0]); });
  dom.processBtn.addEventListener('click', processWS);
  dom.processWhisperBtn.addEventListener('click', processWhisperAPI);
}
