#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
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

// Decode supported Whisper API audio formats from memory buffer
// (flac, mp3, mp4, mpeg, mpga, m4a, ogg, wav, webm), resample to target_rate.
// The file_name is used to detect container format.
AudioData decode_audio(span<const uint8_t> data, std::string_view file_name, int target_rate = 16000);

// Decode base64 payload into raw bytes.
// Throws AudioError on invalid input.
std::vector<uint8_t> base64_decode(std::string_view input);

// Convert little-endian PCM16 bytes to float32 [-1, 1].
// Throws AudioError on invalid input size.
std::vector<float> pcm16_to_float32(span<const uint8_t> pcm16_data);

// Decode OpenAI Realtime-style base64 audio payload.
// Supported formats: pcm16. g711_* placeholders return AudioError.
std::vector<float> decode_realtime_audio(std::string_view base64_audio,
                                         std::string_view format = "pcm16");

// Check whether a file extension is supported by Whisper-compatible APIs.
// Accepts values with or without a leading dot.
bool is_supported_whisper_audio_extension(std::string_view extension);

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

  // Hard reset internal state without generating tail samples.
  void reset();

 private:
  void*              state_ = nullptr;  // SRC_STATE*, opaque to avoid leaking samplerate.h
  double             ratio_;
  std::vector<float> output_buf_;
};

class AudioError : public std::runtime_error {
  using std::runtime_error::runtime_error;
};

}  // namespace asr
