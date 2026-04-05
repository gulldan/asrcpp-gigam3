import { afterEach, describe, expect, mock, test } from 'bun:test';

import { uploadFileToServer, uploadFileToWhisperApi } from './transcription-api';

const originalFetch = globalThis.fetch;

afterEach(() => {
  globalThis.fetch = originalFetch;
});

describe('uploadFileToServer', () => {
  test('posts multipart file to /recognize and parses JSON response', async () => {
    const fetchMock = mock(async (input: RequestInfo | URL, init?: RequestInit) => {
      expect(String(input)).toBe('/recognize');
      expect(init?.method).toBe('POST');
      expect(init?.body).toBeInstanceOf(FormData);

      const body = init?.body as FormData;
      const file = body.get('file');
      expect(file).toBeInstanceOf(File);
      expect((file as File).name).toBe('sample.wav');

      return new Response(JSON.stringify({ text: 'привет мир', duration: 1.25 }), {
        status: 200,
        headers: { 'content-type': 'application/json' },
      });
    });
    globalThis.fetch = fetchMock as typeof fetch;

    const result = await uploadFileToServer(new File(['wav'], 'sample.wav', { type: 'audio/wav' }));
    expect(result).toEqual({ text: 'привет мир', duration: 1.25 });
    expect(fetchMock).toHaveBeenCalledTimes(1);
  });

  test('surfaces JSON error details from /recognize', async () => {
    globalThis.fetch = mock(async () => new Response(JSON.stringify({ detail: 'File too large' }), {
      status: 413,
      headers: { 'content-type': 'application/json' },
    })) as typeof fetch;

    await expect(uploadFileToServer(new File(['wav'], 'sample.wav', { type: 'audio/wav' }))).rejects.toThrow(
      'File too large',
    );
  });
});

describe('uploadFileToWhisperApi', () => {
  test('posts multipart file to whisper endpoint and parses JSON response', async () => {
    const fetchMock = mock(async (input: RequestInfo | URL, init?: RequestInit) => {
      expect(String(input)).toBe('/v1/audio/transcriptions');
      expect(init?.method).toBe('POST');
      expect(init?.body).toBeInstanceOf(FormData);

      const body = init?.body as FormData;
      expect(body.get('model')).toBe('whisper-1');
      expect(body.get('response_format')).toBe('json');

      return new Response(JSON.stringify({ text: 'готово', duration: 2.5 }), {
        status: 200,
        headers: { 'content-type': 'application/json' },
      });
    });
    globalThis.fetch = fetchMock as typeof fetch;

    const result = await uploadFileToWhisperApi(new File(['wav'], 'sample.wav', { type: 'audio/wav' }));
    expect(result).toEqual({ text: 'готово', duration: 2.5 });
    expect(fetchMock).toHaveBeenCalledTimes(1);
  });

  test('parses plain-text whisper response', async () => {
    globalThis.fetch = mock(async () => new Response('plain text transcript', {
      status: 200,
      headers: { 'content-type': 'text/plain' },
    })) as typeof fetch;

    const result = await uploadFileToWhisperApi(new File(['wav'], 'sample.wav', { type: 'audio/wav' }));
    expect(result).toEqual({ text: 'plain text transcript', duration: null });
  });
});
