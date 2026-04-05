// DOM manipulation: tabs, mirrored status labels, and transcript timeline

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;

const modeCopy = {
  mic: {
    controlTitle: 'Микрофон',
    controlDescription: 'Быстрый запуск и выбор источника.',
    stageLabel: 'Потоковый режим',
    stageTitle: 'Голос сразу появляется в расшифровке.',
    stageDescription: 'Сигнал, подключение и готовые фрагменты собраны на одном экране.',
    activeMode: 'Микрофон',
    engineLabel: 'Потоковый сервер',
    captureState: 'Готово',
    sessionHint: 'Нажмите на кнопку записи.',
    transcriptPlaceholder: 'Здесь появится последний распознанный фрагмент.',
  },
  file: {
    controlTitle: 'Файл',
    controlDescription: 'Выберите запись и отправьте её на обработку.',
    stageLabel: 'Обработка файла',
    stageTitle: 'Запись превращается в текст за один проход.',
    stageDescription: 'Сначала выбираете файл, затем запускаете распознавание.',
    activeMode: 'Файл',
    engineLabel: 'Сервер / Whisper API',
    captureState: 'Файл не выбран',
    sessionHint: 'Добавьте аудиофайл.',
    transcriptPlaceholder: 'Здесь появится последний готовый фрагмент.',
  },
} as const;

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
  heroTranscriptText: $('heroTranscriptText'),
  resultsCount: $('resultsCount'),
  clearResultsBtn: $<HTMLButtonElement>('clearResultsBtn'),
  controlTitle: $('controlTitle'),
  controlDescription: $('controlDescription'),
};

let activeMode: 'mic' | 'file' = 'mic';

function setRoleText(role: string, text: string) {
  document.querySelectorAll<HTMLElement>(`[data-role="${role}"]`).forEach((node) => {
    node.textContent = text;
  });
}

function createEmptyState() {
  const empty = document.createElement('div');
  empty.className = 'empty-state';
  empty.innerHTML = `
    <svg viewBox="0 0 24 24" aria-hidden="true">
      <path d="M20 2H4c-1.1 0-2 .9-2 2v18l4-4h14c1.1 0 2-.9 2-2V4c0-1.1-.9-2-2-2zm0 14H6l-2 2V4h16v12z"></path>
    </svg>
    <p class="empty-title">Результатов пока нет</p>
    <p class="empty-text">Готовые фрагменты будут появляться здесь.</p>
  `;
  return empty;
}

function syncResultsCount() {
  const count = dom.results.querySelectorAll('.result-item').length;
  dom.resultsCount.textContent = String(count);
  dom.clearResultsBtn.disabled = count === 0;
}

function syncMode(mode: 'mic' | 'file') {
  activeMode = mode;
  document.body.dataset.mode = mode;

  const copy = modeCopy[mode];
  dom.controlTitle.textContent = copy.controlTitle;
  dom.controlDescription.textContent = copy.controlDescription;
  setRoleText('stage-label', copy.stageLabel);
  setRoleText('stage-title', copy.stageTitle);
  setRoleText('stage-description', copy.stageDescription);
  setRoleText('active-mode', copy.activeMode);
  setRoleText('engine-label', copy.engineLabel);
  setRoleText('capture-state', copy.captureState);
  setRoleText('session-hint', copy.sessionHint);

  const hasResults = dom.results.querySelector('.result-item');
  if (!hasResults) {
    dom.heroTranscriptText.textContent = copy.transcriptPlaceholder;
  }

  if (mode === 'mic') {
    if (dom.micBtn.classList.contains('recording')) {
      setRoleText('capture-state', 'Идёт захват');
      setRoleText('session-hint', 'Говорите обычным темпом.');
    }
    return;
  }

  if (dom.fileInfo.style.display !== 'none' && dom.fileName.textContent) {
    setRoleText('capture-state', 'Файл выбран');
    setRoleText('session-hint', `${dom.fileName.textContent}. Выберите способ обработки.`);
  }
}

export function getActiveMode() {
  return activeMode;
}

export function initTabs() {
  const tabs = document.querySelectorAll<HTMLButtonElement>('.tab');
  const contents = document.querySelectorAll<HTMLElement>('.tab-content');

  const selectTab = (mode: 'mic' | 'file') => {
    tabs.forEach((tab) => {
      const active = tab.dataset.tab === mode;
      tab.classList.toggle('active', active);
      tab.setAttribute('aria-selected', active ? 'true' : 'false');
    });
    contents.forEach((content) => {
      content.classList.toggle('active', content.id === `tab-${mode}`);
    });
    syncMode(mode);
  };

  tabs.forEach((tab) => {
    tab.addEventListener('click', () => selectTab(tab.dataset.tab as 'mic' | 'file'));
  });

  selectTab('mic');
}

export function setStatus(state: 'connected' | 'disconnected' | 'error', text?: string) {
  const dots = document.querySelectorAll<HTMLElement>('[data-role="status-dot"]');
  const resolvedText = state === 'connected'
    ? text ?? 'Подключено'
    : state === 'error'
      ? text ?? 'Ошибка'
      : text ?? 'Отключено';

  document.body.dataset.connection = state;
  dots.forEach((dot) => dot.classList.remove('connected', 'error'));

  if (state === 'connected') {
    dots.forEach((dot) => dot.classList.add('connected'));
  } else if (state === 'error') {
    dots.forEach((dot) => dot.classList.add('error'));
  }

  setRoleText('status-text', resolvedText);
}

