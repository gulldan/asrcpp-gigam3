#include "asr/vad.h"

#include <_string.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "asr/span.h"

namespace asr {

namespace {

Ort::SessionOptions make_vad_session_options() {
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  options.SetGraphOptimizationLevel(ORT_ENABLE_ALL);
  options.EnableMemPattern();
  return options;
}

}  // namespace

struct SharedVadRuntime {
  explicit SharedVadRuntime(const std::string& model_path)
      : env(ORT_LOGGING_LEVEL_WARNING, "vad"),
        session(env, model_path.c_str(), make_vad_session_options()),
        memory_info(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)) {
    spdlog::info("VAD runtime loaded: model={} intra_op_threads=1 graph_optimizations=all", model_path);
  }

  Ort::Env        env;
  Ort::Session    session;
  Ort::MemoryInfo memory_info;
};

namespace {

std::shared_ptr<SharedVadRuntime> shared_vad_runtime(const std::string& model_path) {
  static std::mutex runtime_mutex;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
  static std::unordered_map<std::string, std::weak_ptr<SharedVadRuntime>>
      runtimes;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

  const std::scoped_lock lock(runtime_mutex);
  if (auto it = runtimes.find(model_path); it != runtimes.end()) {
    if (auto runtime = it->second.lock()) {
      return runtime;
    }
  }

  auto runtime         = std::make_shared<SharedVadRuntime>(model_path);
  runtimes[model_path] = runtime;
  return runtime;
}

}  // namespace

VoiceActivityDetector::VoiceActivityDetector(const VadConfig& config)
    : config_(config), runtime_(shared_vad_runtime(config.model_path)) {
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
  prefix_padding_samples_ =
      std::max<int64_t>(0, static_cast<int64_t>(config_.prefix_padding_ms) * config_.sample_rate / 1000);
  pre_roll_.reserve(static_cast<size_t>(prefix_padding_samples_));
  speech_buf_.reserve(
      static_cast<size_t>(config_.max_speech_duration * static_cast<float>(config_.sample_rate)) +
      static_cast<size_t>(prefix_padding_samples_));

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

  auto input_tensor = Ort::Value::CreateTensor<float>(runtime_->memory_info, input_buf_.data(),
                                                      input_buf_.size(), input_shape.data(), 2);
  auto state_tensor = Ort::Value::CreateTensor<float>(runtime_->memory_info, state_.data(), state_.size(),
                                                      state_shape.data(), 3);
  auto sr_tensor =
      Ort::Value::CreateTensor<int64_t>(runtime_->memory_info, &sr_tensor_, 1, sr_shape.data(), 1);

  std::array<Ort::Value, 3> inputs;
  inputs[0] = std::move(input_tensor);
  inputs[1] = std::move(state_tensor);
  inputs[2] = std::move(sr_tensor);

  // Run inference
  auto outputs =
      runtime_->session.Run(run_options_, kInputNames, inputs.data(), inputs.size(), kOutputNames, 2);

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
  const int64_t window_start   = total_samples_seen_;
  const int64_t window_end     = window_start + window_samples;
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
      current_start_sample_ = std::max<int64_t>(0, window_start - static_cast<int64_t>(pre_roll_.size()));
      current_end_sample_   = window_end;
      if (!pre_roll_.empty()) {
        speech_buf_.insert(speech_buf_.end(), pre_roll_.begin(), pre_roll_.end());
      }
      transitions_.push_back({SpeechTransition::Started, current_start_sample_});
    }
    silence_samples_ = 0;
    speech_buf_.insert(speech_buf_.end(), samples.begin(), samples.end());
    speech_start_samples_ += window_samples;
    current_end_sample_ = window_end;

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
      current_end_sample_ = std::max<int64_t>(current_start_sample_, window_end - silence_samples_);

      if (silence_samples_ >= min_silence_samples) {
        finalize_segment();
      }
    }
    // If not in speech, just ignore silence
  }

  if (!in_speech_) {
    append_pre_roll(samples);
  } else {
    pre_roll_.clear();
  }
  total_samples_seen_ += window_samples;
}

void VoiceActivityDetector::finalize_segment() {
  if (speech_buf_.empty()) {
    in_speech_            = false;
    silence_samples_      = 0;
    speech_start_samples_ = 0;
    return;
  }

  const size_t trailing_silence = silence_samples_ > 0 ? static_cast<size_t>(silence_samples_) : 0U;
  if (trailing_silence > 0 && trailing_silence < speech_buf_.size()) {
    speech_buf_.resize(speech_buf_.size() - trailing_silence);
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
  segment.start_sample = current_start_sample_;
  segment.end_sample   = current_end_sample_;
  segment.samples      = std::move(speech_buf_);

  segments_.push_back(std::move(segment));
  transitions_.push_back({SpeechTransition::Stopped, current_end_sample_});

  // Reset state machine — re-reserve after move
  in_speech_            = false;
  silence_samples_      = 0;
  speech_start_samples_ = 0;
  // cppcheck-suppress accessMoved  ; reserve() on moved-from vector is valid per C++ standard
  speech_buf_.reserve(
      static_cast<size_t>(config_.max_speech_duration * static_cast<float>(config_.sample_rate)) +
      static_cast<size_t>(prefix_padding_samples_));
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
  total_samples_seen_   = 0;
  current_start_sample_ = 0;
  current_end_sample_   = 0;
  speech_buf_.clear();
  pre_roll_.clear();
  segments_.clear();
  transitions_.clear();
  context_.assign(static_cast<size_t>(config_.context_size), 0.0f);
  state_.fill(0.0f);
}

bool VoiceActivityDetector::has_transition() const {
  return !transitions_.empty();
}

const SpeechTransition& VoiceActivityDetector::front_transition() const {
  return transitions_.front();
}

void VoiceActivityDetector::pop_transition() {
  transitions_.pop_front();
}

void VoiceActivityDetector::append_pre_roll(span<const float> samples) {
  if (prefix_padding_samples_ <= 0 || samples.empty()) {
    return;
  }

  const auto max_prefix = static_cast<size_t>(prefix_padding_samples_);
  if (samples.size() >= max_prefix) {
    pre_roll_.assign(samples.end() - static_cast<ptrdiff_t>(max_prefix), samples.end());
    return;
  }

  const auto needed = pre_roll_.size() + samples.size();
  if (needed > max_prefix) {
    const auto drop = needed - max_prefix;
    std::move(pre_roll_.begin() + static_cast<ptrdiff_t>(drop), pre_roll_.end(), pre_roll_.begin());
    pre_roll_.resize(pre_roll_.size() - drop);
  }
  pre_roll_.insert(pre_roll_.end(), samples.begin(), samples.end());
}

}  // namespace asr
