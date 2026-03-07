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

    const channels = inputs[0];
    if (!channels || channels.length === 0 || !channels[0] || channels[0].length === 0) {
      return true;
    }

    const frames = channels[0].length;
    let copy;

    if (channels.length === 1) {
      copy = new Float32Array(channels[0]);
    } else {
      // Downmix multichannel input to mono so we don't lose voice on non-primary channels.
      copy = new Float32Array(frames);
      for (let ch = 0; ch < channels.length; ch++) {
        const channel = channels[ch];
        if (!channel || channel.length !== frames) continue;
        for (let i = 0; i < frames; i++) {
          copy[i] += channel[i];
        }
      }
      const norm = 1 / channels.length;
      for (let i = 0; i < frames; i++) {
        copy[i] *= norm;
      }
    }

    this.port.postMessage(copy.buffer, [copy.buffer]);

    return true;
  }
}

registerProcessor('asr-audio-processor', ASRAudioProcessor);
