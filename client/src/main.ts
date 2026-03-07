#!/usr/bin/env bun

import { parseArgs } from "util";
import { RealtimeClient } from "./realtime-client";
import { parseWavHeader, wavToFloat32Mono, float32ToPcm16, chunkSamples } from "./audio";
import type { ServerEvent } from "./protocol";

const { values, positionals } = parseArgs({
  args: Bun.argv.slice(2),
  options: {
    url: { type: "string", default: "ws://localhost:8081/v1/realtime" },
    rate: { type: "string", default: "16000" },
    "chunk-ms": { type: "string", default: "40" },
    threshold: { type: "string", default: "0.35" },
    "silence-ms": { type: "string", default: "400" },
    mic: { type: "boolean", default: false },
    "mic-rate": { type: "string", default: "16000" },
    verbose: { type: "boolean", short: "v", default: false },
    help: { type: "boolean", short: "h", default: false },
  },
  allowPositionals: true,
  strict: true,
});

if (values.help) {
  console.log(`asr-client - Realtime ASR testing tool

Usage:
  asr-client [options] <file.wav>          Stream WAV file
  asr-client [options] --mic               Record from mic (needs arecord/sox)
  arecord -f S16_LE -r 16000 -c 1 -t raw | asr-client --rate 16000 -
  sox -d -r 16000 -c 1 -b 16 -e signed -t raw - | asr-client --rate 16000 -

Options:
  --url <url>         Server URL (default: ws://localhost:8081/v1/realtime)
  --rate <hz>         Sample rate for raw stdin input (default: 16000)
  --chunk-ms <ms>     Audio chunk size in ms (default: 40)
  --threshold <f>     VAD threshold (default: 0.35)
  --silence-ms <ms>   VAD silence duration (default: 400)
  --mic               Record via arecord (Linux)
  --mic-rate <hz>     Mic sample rate (default: 16000)
  -v, --verbose       Verbose logging
  -h, --help          Show help`);
  process.exit(0);
}

const url = values.url!;
const sampleRate = parseInt(values.rate!);
const chunkMs = parseInt(values["chunk-ms"]!);
const threshold = parseFloat(values.threshold!);
const silenceMs = parseInt(values["silence-ms"]!);
const verbose = values.verbose!;
const useMic = values.mic!;
const micRate = parseInt(values["mic-rate"]!);
const inputFile = positionals[0];

if (!inputFile && !useMic) {
  console.error("Error: provide a WAV file, '-' for stdin, or --mic");
  process.exit(1);
}

// --- Streaming logic ---

interface StreamResult {
  transcripts: string[];
  speechEvents: number;
  errors: string[];
  durationSec: number;
}

async function streamAudio(
  samples: Float32Array,
  inputRate: number,
): Promise<StreamResult> {
  const result: StreamResult = { transcripts: [], speechEvents: 0, errors: [], durationSec: 0 };
  const startTime = performance.now();

  const client = new RealtimeClient({
    url,
    verbose,
    sessionConfig: {
      input_audio_format: "pcm16",
      input_sample_rate: inputRate,
      input_audio_transcription: { model: "default" },
      turn_detection: {
        type: "server_vad",
        threshold,
        silence_duration_ms: silenceMs,
      },
    },
  });

  // Collect events
  client.on("conversation.item.input_audio_transcription.completed", (e) => {
    const text = (e as any).transcript as string;
    result.transcripts.push(text);
    const elapsed = ((performance.now() - startTime) / 1000).toFixed(1);
    console.log(`[${elapsed}s] ${text}`);
  });

  client.on("input_audio_buffer.speech_started", () => {
    result.speechEvents++;
    if (verbose) console.error("[speech started]");
  });

  client.on("input_audio_buffer.speech_stopped", () => {
    if (verbose) console.error("[speech stopped]");
  });

  client.on("error", (e) => {
    const msg = (e as any).error?.message || "unknown";
    result.errors.push(msg);
    console.error(`[error] ${msg}`);
  });

  if (verbose) {
    client.on("session.created", () => console.error("[session created]"));
    client.on("transcription_session.updated", () => console.error("[session updated]"));
    client.on("input_audio_buffer.committed", () => console.error("[committed]"));
  }

  await client.connect();

  // Stream in realtime-paced chunks
  const chunkSizeSamples = Math.round((inputRate * chunkMs) / 1000);
  const chunkDuration = chunkMs / 1000;

  let chunkIndex = 0;
  for (const chunk of chunkSamples(samples, chunkSizeSamples)) {
    const pcm16 = float32ToPcm16(chunk);
    const b64 = pcm16.toString("base64");
    client.appendAudio(b64);

    // Pace to realtime
    chunkIndex++;
    const expectedTime = chunkIndex * chunkDuration * 1000;
    const actualTime = performance.now() - startTime;
    const sleepMs = expectedTime - actualTime;
    if (sleepMs > 1) {
      await Bun.sleep(sleepMs);
    }
  }

  // Commit and wait for final results
  client.commit();
  await Bun.sleep(2000); // wait for server to finish processing

  result.durationSec = (performance.now() - startTime) / 1000;
  client.close();

  return result;
}

