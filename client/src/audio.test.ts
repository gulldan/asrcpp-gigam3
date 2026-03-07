import { describe, test, expect } from "bun:test";
import { parseWavHeader, wavToFloat32Mono, float32ToPcm16, chunkSamples } from "./audio";

function makeWav(sampleRate: number, samples: Int16Array): Buffer {
  const dataSize = samples.length * 2;
  const buf = Buffer.alloc(44 + dataSize);

  buf.write("RIFF", 0);
  buf.writeUInt32LE(36 + dataSize, 4);
  buf.write("WAVE", 8);
  buf.write("fmt ", 12);
  buf.writeUInt32LE(16, 16); // fmt chunk size
  buf.writeUInt16LE(1, 20); // PCM
  buf.writeUInt16LE(1, 22); // mono
  buf.writeUInt32LE(sampleRate, 24);
  buf.writeUInt32LE(sampleRate * 2, 28); // byte rate
  buf.writeUInt16LE(2, 32); // block align
  buf.writeUInt16LE(16, 34); // bits per sample
  buf.write("data", 36);
  buf.writeUInt32LE(dataSize, 40);

  for (let i = 0; i < samples.length; i++) {
    buf.writeInt16LE(samples[i], 44 + i * 2);
  }

  return buf;
}

describe("parseWavHeader", () => {
  test("parses valid WAV", () => {
    const samples = new Int16Array([0, 16384, -16384, 32767]);
    const wav = makeWav(16000, samples);
    const info = parseWavHeader(wav);

    expect(info.sampleRate).toBe(16000);
    expect(info.channels).toBe(1);
    expect(info.bitsPerSample).toBe(16);
    expect(info.dataOffset).toBe(44);
    expect(info.dataSize).toBe(8);
  });

  test("rejects non-WAV", () => {
    expect(() => parseWavHeader(Buffer.from("not a wav file at all!"))).toThrow();
  });

  test("rejects too small", () => {
    expect(() => parseWavHeader(Buffer.alloc(10))).toThrow();
  });
});

describe("wavToFloat32Mono", () => {
  test("converts PCM16 to float32", () => {
    const samples = new Int16Array([0, 32767, -32768]);
    const wav = makeWav(16000, samples);
    const info = parseWavHeader(wav);
    const f32 = wavToFloat32Mono(wav, info);

    expect(f32.length).toBe(3);
    expect(f32[0]).toBeCloseTo(0, 5);
    expect(f32[1]).toBeCloseTo(32767 / 32768, 4);
    expect(f32[2]).toBeCloseTo(-1, 5);
  });
});

describe("float32ToPcm16", () => {
  test("roundtrips through PCM16", () => {
    const original = new Float32Array([0, 0.5, -0.5, 1.0, -1.0]);
    const pcm16 = float32ToPcm16(original);
    expect(pcm16.length).toBe(10); // 5 samples * 2 bytes

    // Read back
    for (let i = 0; i < original.length; i++) {
      const val = pcm16.readInt16LE(i * 2) / 32768;
      expect(val).toBeCloseTo(original[i], 2);
    }
  });

  test("clamps out-of-range values", () => {
    const extreme = new Float32Array([2.0, -2.0]);
    const pcm16 = float32ToPcm16(extreme);
    expect(pcm16.readInt16LE(0)).toBe(32767);
    expect(pcm16.readInt16LE(2)).toBe(-32768);
  });
});

describe("chunkSamples", () => {
  test("splits evenly", () => {
    const samples = new Float32Array([1, 2, 3, 4, 5, 6]);
    const chunks = [...chunkSamples(samples, 3)];
    expect(chunks.length).toBe(2);
    expect([...chunks[0]]).toEqual([1, 2, 3]);
    expect([...chunks[1]]).toEqual([4, 5, 6]);
  });

  test("handles remainder", () => {
    const samples = new Float32Array([1, 2, 3, 4, 5]);
    const chunks = [...chunkSamples(samples, 3)];
    expect(chunks.length).toBe(2);
    expect([...chunks[0]]).toEqual([1, 2, 3]);
    expect([...chunks[1]]).toEqual([4, 5]);
  });

  test("single chunk", () => {
    const samples = new Float32Array([1, 2]);
    const chunks = [...chunkSamples(samples, 10)];
    expect(chunks.length).toBe(1);
  });

  test("empty input", () => {
    const chunks = [...chunkSamples(new Float32Array(0), 10)];
    expect(chunks.length).toBe(0);
  });
});
