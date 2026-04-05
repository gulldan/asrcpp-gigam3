#pragma once

#include <onnxruntime_cxx_api.h>

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace asr {
template <typename T>
class span;
}  // namespace asr

namespace asr {

struct SharedVadRuntime;

struct VadConfig {
  std::string model_path;
  float       threshold            = 0.5f;
  float       min_silence_duration = 0.5f;
  float       min_speech_duration  = 0.25f;
  float       max_speech_duration  = 20.0f;
  int         prefix_padding_ms    = 0;
  int         sample_rate          = 16000;
  int         window_size          = 512;
  int         context_size         = 64;
};

struct SpeechSegment {
  std::vector<float> samples;
  int64_t            start_sample = 0;
  int64_t            end_sample   = 0;
};

struct SpeechTransition {
  enum Kind { Started, Stopped } kind = Started;
  int64_t sample                      = 0;
};

class VoiceActivityDetector {
 public:
  explicit VoiceActivityDetector(const VadConfig& config);

  // Feed exactly window_size samples
  void accept_waveform(span<const float> samples);

  // Segment queue
  [[nodiscard]] bool                 empty() const;
  [[nodiscard]] const SpeechSegment& front() const;
  void                               pop();

  // State
  [[nodiscard]] bool                    is_speech() const;
  void                                  flush();
  void                                  reset();
  [[nodiscard]] bool                    has_transition() const;
  [[nodiscard]] const SpeechTransition& front_transition() const;
  void                                  pop_transition();

 private:
  float infer(span<const float> samples);
  void  finalize_segment();
  void  append_pre_roll(span<const float> samples);

  VadConfig config_;

  // ONNX Runtime model/session state shared across detector instances.
  std::shared_ptr<SharedVadRuntime> runtime_;
  Ort::RunOptions                   run_options_{nullptr};  // pre-constructed, reused per infer()

  // Pre-allocated tensors (zero runtime allocations)
  static constexpr int          kStateSize = 2 * 1 * 128;  // shape (2, 1, 128)
  std::array<float, kStateSize> state_{};
  std::vector<float>            input_buf_;  // context_size + window_size
  std::vector<float>            context_;    // last context_size samples
  int64_t                       sr_tensor_ = 16000;

  // ONNX input/output names (must persist for session lifetime)
  static constexpr const char* kInputNames[]  = {"input", "state", "sr"};
  static constexpr const char* kOutputNames[] = {"output", "stateN"};

  // State machine
  bool                         in_speech_              = false;
  int64_t                      silence_samples_        = 0;
  int64_t                      speech_start_samples_   = 0;
  int64_t                      total_samples_seen_     = 0;
  int64_t                      current_start_sample_   = 0;
  int64_t                      current_end_sample_     = 0;
  int64_t                      prefix_padding_samples_ = 0;
  std::vector<float>           speech_buf_;
  std::vector<float>           pre_roll_;
  std::deque<SpeechSegment>    segments_;
  std::deque<SpeechTransition> transitions_;
};

}  // namespace asr