async function streamFromStdin(rate: number): Promise<StreamResult> {
  const result: StreamResult = { transcripts: [], speechEvents: 0, errors: [], durationSec: 0 };
  const startTime = performance.now();

  const client = new RealtimeClient({
    url,
    verbose,
    sessionConfig: {
      input_audio_format: "pcm16",
      input_sample_rate: rate,
      input_audio_transcription: { model: "default" },
      turn_detection: {
        type: "server_vad",
        threshold,
        silence_duration_ms: silenceMs,
      },
    },
  });

  client.on("conversation.item.input_audio_transcription.completed", (e) => {
    const text = (e as any).transcript as string;
    result.transcripts.push(text);
    const elapsed = ((performance.now() - startTime) / 1000).toFixed(1);
    console.log(`[${elapsed}s] ${text}`);
  });

  client.on("input_audio_buffer.speech_started", () => {
    result.speechEvents++;
    if (verbose) console.error("[speech started]");
  });

  client.on("error", (e) => {
    const msg = (e as any).error?.message || "unknown";
    result.errors.push(msg);
    console.error(`[error] ${msg}`);
  });

  await client.connect();
  console.error(`Streaming from stdin at ${rate} Hz (PCM16 mono LE)...`);

  const chunkBytes = Math.round(rate * (chunkMs / 1000)) * 2; // PCM16 = 2 bytes per sample
  let pending = Buffer.alloc(0);

  for await (const raw of Bun.stdin.stream()) {
    pending = Buffer.concat([pending, Buffer.from(raw)]);

    while (pending.length >= chunkBytes) {
      const chunk = pending.subarray(0, chunkBytes);
      pending = pending.subarray(chunkBytes);
      const b64 = chunk.toString("base64");
      client.appendAudio(b64);
    }
  }

  // Send remaining
  if (pending.length > 0) {
    client.appendAudio(pending.toString("base64"));
  }

  client.commit();
  await Bun.sleep(2000);

  result.durationSec = (performance.now() - startTime) / 1000;
  client.close();

  return result;
}

async function streamFromMic(): Promise<StreamResult> {
  console.error(`Recording from mic at ${micRate} Hz... (Ctrl+C to stop)`);

  // Use arecord on Linux
  const proc = Bun.spawn(["arecord", "-f", "S16_LE", "-r", String(micRate), "-c", "1", "-t", "raw", "-q"], {
    stdout: "pipe",
    stderr: "ignore",
  });

  const result: StreamResult = { transcripts: [], speechEvents: 0, errors: [], durationSec: 0 };
  const startTime = performance.now();

  const client = new RealtimeClient({
    url,
    verbose,
    sessionConfig: {
      input_audio_format: "pcm16",
      input_sample_rate: micRate,
      input_audio_transcription: { model: "default" },
      turn_detection: {
        type: "server_vad",
        threshold,
        silence_duration_ms: silenceMs,
      },
    },
  });

  client.on("conversation.item.input_audio_transcription.completed", (e) => {
    const text = (e as any).transcript as string;
    result.transcripts.push(text);
    const elapsed = ((performance.now() - startTime) / 1000).toFixed(1);
    console.log(`[${elapsed}s] ${text}`);
  });

  client.on("input_audio_buffer.speech_started", () => {
    result.speechEvents++;
    if (verbose) console.error("[speech started]");
  });

  client.on("input_audio_buffer.speech_stopped", () => {
    if (verbose) console.error("[speech stopped]");
  });

  client.on("error", (e) => {
    const msg = (e as any).error?.message || "unknown";
    result.errors.push(msg);
    console.error(`[error] ${msg}`);
  });

  await client.connect();

  // Handle Ctrl+C
  let stopped = false;
  process.on("SIGINT", () => {
    if (stopped) return;
    stopped = true;
    console.error("\nStopping...");
    proc.kill();
  });

  const chunkBytes = Math.round(micRate * (chunkMs / 1000)) * 2;
  let pending = Buffer.alloc(0);

  const reader = proc.stdout.getReader();
  while (!stopped) {
    const { done, value } = await reader.read();
    if (done) break;

    pending = Buffer.concat([pending, Buffer.from(value)]);

    while (pending.length >= chunkBytes) {
      const chunk = pending.subarray(0, chunkBytes);
      pending = pending.subarray(chunkBytes);
      client.appendAudio(chunk.toString("base64"));
    }
  }

  if (pending.length > 0) {
    client.appendAudio(pending.toString("base64"));
  }

  client.commit();
  await Bun.sleep(2000);

  result.durationSec = (performance.now() - startTime) / 1000;
  client.close();

  return result;
}

// --- Main ---

async function main() {
  let result: StreamResult;

  if (useMic) {
    result = await streamFromMic();
  } else if (inputFile === "-") {
    result = await streamFromStdin(sampleRate);
  } else {
    // WAV file
    const file = Bun.file(inputFile!);
    if (!(await file.exists())) {
      console.error(`File not found: ${inputFile}`);
      process.exit(1);
    }

    const buf = Buffer.from(await file.arrayBuffer());
    const info = parseWavHeader(buf);
    const samples = wavToFloat32Mono(buf, info);

    console.error(
      `Streaming ${inputFile}: ${info.sampleRate} Hz, ${info.channels}ch, ${info.bitsPerSample}bit, ${(samples.length / info.sampleRate).toFixed(1)}s`,
    );

    result = await streamAudio(samples, info.sampleRate);
  }

  // Summary
  console.error(`\n--- Summary ---`);
  console.error(`Duration: ${result.durationSec.toFixed(1)}s`);
  console.error(`Transcripts: ${result.transcripts.length}`);
  console.error(`Speech events: ${result.speechEvents}`);
  if (result.errors.length > 0) {
    console.error(`Errors: ${result.errors.length}`);
    for (const e of result.errors) console.error(`  - ${e}`);
  }

  process.exit(result.errors.length > 0 ? 1 : 0);
}

main().catch((err) => {
  console.error("Fatal:", err);
  process.exit(1);
});
