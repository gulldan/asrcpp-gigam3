// AudioWorkletProcessor for real-time microphone capture.
// Forwards raw samples at native AudioContext.sampleRate to the main thread.
// Resampling is handled server-side via libsamplerate.

class ASRAudioProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.running = true;

    this.port.onmessage = (e) => {
      if (e.data === 'stop') {
        this.running = false;
      }
    };
  }

  process(inputs) {
    if (!this.running) {
      return false; // stop processing
    }

    const input = inputs[0];
    if (!input || !input[0] || input[0].length === 0) {
      return true;
    }

    const copy = new Float32Array(input[0]);
    this.port.postMessage(copy.buffer, [copy.buffer]);

    return true;
  }
}

registerProcessor('asr-audio-processor', ASRAudioProcessor);
