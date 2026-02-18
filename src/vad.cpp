#include "asr/vad.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "asr/span.h"

namespace asr {

VoiceActivityDetector::VoiceActivityDetector(const VadConfig& config)
    : config_(config),
      env_(ORT_LOGGING_LEVEL_WARNING, "vad"),
      session_(env_, config.model_path.c_str(), Ort::SessionOptions{}),
      memory_info_(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)) {
  if (config_.window_size <= 0) {
    throw std::invalid_argument("VAD window_size must be positive");
  }
  if (config_.context_size < 0 || config_.context_size >= config_.window_size) {
    throw std::invalid_argument("VAD context_size must be in [0, window_size)");
  }
  if (config_.sample_rate <= 0) {
    throw std::invalid_argument("VAD sample_rate must be positive");
  }
  if (config_.threshold <= 0.0f || config_.threshold >= 1.0f) {
    throw std::invalid_argument("VAD threshold must be in (0, 1)");
  }

  // Initialize buffers
  input_buf_.resize(static_cast<size_t>(config_.context_size) + static_cast<size_t>(config_.window_size),
                    0.0f);
  context_.resize(static_cast<size_t>(config_.context_size), 0.0f);
  sr_tensor_ = static_cast<int64_t>(config_.sample_rate);
  state_.fill(0.0f);
  speech_buf_.reserve(
      static_cast<size_t>(config_.max_speech_duration * static_cast<float>(config_.sample_rate)));

  spdlog::info("VAD initialized: threshold={}, window={}, context={}", config_.threshold, config_.window_size,
               config_.context_size);
}

float VoiceActivityDetector::infer(span<const float> samples) {
  if (static_cast<int>(samples.size()) != config_.window_size) {
    throw std::invalid_argument("infer: expected " + std::to_string(config_.window_size) + " samples, got " +
                                std::to_string(samples.size()));
  }

  // Build input: [context | samples]
  std::memcpy(input_buf_.data(), context_.data(), static_cast<size_t>(config_.context_size) * sizeof(float));
  std::memcpy(input_buf_.data() + config_.context_size, samples.data(),
              static_cast<size_t>(config_.window_size) * sizeof(float));

  // Create tensors from pre-allocated buffers
  std::array<int64_t, 2> input_shape = {1, static_cast<int64_t>(config_.context_size + config_.window_size)};
  std::array<int64_t, 3> state_shape = {2, 1, 128};
  std::array<int64_t, 1> sr_shape    = {1};

  auto input_tensor = Ort::Value::CreateTensor<float>(memory_info_, input_buf_.data(), input_buf_.size(),
                                                      input_shape.data(), 2);
  auto state_tensor =
      Ort::Value::CreateTensor<float>(memory_info_, state_.data(), state_.size(), state_shape.data(), 3);
  auto sr_tensor = Ort::Value::CreateTensor<int64_t>(memory_info_, &sr_tensor_, 1, sr_shape.data(), 1);

  std::array<Ort::Value, 3> inputs;
  inputs[0] = std::move(input_tensor);
  inputs[1] = std::move(state_tensor);
  inputs[2] = std::move(sr_tensor);

  // Run inference
  auto outputs = session_.Run(run_options_, kInputNames, inputs.data(), inputs.size(), kOutputNames, 2);

  // Extract probability
  const float prob = outputs[0].GetTensorData<float>()[0];

  // Update state from output
  const auto* new_state = outputs[1].GetTensorData<float>();
  std::memcpy(state_.data(), new_state, kStateSize * sizeof(float));

  // Update context with last context_size samples from input
  std::memcpy(context_.data(), samples.data() + (config_.window_size - config_.context_size),
              static_cast<size_t>(config_.context_size) * sizeof(float));

  return prob;
}

void VoiceActivityDetector::accept_waveform(span<const float> samples) {
  if (static_cast<int>(samples.size()) != config_.window_size) {
    throw std::invalid_argument("accept_waveform: expected " + std::to_string(config_.window_size) +
                                " samples, got " + std::to_string(samples.size()));
  }

  const float   prob           = infer(samples);
  const int64_t window_samples = config_.window_size;
  const auto    min_silence_samples =
      static_cast<int64_t>(config_.min_silence_duration * static_cast<float>(config_.sample_rate));
  const auto max_speech_samples =
      static_cast<int64_t>(config_.max_speech_duration * static_cast<float>(config_.sample_rate));

  if (prob >= config_.threshold) {
    // Speech detected
    if (!in_speech_) {
      in_speech_            = true;
      speech_start_samples_ = 0;
      speech_buf_.clear();
    }
    silence_samples_ = 0;
    speech_buf_.insert(speech_buf_.end(), samples.begin(), samples.end());
    speech_start_samples_ += window_samples;

    // Force-split if max speech duration exceeded
    if (speech_start_samples_ >= max_speech_samples) {
      spdlog::debug("VAD: force-split at {} samples", speech_start_samples_);
      finalize_segment();
    }
  } else {
    // Silence detected
    if (in_speech_) {
      silence_samples_ += window_samples;
      speech_buf_.insert(speech_buf_.end(), samples.begin(), samples.end());
      speech_start_samples_ += window_samples;

      if (silence_samples_ >= min_silence_samples) {
        finalize_segment();
      }
    }
    // If not in speech, just ignore silence
  }
}

void VoiceActivityDetector::finalize_segment() {
  if (speech_buf_.empty()) {
    in_speech_            = false;
    silence_samples_      = 0;
    speech_start_samples_ = 0;
    return;
  }

  // Check minimum speech duration
  float duration = static_cast<float>(speech_buf_.size()) / static_cast<float>(config_.sample_rate);
  if (duration < config_.min_speech_duration) {
    spdlog::debug("VAD: discarding short segment ({:.3f}s < {:.3f}s)", duration, config_.min_speech_duration);
    in_speech_            = false;
    silence_samples_      = 0;
    speech_start_samples_ = 0;
    speech_buf_.clear();
    return;
  }

  // Zero-copy: move flat buffer directly into segment
  SpeechSegment segment;
  segment.samples = std::move(speech_buf_);

  segments_.push_back(std::move(segment));

  // Reset state machine — re-reserve after move
  in_speech_            = false;
  silence_samples_      = 0;
  speech_start_samples_ = 0;
  // cppcheck-suppress accessMoved  ; reserve() on moved-from vector is valid per C++ standard
  speech_buf_.reserve(
      static_cast<size_t>(config_.max_speech_duration * static_cast<float>(config_.sample_rate)));
}

bool VoiceActivityDetector::empty() const {
  return segments_.empty();
}

const SpeechSegment& VoiceActivityDetector::front() const {
  return segments_.front();
}

void VoiceActivityDetector::pop() {
  segments_.pop_front();
}

bool VoiceActivityDetector::is_speech() const {
  return in_speech_;
}

void VoiceActivityDetector::flush() {
  if (in_speech_ && !speech_buf_.empty()) {
    finalize_segment();
  }
}

void VoiceActivityDetector::reset() {
  in_speech_            = false;
  silence_samples_      = 0;
  speech_start_samples_ = 0;
  speech_buf_.clear();
  segments_.clear();
  context_.assign(static_cast<size_t>(config_.context_size), 0.0f);
  state_.fill(0.0f);
}

}  // namespace asr
