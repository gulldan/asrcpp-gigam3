#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include "asr/config.h"

namespace asr {
namespace {

// RAII helper for environment variable management in tests
class ScopedEnv {
 public:
  ScopedEnv(const char* name, const char* value) : name_(name) {
    setenv(name, value, 1);
  }
  ~ScopedEnv() {
    unsetenv(name_);
  }
  ScopedEnv(const ScopedEnv&)            = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;
  ScopedEnv(ScopedEnv&&)                 = delete;
  ScopedEnv& operator=(ScopedEnv&&)      = delete;

 private:
  const char* name_;
};

TEST(Config, DefaultValues) {
  const Config cfg;
  EXPECT_EQ(cfg.host, "0.0.0.0");
  EXPECT_EQ(cfg.port, 8081);
  EXPECT_EQ(cfg.provider, "cpu");
  EXPECT_EQ(cfg.num_threads, 4);
  EXPECT_EQ(cfg.sample_rate, 16000);
  EXPECT_EQ(cfg.feature_dim, 64);
  EXPECT_FLOAT_EQ(cfg.vad_threshold, 0.5f);
  EXPECT_FLOAT_EQ(cfg.vad_min_silence, 0.5f);
  EXPECT_FLOAT_EQ(cfg.vad_min_speech, 0.25f);
  EXPECT_FLOAT_EQ(cfg.vad_max_speech, 20.0f);
  EXPECT_EQ(cfg.vad_window_size, 512);
  EXPECT_EQ(cfg.vad_context_size, 64);
  EXPECT_FLOAT_EQ(cfg.silence_threshold, 0.008f);
  EXPECT_FLOAT_EQ(cfg.min_audio_sec, 0.5f);
  EXPECT_FLOAT_EQ(cfg.max_audio_sec, 30.0f);
  EXPECT_EQ(cfg.model_dir, "models/sherpa-onnx-nemo-transducer-punct-giga-am-v3-russian-2025-12-16");
  EXPECT_EQ(cfg.vad_model, "models/silero_vad.onnx");
}

TEST(Config, FromEnvOverrides) {
  const ScopedEnv e1("HOST", "127.0.0.1");
  const ScopedEnv e2("HTTP_PORT", "9090");
  const ScopedEnv e3("PROVIDER", "cuda");
  const ScopedEnv e4("NUM_THREADS", "8");
  const ScopedEnv e5("VAD_THRESHOLD", "0.7");
  const ScopedEnv e6("SILENCE_THRESHOLD", "0.01");
  const ScopedEnv e7("MIN_AUDIO_SEC", "1.0");
  const ScopedEnv e8("MAX_AUDIO_SEC", "60.0");
  const ScopedEnv e9("VAD_MIN_SILENCE", "0.3");
  const ScopedEnv e10("VAD_MIN_SPEECH", "0.1");
  const ScopedEnv e11("VAD_MAX_SPEECH", "15.0");
  const ScopedEnv e12("MODEL_DIR", "/custom/model");
  const ScopedEnv e13("VAD_MODEL", "/custom/vad.onnx");

  auto cfg = Config::from_env();
  EXPECT_EQ(cfg.host, "127.0.0.1");
  EXPECT_EQ(cfg.port, 9090);
  EXPECT_EQ(cfg.provider, "cuda");
  EXPECT_EQ(cfg.num_threads, 8);
  EXPECT_FLOAT_EQ(cfg.vad_threshold, 0.7f);
  EXPECT_FLOAT_EQ(cfg.silence_threshold, 0.01f);
  EXPECT_FLOAT_EQ(cfg.min_audio_sec, 1.0f);
  EXPECT_FLOAT_EQ(cfg.max_audio_sec, 60.0f);
  EXPECT_FLOAT_EQ(cfg.vad_min_silence, 0.3f);
  EXPECT_FLOAT_EQ(cfg.vad_min_speech, 0.1f);
  EXPECT_FLOAT_EQ(cfg.vad_max_speech, 15.0f);
  EXPECT_EQ(cfg.model_dir, "/custom/model");
  EXPECT_EQ(cfg.vad_model, "/custom/vad.onnx");
}

TEST(Config, MissingEnvUsesDefaults) {
  // Ensure env vars are not set
  unsetenv("HOST");
  unsetenv("HTTP_PORT");
  unsetenv("PROVIDER");
  unsetenv("NUM_THREADS");

  auto cfg = Config::from_env();
  EXPECT_EQ(cfg.host, "0.0.0.0");
  EXPECT_EQ(cfg.port, 8081);
  EXPECT_EQ(cfg.provider, "cpu");
  EXPECT_EQ(cfg.num_threads, 4);
}

TEST(Config, InvalidEnvValues) {
  const ScopedEnv e1("HTTP_PORT", "not_a_number");
  const ScopedEnv e2("NUM_THREADS", "abc");
  const ScopedEnv e3("VAD_THRESHOLD", "xyz");

  auto cfg = Config::from_env();
  // Should fall back to defaults on parse failure (with logged warnings)
  EXPECT_EQ(cfg.port, 8081);
  EXPECT_EQ(cfg.num_threads, 4);
  EXPECT_FLOAT_EQ(cfg.vad_threshold, 0.5f);
}

TEST(Config, PortOverflowUsesDefault) {
  const ScopedEnv e("HTTP_PORT", "70000");
  auto            cfg = Config::from_env();
  EXPECT_EQ(cfg.port, 8081);  // default, not truncated 70000
}

TEST(Config, PortNegativeUsesDefault) {
  const ScopedEnv e("HTTP_PORT", "-1");
  auto            cfg = Config::from_env();
  EXPECT_EQ(cfg.port, 8081);
}

TEST(ConfigValidation, RejectsZeroPort) {
  Config cfg;
  cfg.port = 0;
  EXPECT_THROW(cfg.validate(), ConfigError);
}

TEST(ConfigValidation, DefaultsPass) {
  Config cfg;
  EXPECT_NO_THROW(cfg.validate());
}

TEST(ConfigValidation, RejectsZeroSampleRate) {
  Config cfg;
  cfg.sample_rate = 0;
  EXPECT_THROW(cfg.validate(), ConfigError);
}

TEST(ConfigValidation, RejectsNegativeSampleRate) {
  Config cfg;
  cfg.sample_rate = -1;
  EXPECT_THROW(cfg.validate(), ConfigError);
}

TEST(ConfigValidation, ClampsSampleRate) {
  Config cfg;
  cfg.sample_rate = 100;
  cfg.validate();
  EXPECT_GE(cfg.sample_rate, 8000);
  EXPECT_LE(cfg.sample_rate, 48000);
}

TEST(ConfigValidation, RejectsZeroWindowSize) {
  Config cfg;
  cfg.vad_window_size = 0;
  EXPECT_THROW(cfg.validate(), ConfigError);
}

TEST(ConfigValidation, RejectsContextGEWindow) {
  Config cfg;
  cfg.vad_context_size = cfg.vad_window_size;
  EXPECT_THROW(cfg.validate(), ConfigError);
}

TEST(ConfigValidation, ClampsThreshold) {
  Config cfg;
  cfg.vad_threshold = 0.0f;
  cfg.validate();
  EXPECT_GT(cfg.vad_threshold, 0.0f);
  EXPECT_LT(cfg.vad_threshold, 1.0f);
}

TEST(ConfigValidation, FixesMaxAudioSec) {
  Config cfg;
  cfg.max_audio_sec = 0.1f;
  cfg.min_audio_sec = 0.5f;
  cfg.validate();
  EXPECT_GT(cfg.max_audio_sec, cfg.min_audio_sec);
}

TEST(ConfigValidation, ClampsThreads) {
  Config cfg;
  cfg.num_threads = 500;
  cfg.validate();
  EXPECT_LE(cfg.num_threads, 128);
}

TEST(ConfigValidation, ValidateServerThreads) {
  Config cfg;
  cfg.threads = 1000;
  cfg.validate();
  EXPECT_LE(cfg.threads, static_cast<size_t>(256));
}

TEST(ConfigValidation, VadMaxSpeechMustExceedMinSpeech) {
  Config cfg;
  cfg.vad_max_speech = 0.1f;
  cfg.vad_min_speech = 0.25f;
  cfg.validate();
  EXPECT_GT(cfg.vad_max_speech, cfg.vad_min_speech);
}

TEST(ConfigValidation, ClampsMinSilence) {
  Config cfg;
  cfg.vad_min_silence = -1.0f;
  cfg.validate();
  EXPECT_GT(cfg.vad_min_silence, 0.0f);
}

TEST(ConfigValidation, ClampsMinSpeech) {
  Config cfg;
  cfg.vad_min_speech = 0.0f;
  cfg.validate();
  EXPECT_GT(cfg.vad_min_speech, 0.0f);
}

TEST(Config, DefaultPoolValues) {
  const Config cfg;
  EXPECT_EQ(cfg.recognizer_pool_size, 0);
  EXPECT_EQ(cfg.max_concurrent_requests, static_cast<size_t>(0));
}

TEST(Config, FromEnvPoolOverrides) {
  const ScopedEnv e1("RECOGNIZER_POOL_SIZE", "4");
  const ScopedEnv e2("MAX_CONCURRENT_REQUESTS", "16");

  auto cfg = Config::from_env();
  EXPECT_EQ(cfg.recognizer_pool_size, 4);
  EXPECT_EQ(cfg.max_concurrent_requests, static_cast<size_t>(16));
}

TEST(ConfigValidation, PoolSizeAutoDefault) {
  Config cfg;
  cfg.recognizer_pool_size = 0;
  cfg.validate();
  EXPECT_EQ(cfg.recognizer_pool_size, static_cast<int>(cfg.threads));
}

TEST(ConfigValidation, MaxConcurrentAutoDefault) {
  Config cfg;
  cfg.max_concurrent_requests = 0;
  cfg.validate();
  EXPECT_EQ(cfg.max_concurrent_requests, cfg.threads * 2);
}

TEST(ConfigValidation, ClampsPoolSize) {
  Config cfg;
  cfg.recognizer_pool_size = 500;
  cfg.validate();
  EXPECT_LE(cfg.recognizer_pool_size, 256);
}

}  // namespace
}  // namespace asr