export function addResult(
  text: string,
  duration: number | null = null,
  source = activeMode === 'mic' ? 'Микрофон' : 'Файл',
  tone: 'default' | 'diagnostic' = 'default',
) {
  const empty = dom.results.querySelector('.empty-state');
  if (empty) empty.remove();

  const item = document.createElement('article');
  item.className = `result-item${tone === 'diagnostic' ? ' diagnostic' : ''}`;

  const chrome = document.createElement('div');
  chrome.className = 'result-chrome';

  const sourceSpan = document.createElement('span');
  sourceSpan.className = 'result-source';
  sourceSpan.textContent = source;

  const timeSpan = document.createElement('span');
  timeSpan.className = 'result-time';
  timeSpan.textContent = new Date().toLocaleTimeString();

  chrome.append(sourceSpan, timeSpan);

  const textDiv = document.createElement('div');
  textDiv.className = 'result-text';
  textDiv.textContent = text;

  item.append(chrome, textDiv);

  if (Number.isFinite(duration)) {
    const metaDiv = document.createElement('div');
    metaDiv.className = 'result-meta';

    const durationSpan = document.createElement('span');
    durationSpan.textContent = `Длительность ${duration!.toFixed(1)} сек`;
    metaDiv.append(durationSpan);
    item.append(metaDiv);
  }

  dom.results.appendChild(item);
  dom.heroTranscriptText.textContent = text;
  syncResultsCount();

  const nearBottom = dom.results.scrollHeight - dom.results.scrollTop - dom.results.clientHeight < 80;
  if (nearBottom) {
    requestAnimationFrame(() => {
      dom.results.scrollTo({ top: dom.results.scrollHeight, behavior: 'smooth' });
    });
  }
}

export function clearResults() {
  dom.results.innerHTML = '';
  dom.results.append(createEmptyState());
  dom.heroTranscriptText.textContent = modeCopy[activeMode].transcriptPlaceholder;
  syncResultsCount();
}

export function setMicUI(recording: boolean, statusMsg?: string) {
  const { micBtn, micStatus } = dom;
  const statusText = recording
    ? statusMsg ?? 'Идёт распознавание'
    : statusMsg ?? 'Готов к распознаванию';

  document.body.dataset.recording = recording ? 'true' : 'false';
  micBtn.classList.toggle('recording', recording);
  micStatus.classList.toggle('recording', recording);
  micStatus.textContent = statusText;

  if (activeMode === 'mic') {
    setRoleText('capture-state', recording ? 'Идёт захват' : 'Готово');
    setRoleText(
      'session-hint',
      recording
        ? 'Говорите обычным темпом.'
        : 'Нажмите на кнопку записи.',
    );
  }
}

export function setMicDeviceNote(text: string) {
  dom.micDeviceNote.textContent = text;
}

export function setSelectedDeviceLabel(text: string) {
  setRoleText('device-label', text || 'Автовыбор');
}

export function setFileSelectionState(name: string, size: string) {
  setRoleText('capture-state', `Файл выбран`);
  if (activeMode === 'file') {
    setRoleText('session-hint', `${name} • ${size}. Выберите способ обработки.`);
  }
  dom.heroTranscriptText.textContent = `Готов к обработке: ${name}`;
}

export function formatSize(bytes: number): string {
  if (bytes < 1024) return bytes + ' Б';
  if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' КБ';
  return (bytes / (1024 * 1024)).toFixed(1) + ' МБ';
}

export function setFileButtonsLoading(activeBtn: HTMLButtonElement, text: string) {
  const isServer = activeBtn === dom.processBtn;
  dom.processBtn.disabled = true;
  dom.processWhisperBtn.disabled = true;
  activeBtn.innerHTML = `<span class="loading"><span class="spinner"></span><span>${text}</span></span>`;
  setRoleText('capture-state', `Идёт обработка`);
  setRoleText('engine-label', isServer ? 'Сервер' : 'Whisper API');
  if (activeMode === 'file') {
    setRoleText('session-hint', `Файл обрабатывается через ${isServer ? 'сервер' : 'Whisper API'}.`);
  }
}

export function resetFileButtons() {
  [dom.processBtn, dom.processWhisperBtn].forEach((button) => {
    button.disabled = false;
    button.textContent = button.dataset.label ?? button.textContent ?? '';
  });

  if (activeMode === 'file') {
    const hasSelectedFile = dom.fileInfo.style.display !== 'none' && !!dom.fileName.textContent;
    setRoleText('capture-state', hasSelectedFile ? 'Файл выбран' : 'Файл не выбран');
    setRoleText('engine-label', modeCopy.file.engineLabel);
    setRoleText(
      'session-hint',
      hasSelectedFile
        ? `${dom.fileName.textContent}. Можно запустить обработку ещё раз.`
        : modeCopy.file.sessionHint,
    );
  }
}

syncResultsCount();
