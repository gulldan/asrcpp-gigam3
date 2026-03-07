// DOM manipulation: tabs, results list, connection status

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;

export const dom = {
  micBtn: $<HTMLButtonElement>('micBtn'),
  micStatus: $('micStatus'),
  micDeviceSelect: $<HTMLSelectElement>('micDeviceSelect'),
  refreshDevicesBtn: $<HTMLButtonElement>('refreshDevicesBtn'),
  micDeviceNote: $('micDeviceNote'),
  canvas: $<HTMLCanvasElement>('visualizer'),
  uploadZone: $('uploadZone'),
  fileInput: $<HTMLInputElement>('fileInput'),
  fileInfo: $('fileInfo'),
  fileName: $('fileName'),
  fileSize: $('fileSize'),
  processBtn: $<HTMLButtonElement>('processBtn'),
  processWhisperBtn: $<HTMLButtonElement>('processWhisperBtn'),
  results: $('results'),
  statusDot: $('statusDot'),
  statusText: $('statusText'),
};

export function initTabs() {
  const tabs = document.querySelectorAll<HTMLButtonElement>('.tab');
  const contents = document.querySelectorAll<HTMLElement>('.tab-content');
  tabs.forEach(tab => {
    tab.addEventListener('click', () => {
      tabs.forEach(t => t.classList.remove('active'));
      contents.forEach(c => c.classList.remove('active'));
      tab.classList.add('active');
      document.getElementById(`tab-${tab.dataset.tab}`)!.classList.add('active');
    });
  });
}

export function setStatus(state: 'connected' | 'disconnected' | 'error', text?: string) {
  const { statusDot, statusText } = dom;
  statusDot.classList.remove('connected', 'error');
  if (state === 'connected') {
    statusDot.classList.add('connected');
    statusText.textContent = text ?? 'Подключено';
  } else if (state === 'error') {
    statusDot.classList.add('error');
    statusText.textContent = text ?? 'Ошибка';
  } else {
    statusText.textContent = text ?? 'Отключено';
  }
}

export function addResult(text: string, duration: number | null = null) {
  const { results } = dom;
  const empty = results.querySelector('.empty-state');
  if (empty) empty.remove();

  const item = document.createElement('div');
  item.className = 'result-item';

  const textDiv = document.createElement('div');
  textDiv.className = 'result-text';
  textDiv.textContent = text;

  const metaDiv = document.createElement('div');
  metaDiv.className = 'result-meta';

  const timeSpan = document.createElement('span');
  timeSpan.textContent = new Date().toLocaleTimeString();

  const durationSpan = document.createElement('span');
  durationSpan.textContent = Number.isFinite(duration)
    ? duration!.toFixed(1) + ' сек'
    : 'длительность n/a';

  metaDiv.append(timeSpan, durationSpan);
  item.append(textDiv, metaDiv);
  results.insertBefore(item, results.firstChild);
}

export function setMicUI(recording: boolean, statusMsg?: string) {
  const { micBtn, micStatus } = dom;
  if (recording) {
    micBtn.classList.add('recording');
    micStatus.classList.add('recording');
    micStatus.textContent = statusMsg ?? 'Слушаю...';
  } else {
    micBtn.classList.remove('recording');
    micStatus.classList.remove('recording');
    micStatus.textContent = statusMsg ?? 'Нажмите, чтобы слушать';
  }
}

export function setMicDeviceNote(text: string) {
  dom.micDeviceNote.textContent = text;
}

export function formatSize(bytes: number): string {
  if (bytes < 1024) return bytes + ' Б';
  if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' КБ';
  return (bytes / (1024 * 1024)).toFixed(1) + ' МБ';
}

export function setFileButtonsLoading(activeBtn: HTMLButtonElement, text: string) {
  dom.processBtn.disabled = true;
  dom.processWhisperBtn.disabled = true;
  activeBtn.innerHTML = `<span class="loading"><span class="spinner"></span> ${text}</span>`;
}

export function resetFileButtons() {
  dom.processBtn.disabled = false;
  dom.processWhisperBtn.disabled = false;
  dom.processBtn.textContent = 'Распознать (WS)';
  dom.processWhisperBtn.textContent = 'Whisper API';
}
