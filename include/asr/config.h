#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>

namespace asr {

class ConfigError : public std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct Config {
  // Server
  std::string host    = "0.0.0.0";
  uint16_t    port    = 8081;
  size_t      threads = std::thread::hardware_concurrency();

  // Model paths
  std::string model_dir = "models/sherpa-onnx-nemo-transducer-punct-giga-am-v3-russian-2025-12-16";
  std::string vad_model = "models/silero_vad.onnx";

  // ASR
  std::string provider    = "cpu";
  int         num_threads = 4;
  int         sample_rate = 16000;
  int         feature_dim = 64;

  // VAD
  float vad_threshold    = 0.5f;
  float vad_min_silence  = 0.5f;
  float vad_min_speech   = 0.25f;
  float vad_max_speech   = 20.0f;
  int   vad_window_size  = 512;
  int   vad_context_size = 64;

  // Concurrency
  int    recognizer_pool_size    = 0;  // 0 = auto = threads
  size_t max_concurrent_requests = 0;  // 0 = auto = threads * 2

  // Audio
  float  silence_threshold    = 0.008f;
  float  min_audio_sec        = 0.5f;
  float  max_audio_sec        = 30.0f;
  size_t max_upload_bytes     = static_cast<size_t>(100) * 1024 * 1024;
  size_t max_ws_message_bytes = static_cast<size_t>(4) * 1024 * 1024;  // 4 MB per WS frame

  // Parse all from environment variables
  static Config from_env();
  void          validate();
};

}  // namespace asr
