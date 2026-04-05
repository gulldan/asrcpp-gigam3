// File upload handling for offline recognition endpoints.

import {
  addResult,
  setStatus,
  dom,
  formatSize,
  setFileButtonsLoading,
  resetFileButtons,
  setFileSelectionState,
} from './ui';
import { uploadFileToServer, uploadFileToWhisperApi } from './transcription-api';

let selectedFile: File | null = null;

function handleFile(file: File) {
  selectedFile = file;
  dom.fileName.textContent = file.name;
  dom.fileSize.textContent = formatSize(file.size);
  dom.fileInfo.style.display = 'flex';
  setFileSelectionState(file.name, formatSize(file.size));
}

async function processServer() {
  if (!selectedFile) return;
  setFileButtonsLoading(dom.processBtn, 'Сервер...');
  setStatus('connected', 'Отправка на сервер...');

  try {
    const { text, duration } = await uploadFileToServer(selectedFile);
    addResult(text, duration, 'Сервер');
    setStatus('connected', 'Результат готов');
    resetFileButtons();
  } catch (err) {
    const msg = err instanceof Error ? err.message : 'Не удалось обработать файл';
    alert('Ошибка: ' + msg);
    setStatus('error', 'Ошибка обработки');
    resetFileButtons();
  }
}

async function processWhisperAPI() {
  if (!selectedFile) return;
  setFileButtonsLoading(dom.processWhisperBtn, 'Whisper...');
  setStatus('connected', 'Отправка в Whisper API...');

  try {
    const { text, duration } = await uploadFileToWhisperApi(selectedFile);
    addResult(text, duration, 'Whisper API');
    setStatus('connected', 'Whisper API OK');
  } catch (err) {
    const msg = err instanceof Error ? err.message : 'Ошибка Whisper API';
    alert('Ошибка: ' + msg);
    setStatus('error', 'Whisper API: ошибка');
  } finally {
    resetFileButtons();
  }
}

export function initFileUpload() {
  const { uploadZone, fileInput } = dom;
  uploadZone.addEventListener('click', () => fileInput.click());
  uploadZone.addEventListener('dragover', (e) => {
    e.preventDefault();
    uploadZone.classList.add('dragover');
  });
  uploadZone.addEventListener('dragleave', () => uploadZone.classList.remove('dragover'));
  uploadZone.addEventListener('drop', (e) => {
    e.preventDefault();
    uploadZone.classList.remove('dragover');
    if (e.dataTransfer?.files[0]) handleFile(e.dataTransfer.files[0]);
  });
  fileInput.addEventListener('change', () => {
    if (fileInput.files?.[0]) handleFile(fileInput.files[0]);
  });
  dom.processBtn.addEventListener('click', processServer);
  dom.processWhisperBtn.addEventListener('click', processWhisperAPI);
}
