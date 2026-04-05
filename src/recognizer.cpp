#include "asr/recognizer.h"

#include <sherpa-onnx/c-api/c-api.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <ratio>
#include <stdexcept>

#include "asr/config.h"
#include "asr/metrics.h"
#include "asr/span.h"

namespace asr {

Recognizer::Recognizer(const Config& cfg)
    : encoder_path_(cfg.model_dir + "/encoder.int8.onnx"),
      decoder_path_(cfg.model_dir + "/decoder.onnx"),
      joiner_path_(cfg.model_dir + "/joiner.onnx"),
      tokens_path_(cfg.model_dir + "/tokens.txt"),
      provider_(cfg.provider),
      wait_timeout_ms_(cfg.recognizer_wait_timeout_ms) {
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

  struct SlotLease {
    Recognizer* owner    = nullptr;
    size_t      slot_idx = 0;

    ~SlotLease() {
      if (owner == nullptr) {
        return;
      }
      {
        const std::scoped_lock lock(owner->pool_mutex_);
        owner->slots_[slot_idx].in_use = false;
      }
      owner->pool_cv_.notify_one();
    }
  };

  const SherpaOnnxOfflineRecognizer* handle       = nullptr;
  size_t                             slot_idx     = 0;
  const auto                         wait_started = std::chrono::steady_clock::now();
  {
    std::unique_lock lock(pool_mutex_);
    const bool       acquired =
        pool_cv_.wait_for(lock, std::chrono::milliseconds(wait_timeout_ms_), [this, &slot_idx]() {
          const auto it =
              std::find_if(slots_.begin(), slots_.end(), [](const Slot& slot) { return !slot.in_use; });
          if (it == slots_.end()) {
            return false;
          }
          slot_idx = static_cast<size_t>(std::distance(slots_.begin(), it));
          return true;
        });
    const auto wait_sec =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - wait_started).count();
    ASRMetrics::instance().observe_recognizer_wait(wait_sec, !acquired);
    if (!acquired) {
      throw RecognizerBusyError("Recognizer pool is saturated");
    }
    slots_[slot_idx].in_use = true;
    handle                  = slots_[slot_idx].handle;
  }
  SlotLease slot_lease{this, slot_idx};

  using StreamHandle =
      std::unique_ptr<const SherpaOnnxOfflineStream, decltype(&SherpaOnnxDestroyOfflineStream)>;
  using ResultHandle = std::unique_ptr<const SherpaOnnxOfflineRecognizerResult,
                                       decltype(&SherpaOnnxDestroyOfflineRecognizerResult)>;

  StreamHandle stream(SherpaOnnxCreateOfflineStream(handle), &SherpaOnnxDestroyOfflineStream);
  if (stream == nullptr) {
    spdlog::error("Failed to create offline stream");
    return {};
  }

  SherpaOnnxAcceptWaveformOffline(stream.get(), sample_rate, audio.data(),
                                  static_cast<int32_t>(audio.size()));
  SherpaOnnxDecodeOfflineStream(handle, stream.get());

  ResultHandle result(SherpaOnnxGetOfflineStreamResult(stream.get()),
                      &SherpaOnnxDestroyOfflineRecognizerResult);

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

  return text;
}

bool Recognizer::ready() const noexcept {
  return std::all_of(slots_.begin(), slots_.end(), [](const Slot& slot) { return slot.handle != nullptr; });
}

}  // namespace asr
