// Audio utilities: WAV parsing, PCM conversion, chunking

export interface WavInfo {
  sampleRate: number;
  channels: number;
  bitsPerSample: number;
  dataOffset: number;
  dataSize: number;
}

export function parseWavHeader(buf: Buffer): WavInfo {
  if (buf.length < 44) throw new Error("WAV file too small");
  const riff = buf.toString("ascii", 0, 4);
  if (riff !== "RIFF") throw new Error(`Not a WAV file (got ${riff})`);
  const wave = buf.toString("ascii", 8, 12);
  if (wave !== "WAVE") throw new Error(`Not a WAVE file (got ${wave})`);

  let offset = 12;
  let fmt: WavInfo | null = null;

  while (offset + 8 <= buf.length) {
    const chunkId = buf.toString("ascii", offset, offset + 4);
    const chunkSize = buf.readUInt32LE(offset + 4);

    if (chunkId === "fmt ") {
      const audioFormat = buf.readUInt16LE(offset + 8);
      if (audioFormat !== 1) throw new Error(`Unsupported WAV format ${audioFormat} (need PCM=1)`);
      fmt = {
        channels: buf.readUInt16LE(offset + 10),
        sampleRate: buf.readUInt32LE(offset + 12),
        bitsPerSample: buf.readUInt16LE(offset + 22),
        dataOffset: 0,
        dataSize: 0,
      };
    } else if (chunkId === "data") {
      if (!fmt) throw new Error("WAV: data chunk before fmt chunk");
      fmt.dataOffset = offset + 8;
      fmt.dataSize = chunkSize;
      return fmt;
    }

    offset += 8 + chunkSize;
    if (chunkSize % 2 !== 0) offset++; // padding byte
  }

  throw new Error("WAV: no data chunk found");
}

/** Convert any PCM WAV data to float32 mono samples */
export function wavToFloat32Mono(buf: Buffer, info: WavInfo): Float32Array {
  const data = buf.subarray(info.dataOffset, info.dataOffset + info.dataSize);

  let monoSamples: Float32Array;

  if (info.bitsPerSample === 16) {
    const sampleCount = data.length / (2 * info.channels);
    monoSamples = new Float32Array(sampleCount);
    for (let i = 0; i < sampleCount; i++) {
      let sum = 0;
      for (let ch = 0; ch < info.channels; ch++) {
        sum += data.readInt16LE((i * info.channels + ch) * 2);
      }
      monoSamples[i] = sum / info.channels / 32768;
    }
  } else if (info.bitsPerSample === 32) {
    const sampleCount = data.length / (4 * info.channels);
    monoSamples = new Float32Array(sampleCount);
    for (let i = 0; i < sampleCount; i++) {
      let sum = 0;
      for (let ch = 0; ch < info.channels; ch++) {
        sum += data.readFloatLE((i * info.channels + ch) * 4);
      }
      monoSamples[i] = sum / info.channels;
    }
  } else {
    throw new Error(`Unsupported bits per sample: ${info.bitsPerSample}`);
  }

  return monoSamples;
}

/** Convert float32 samples to PCM16 LE bytes */
export function float32ToPcm16(samples: Float32Array): Buffer {
  const buf = Buffer.alloc(samples.length * 2);
  for (let i = 0; i < samples.length; i++) {
    const s = Math.max(-1, Math.min(1, samples[i]));
    buf.writeInt16LE(s < 0 ? Math.round(s * 32768) : Math.round(s * 32767), i * 2);
  }
  return buf;
}

/** Split a Float32Array into chunks of given size */
export function* chunkSamples(samples: Float32Array, chunkSize: number): Generator<Float32Array> {
  for (let i = 0; i < samples.length; i += chunkSize) {
    yield samples.subarray(i, Math.min(i + chunkSize, samples.length));
  }
}

/** Read raw PCM16 mono from stdin */
export async function readStdinPcm16(): Promise<Float32Array> {
  const chunks: Buffer[] = [];
  for await (const chunk of Bun.stdin.stream()) {
    chunks.push(Buffer.from(chunk));
  }
  const raw = Buffer.concat(chunks);
  const samples = new Float32Array(raw.length / 2);
  for (let i = 0; i < samples.length; i++) {
    samples[i] = raw.readInt16LE(i * 2) / 32768;
  }
  return samples;
}
