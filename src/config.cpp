#include "asr/config.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <string>

namespace asr {

namespace {

std::string get_env(const char* name, const std::string& default_val) {
  const char* val = std::getenv(name);
  return val != nullptr ? std::string(val) : default_val;
}

int get_env_int(const char* name, int default_val) {
  const char* val = std::getenv(name);
  if (val == nullptr)
    return default_val;
  try {
    return std::stoi(val);
  } catch (const std::exception& e) {
    spdlog::warn("{}: invalid integer '{}' ({}), using default {}", name, val, e.what(), default_val);
    return default_val;
  }
}

float get_env_float(const char* name, float default_val) {
  const char* val = std::getenv(name);
  if (val == nullptr)
    return default_val;
  try {
    return std::stof(val);
  } catch (const std::exception& e) {
    spdlog::warn("{}: invalid float '{}' ({}), using default {}", name, val, e.what(), default_val);
    return default_val;
  }
}

uint16_t get_env_uint16(const char* name, uint16_t default_val) {
  int val = get_env_int(name, default_val);
  if (val < 0 || val > 65535) {
    spdlog::warn("{}: value {} out of uint16 range [0, 65535], using default {}", name, val, default_val);
    return default_val;
  }
  return static_cast<uint16_t>(val);
}

size_t get_env_size(const char* name, size_t default_val) {
  const char* val = std::getenv(name);
  if (val == nullptr)
    return default_val;
  try {
    return std::stoull(val);
  } catch (const std::exception& e) {
    spdlog::warn("{}: invalid size '{}' ({}), using default {}", name, val, e.what(), default_val);
    return default_val;
  }
}

}  // namespace

Config Config::from_env() {
  Config cfg;
  cfg.host                    = get_env("HOST", cfg.host);
  cfg.port                    = get_env_uint16("HTTP_PORT", cfg.port);
  cfg.threads                 = get_env_size("THREADS", cfg.threads);
  cfg.model_dir               = get_env("MODEL_DIR", cfg.model_dir);
  cfg.vad_model               = get_env("VAD_MODEL", cfg.vad_model);
  cfg.provider                = get_env("PROVIDER", cfg.provider);
  cfg.num_threads             = get_env_int("NUM_THREADS", cfg.num_threads);
  cfg.sample_rate             = get_env_int("SAMPLE_RATE", cfg.sample_rate);
  cfg.feature_dim             = get_env_int("FEATURE_DIM", cfg.feature_dim);
  cfg.vad_threshold           = get_env_float("VAD_THRESHOLD", cfg.vad_threshold);
  cfg.vad_min_silence         = get_env_float("VAD_MIN_SILENCE", cfg.vad_min_silence);
  cfg.vad_min_speech          = get_env_float("VAD_MIN_SPEECH", cfg.vad_min_speech);
  cfg.vad_max_speech          = get_env_float("VAD_MAX_SPEECH", cfg.vad_max_speech);
  cfg.vad_window_size         = get_env_int("VAD_WINDOW_SIZE", cfg.vad_window_size);
  cfg.vad_context_size        = get_env_int("VAD_CONTEXT_SIZE", cfg.vad_context_size);
  cfg.silence_threshold       = get_env_float("SILENCE_THRESHOLD", cfg.silence_threshold);
  cfg.min_audio_sec           = get_env_float("MIN_AUDIO_SEC", cfg.min_audio_sec);
  cfg.max_audio_sec           = get_env_float("MAX_AUDIO_SEC", cfg.max_audio_sec);
  cfg.max_upload_bytes        = get_env_size("MAX_UPLOAD_BYTES", cfg.max_upload_bytes);
  cfg.max_ws_message_bytes    = get_env_size("MAX_WS_MESSAGE_BYTES", cfg.max_ws_message_bytes);
  cfg.recognizer_pool_size    = get_env_int("RECOGNIZER_POOL_SIZE", cfg.recognizer_pool_size);
  cfg.max_concurrent_requests = get_env_size("MAX_CONCURRENT_REQUESTS", cfg.max_concurrent_requests);
  return cfg;
}

void Config::validate() {
  if (sample_rate <= 0) {
    throw ConfigError("sample_rate must be positive, got " + std::to_string(sample_rate));
  }
  if (sample_rate < 8000 || sample_rate > 48000) {
    spdlog::warn("Clamping sample_rate {} to [8000, 48000]", sample_rate);
    sample_rate = std::clamp(sample_rate, 8000, 48000);
  }

  if (vad_window_size <= 0) {
    throw ConfigError("vad_window_size must be positive, got " + std::to_string(vad_window_size));
  }
  if (vad_window_size < 64 || vad_window_size > 4096) {
    spdlog::warn("Clamping vad_window_size {} to [64, 4096]", vad_window_size);
    vad_window_size = std::clamp(vad_window_size, 64, 4096);
  }

  if (vad_context_size < 0 || vad_context_size >= vad_window_size) {
    throw ConfigError("vad_context_size must be in [0, vad_window_size), got " +
                      std::to_string(vad_context_size));
  }

  if (num_threads < 1 || num_threads > 128) {
    spdlog::warn("Clamping num_threads {} to [1, 128]", num_threads);
    num_threads = std::clamp(num_threads, 1, 128);
  }

  if (threads < 1 || threads > 256) {
    spdlog::warn("Clamping threads {} to [1, 256]", threads);
    threads = std::clamp(threads, static_cast<size_t>(1), static_cast<size_t>(256));
  }

  if (vad_threshold <= 0.0f || vad_threshold >= 1.0f) {
    spdlog::warn("Clamping vad_threshold {} to (0.0, 1.0)", vad_threshold);
    vad_threshold = std::clamp(vad_threshold, 0.01f, 0.99f);
  }

  if (min_audio_sec < 0.0f) {
    spdlog::warn("Clamping min_audio_sec {} to 0", min_audio_sec);
    min_audio_sec = 0.0f;
  }

  if (max_audio_sec <= min_audio_sec) {
    spdlog::warn("max_audio_sec ({}) must be > min_audio_sec ({}), fixing", max_audio_sec, min_audio_sec);
    max_audio_sec = min_audio_sec + 30.0f;
  }

  if (feature_dim <= 0) {
    throw ConfigError("feature_dim must be positive, got " + std::to_string(feature_dim));
  }

  if (max_upload_bytes == 0) {
    throw ConfigError("max_upload_bytes must be positive");
  }

  if (port == 0) {
    throw ConfigError("port must be non-zero");
  }

  if (max_ws_message_bytes == 0) {
    throw ConfigError("max_ws_message_bytes must be positive");
  }

  // Pool size: 0 = auto (= threads)
  if (recognizer_pool_size == 0) {
    recognizer_pool_size = static_cast<int>(threads);
  }
  if (recognizer_pool_size < 1 || recognizer_pool_size > 256) {
    spdlog::warn("Clamping recognizer_pool_size {} to [1, 256]", recognizer_pool_size);
    recognizer_pool_size = std::clamp(recognizer_pool_size, 1, 256);
  }

  // Max concurrent requests: 0 = auto (= threads * 2)
  if (max_concurrent_requests == 0) {
    max_concurrent_requests = threads * 2;
  }

  // Cross-validation: VAD durations
  if (vad_min_silence <= 0.0f) {
    spdlog::warn("Clamping vad_min_silence {} to 0.01", vad_min_silence);
    vad_min_silence = 0.01f;
  }

  if (vad_min_speech <= 0.0f) {
    spdlog::warn("Clamping vad_min_speech {} to 0.01", vad_min_speech);
    vad_min_speech = 0.01f;
  }

  if (vad_max_speech <= vad_min_speech) {
    spdlog::warn("vad_max_speech ({}) must be > vad_min_speech ({}), fixing", vad_max_speech, vad_min_speech);
    vad_max_speech = vad_min_speech + 10.0f;
  }
}

}  // namespace asr
