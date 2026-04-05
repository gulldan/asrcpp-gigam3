export interface FileTranscriptionResult {
  text: string;
  duration: number | null;
}

async function readErrorMessage(response: Response, fallback: string): Promise<string> {
  const contentType = response.headers.get('content-type') || '';

  if (contentType.includes('application/json')) {
    try {
      const payload = await response.json();
      return payload?.error?.message || payload?.detail || fallback;
    } catch {
      return fallback;
    }
  }

  try {
    const text = (await response.text()).trim();
    return text || fallback;
  } catch {
    return fallback;
  }
}

export async function uploadFileToServer(file: File): Promise<FileTranscriptionResult> {
  const form = new FormData();
  form.append('file', file, file.name);

  const response = await fetch('/recognize', { method: 'POST', body: form });
  if (!response.ok) {
    throw new Error(await readErrorMessage(response, `HTTP ${response.status}`));
  }

  const payload = await response.json();
  const text = (payload?.text || '').trim();
  if (!text) {
    throw new Error('Пустой ответ транскрипции');
  }

  return {
    text,
    duration: Number.isFinite(payload?.duration) ? payload.duration : null,
  };
}

export async function uploadFileToWhisperApi(file: File): Promise<FileTranscriptionResult> {
  const form = new FormData();
  form.append('file', file, file.name);
  form.append('model', 'whisper-1');
  form.append('response_format', 'json');

  const response = await fetch('/v1/audio/transcriptions', { method: 'POST', body: form });
  if (!response.ok) {
    throw new Error(await readErrorMessage(response, `HTTP ${response.status}`));
  }

  const contentType = response.headers.get('content-type') || '';
  if (contentType.includes('application/json')) {
    const payload = await response.json();
    const text = (payload?.text || '').trim();
    if (!text) {
      throw new Error('Пустой ответ транскрипции');
    }

    return {
      text,
      duration: Number.isFinite(payload?.duration) ? payload.duration : null,
    };
  }

  const text = (await response.text()).trim();
  if (!text) {
    throw new Error('Пустой ответ транскрипции');
  }

  return { text, duration: null };
}
