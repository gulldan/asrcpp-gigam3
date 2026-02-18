#pragma once

#include <onnxruntime_cxx_api.h>

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace asr {
template <typename T>
class span;
}  // namespace asr

namespace asr {

struct VadConfig {
  std::string model_path;
  float       threshold            = 0.5f;
  float       min_silence_duration = 0.5f;
  float       min_speech_duration  = 0.25f;
  float       max_speech_duration  = 20.0f;
  int         sample_rate          = 16000;
  int         window_size          = 512;
  int         context_size         = 64;
};

struct SpeechSegment {
  std::vector<float> samples;
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
  [[nodiscard]] bool is_speech() const;
  void               flush();
  void               reset();

 private:
  float infer(span<const float> samples);
  void  finalize_segment();

  VadConfig config_;

  // ONNX Runtime
  Ort::Env        env_;
  Ort::Session    session_;
  Ort::MemoryInfo memory_info_;
  Ort::RunOptions run_options_{nullptr};  // pre-constructed, reused per infer()

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
  bool                      in_speech_            = false;
  int64_t                   silence_samples_      = 0;
  int64_t                   speech_start_samples_ = 0;
  std::vector<float>        speech_buf_;
  std::deque<SpeechSegment> segments_;
};

}  // namespace asr
