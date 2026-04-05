#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace asr {
struct Config;
template <typename T>
class span;
}  // namespace asr

// Forward declare sherpa-onnx C types
struct SherpaOnnxOfflineRecognizer;

namespace asr {

class RecognizerBusyError : public std::runtime_error {
  using std::runtime_error::runtime_error;
};

class Recognizer {
 public:
  explicit Recognizer(const Config& cfg);
  ~Recognizer();

  // Non-copyable, non-movable (pool internals are non-trivial)
  Recognizer(const Recognizer&)            = delete;
  Recognizer& operator=(const Recognizer&) = delete;
  Recognizer(Recognizer&&)                 = delete;
  Recognizer& operator=(Recognizer&&)      = delete;

  // Thread-safe: acquires a free pool slot, decodes, releases it
  std::string        recognize(span<const float> audio, int sample_rate = 16000);
  [[nodiscard]] bool ready() const noexcept;

 private:
  struct Slot {
    const SherpaOnnxOfflineRecognizer* handle = nullptr;
    bool                               in_use = false;
  };

  std::vector<Slot>       slots_;
  std::mutex              pool_mutex_;
  std::condition_variable pool_cv_;

  // Keep path strings alive for c_str() during construction
  std::string encoder_path_;
  std::string decoder_path_;
  std::string joiner_path_;
  std::string tokens_path_;
  std::string provider_;
  size_t      wait_timeout_ms_ = 30000;
};

}  // namespace asr
