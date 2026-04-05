#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace asr {

enum class OpusPacketMode {
  Auto,
  Raw,
  Rtp,
};
template <typename T>
class span;
}  // namespace asr

namespace asr {

struct AudioData {
  std::vector<float> samples;  // float32 [-1, 1]
  float              duration_sec = 0.0f;
};

struct AudioStreamStats {
  size_t samples      = 0;
  float  duration_sec = 0.0f;
};

struct OpusDecodeStats {
  uint64_t decoded_packets      = 0;
  uint64_t decoded_samples      = 0;
  uint64_t rtp_packets          = 0;
  uint64_t lost_packets         = 0;
  uint64_t plc_packets          = 0;
  uint64_t fec_packets          = 0;
  uint64_t duplicate_packets    = 0;
  uint64_t out_of_order_packets = 0;
};

using AudioChunkCallback = std::function<void(span<const float> chunk)>;

// Decode WAV file from memory buffer, resample to target_rate
// Throws AudioError on invalid input
AudioData decode_wav(span<const uint8_t> data, int target_rate = 16000);

// Decode supported Whisper API audio formats from memory buffer
// (wav and optionally opus if built with libopusfile), resample to target_rate.
// The file_name is used to detect container format.
AudioData decode_audio(span<const uint8_t> data, std::string_view file_name, int target_rate = 16000);

// Stream decode supported audio formats in target sample rate and emit fixed-size chunks.
// The callback is invoked sequentially for each chunk (and a final tail chunk if any).
AudioStreamStats decode_audio_streamed(span<const uint8_t> data, std::string_view file_name, int target_rate,
                                       size_t chunk_samples, const AudioChunkCallback& on_chunk);

// Decode base64 payload into raw bytes.
// Throws AudioError on invalid input.
std::vector<uint8_t> base64_decode(std::string_view input);

// Decode base64 payload into a reusable output buffer.
// Throws AudioError on invalid input.
void base64_decode_into(std::string_view input, std::vector<uint8_t>& out);

// Convert little-endian PCM16 bytes to float32 [-1, 1].
// Throws AudioError on invalid input size.
std::vector<float> pcm16_to_float32(span<const uint8_t> pcm16_data);

// Convert little-endian PCM16 bytes to float32 [-1, 1] into reusable output buffer.
// Throws AudioError on invalid input size.
void pcm16_to_float32_into(span<const uint8_t> pcm16_data, std::vector<float>& out);

// Decode realtime binary audio payload according to selected format.
// Supported formats: pcm16, opus, opus_raw, opus_rtp.
std::vector<float> decode_realtime_audio_bytes(span<const uint8_t> audio_bytes,
                                               std::string_view format = "pcm16", int target_rate = 16000);

// Decode OpenAI Realtime-style base64 audio payload.
// Supported formats: pcm16, opus, opus_raw, opus_rtp.
std::vector<float> decode_realtime_audio(std::string_view base64_audio, std::string_view format = "pcm16",
                                         int target_rate = 16000);

// Stateful Opus decoder for realtime streams (WebRTC-style packet flow).
// Input packet may be either raw Opus payload or a full RTP packet containing Opus payload.
// Maintains decoder state across packets, performs packet-loss concealment (PLC),
// and uses in-band FEC when possible for single-packet loss bursts.
class RealtimeOpusDecoder {
 public:
  explicit RealtimeOpusDecoder(int sample_rate = 48000, size_t max_plc_packets = 8, bool enable_fec = true,
                               OpusPacketMode packet_mode = OpusPacketMode::Auto);
  ~RealtimeOpusDecoder();
  RealtimeOpusDecoder(const RealtimeOpusDecoder&)            = delete;
  RealtimeOpusDecoder& operator=(const RealtimeOpusDecoder&) = delete;
  RealtimeOpusDecoder(RealtimeOpusDecoder&&)                 = delete;
  RealtimeOpusDecoder& operator=(RealtimeOpusDecoder&&)      = delete;

  // Decode one packet and return a view into internal buffer.
  // The buffer remains valid until the next decode_packet/reset call.
  span<const float> decode_packet(span<const uint8_t> packet);

  void reset();

  [[nodiscard]] int                    sample_rate() const noexcept;
  [[nodiscard]] const OpusDecodeStats& stats() const noexcept;

 private:
  void*              decoder_            = nullptr;  // OpusDecoder*
  int                sample_rate_        = 0;
  int                max_frame_samples_  = 0;
  int                last_frame_samples_ = 0;
  size_t             max_plc_packets_    = 0;
  bool               enable_fec_         = true;
  OpusPacketMode     packet_mode_        = OpusPacketMode::Auto;
  bool               have_last_seq_      = false;
  uint16_t           last_seq_           = 0;
  OpusDecodeStats    stats_{};
  std::vector<float> output_;
};

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
