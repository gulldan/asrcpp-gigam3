#include "asr/recognizer.h"

#include <sherpa-onnx/c-api/c-api.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>

#include "asr/config.h"
#include "asr/span.h"

namespace asr {

Recognizer::Recognizer(const Config& cfg)
    : encoder_path_(cfg.model_dir + "/encoder.int8.onnx"),
      decoder_path_(cfg.model_dir + "/decoder.onnx"),
      joiner_path_(cfg.model_dir + "/joiner.onnx"),
      tokens_path_(cfg.model_dir + "/tokens.txt"),
      provider_(cfg.provider) {
  const int pool_size        = cfg.recognizer_pool_size > 0 ? cfg.recognizer_pool_size : 1;
  const int threads_per_slot = std::max(1, cfg.num_threads / pool_size);

  slots_.resize(static_cast<size_t>(pool_size));

  for (int i = 0; i < pool_size; ++i) {
    SherpaOnnxOfflineRecognizerConfig c{};

    // Transducer model config
    c.model_config.transducer.encoder = encoder_path_.c_str();
    c.model_config.transducer.decoder = decoder_path_.c_str();
    c.model_config.transducer.joiner  = joiner_path_.c_str();

    // General model config
    c.model_config.tokens      = tokens_path_.c_str();
    c.model_config.num_threads = threads_per_slot;
    c.model_config.provider    = provider_.c_str();
    c.model_config.model_type  = "nemo_transducer";
    c.model_config.debug       = 0;

    // Feature config
    c.feat_config.sample_rate = cfg.sample_rate;
    c.feat_config.feature_dim = cfg.feature_dim;

    // Decoding config
    c.decoding_method = "greedy_search";

    const auto* handle = SherpaOnnxCreateOfflineRecognizer(&c);
    if (handle == nullptr) {
      // Destroy any handles already created
      for (int j = 0; j < i; ++j) {
        SherpaOnnxDestroyOfflineRecognizer(slots_[static_cast<size_t>(j)].handle);
      }
      throw std::runtime_error("Failed to create sherpa-onnx offline recognizer slot " + std::to_string(i) +
                               " (provider=" + cfg.provider + ", model_dir=" + cfg.model_dir +
                               "). Check that model files exist and provider is available.");
    }

    slots_[static_cast<size_t>(i)].handle = handle;
    slots_[static_cast<size_t>(i)].in_use = false;
  }

  spdlog::info("Recognizer pool initialized: pool_size={}, threads_per_slot={}, provider={}", pool_size,
               threads_per_slot, cfg.provider);
}

Recognizer::~Recognizer() {
  for (auto& slot : slots_) {
    if (slot.handle != nullptr) {
      SherpaOnnxDestroyOfflineRecognizer(slot.handle);
    }
  }
}

std::string Recognizer::recognize(span<const float> audio, int sample_rate) {
  if (audio.empty()) {
    return {};
  }

  // Acquire a free slot from the pool
  const SherpaOnnxOfflineRecognizer* handle   = nullptr;
  size_t                             slot_idx = 0;
  {
    std::unique_lock lock(pool_mutex_);
    pool_cv_.wait(lock, [this, &slot_idx]() {
      for (size_t i = 0; i < slots_.size(); ++i) {
        // cppcheck-suppress useStlAlgorithm  ; need to capture index, not iterator
        if (!slots_[i].in_use) {
          slot_idx = i;
          return true;
        }
      }
      return false;
    });
    slots_[slot_idx].in_use = true;
    handle                  = slots_[slot_idx].handle;
  }

  // Decode (no mutex held — parallel inference is possible)
  const SherpaOnnxOfflineStream* stream = SherpaOnnxCreateOfflineStream(handle);
  if (stream == nullptr) {
    spdlog::error("Failed to create offline stream");
    const std::scoped_lock lock(pool_mutex_);
    slots_[slot_idx].in_use = false;
    pool_cv_.notify_one();
    return {};
  }

  SherpaOnnxAcceptWaveformOffline(stream, sample_rate, audio.data(), static_cast<int32_t>(audio.size()));
  SherpaOnnxDecodeOfflineStream(handle, stream);

  const SherpaOnnxOfflineRecognizerResult* result = SherpaOnnxGetOfflineStreamResult(stream);

  std::string text;
  if (result != nullptr && result->text != nullptr) {
    text = result->text;
    // Trim in-place — avoids substr allocation
    auto end = text.find_last_not_of(" \t\n\r");
    if (end != std::string::npos) {
      text.erase(end + 1);
    } else {
      text.clear();
    }
    auto start = text.find_first_not_of(" \t\n\r");
    if (start != std::string::npos && start > 0) {
      text.erase(0, start);
    }
  }

  if (result != nullptr) {
    SherpaOnnxDestroyOfflineRecognizerResult(result);
  }
  SherpaOnnxDestroyOfflineStream(stream);

  // Release slot
  {
    const std::scoped_lock lock(pool_mutex_);
    slots_[slot_idx].in_use = false;
  }
  pool_cv_.notify_one();

  return text;
}

}  // namespace asr
