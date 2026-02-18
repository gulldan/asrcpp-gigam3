#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace asr {
template <typename T>
class span;
}  // namespace asr

namespace asr {

struct AudioData {
  std::vector<float> samples;  // float32 [-1, 1]
  float              duration_sec = 0.0f;
};

// Decode WAV file from memory buffer, resample to target_rate
// Throws AudioError on invalid input
AudioData decode_wav(span<const uint8_t> data, int target_rate = 16000);

// Compute RMS of audio segment
float compute_rms(span<const float> samples);

// Streaming resampler for real-time WebSocket audio.
// Uses libsamplerate (sinc interpolation) for high-quality conversion.
class StreamResampler {
 public:
  StreamResampler(int input_rate, int output_rate);
  ~StreamResampler();
  StreamResampler(const StreamResampler&)            = delete;
  StreamResampler& operator=(const StreamResampler&) = delete;
  StreamResampler(StreamResampler&&)                 = delete;
  StreamResampler& operator=(StreamResampler&&)      = delete;

  // Resample input samples. Returns view of internal buffer (valid until next call).
  span<const float> process(span<const float> input);

  // Flush remaining samples from internal filter state (call once at end of stream).
  // Resets internal state for potential reuse after flush.
  span<const float> flush();

 private:
  void*              state_ = nullptr;  // SRC_STATE*, opaque to avoid leaking samplerate.h
  double             ratio_;
  std::vector<float> output_buf_;
};

class AudioError : public std::runtime_error {
  using std::runtime_error::runtime_error;
};

}  // namespace asr
