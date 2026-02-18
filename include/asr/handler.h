#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include "asr/vad.h"

namespace asr {
class Recognizer;
struct Config;
template <typename T>
class span;
}  // namespace asr

namespace asr {

class ASRSession {
 public:
  ASRSession(Recognizer& recognizer, const VadConfig& vad_config, const Config& config);

  struct OutMessage {
    enum Type { Interim, Final, Done } type = Interim;
    std::string json;
  };

  // Process binary audio chunk (float32 samples).
  // Returns a view into an internal buffer — valid until the next call.
  span<const OutMessage> on_audio(span<const float> samples);

  // Handle RECOGNIZE command — finalize session.
  // Returns a view into an internal buffer — valid until the next call.
  span<const OutMessage> on_recognize();

  // Handle RESET command
  void on_reset();

  // Handle connection close — clean up session metrics
  void on_close();

 private:
  using SteadyClock = std::chrono::steady_clock;
  using TimePoint   = SteadyClock::time_point;

  // Zero-alloc message buffer: elements persist across calls, reusing string capacity.
  // out_size_ tracks logical size; elements beyond it retain allocations for reuse.
  std::vector<OutMessage> out_messages_;
  size_t                  out_size_ = 0;

  void begin_messages() {
    out_size_ = 0;
  }
  OutMessage&                          next_message();
  [[nodiscard]] span<const OutMessage> current_messages() const;

  // Append messages from VAD segments to out_messages_
  void process_vad_segments();

  // Pad remaining pending samples and flush VAD
  void flush_pending();

  // Record session-level metrics, append done message, and reset
  void finalize_session();

  void reset_session();

  // Write JSON directly into next_message() — zero-alloc when capacity is reused
  void write_interim(float duration, float rms, bool is_speech);
  void write_final(const std::string& text, float duration);
  void write_done();

  Recognizer&           recognizer_;
  VoiceActivityDetector vad_;
  const Config&         config_;

  // Sub-window accumulator
  std::vector<float> pending_;

  // Session state
  TimePoint start_ts_;
  TimePoint first_result_ts_;
  bool      has_first_result_       = false;
  int       segments_               = 0;
  int       silence_segments_       = 0;
  double    decode_sec_             = 0.0;
  double    preprocess_sec_         = 0.0;
  size_t    audio_samples_          = 0;
  size_t    total_samples_received_ = 0;
  bool      session_active_         = false;
  bool      max_duration_exceeded_  = false;
  int       chunks_                 = 0;
  size_t    bytes_                  = 0;
};

}  // namespace asr
