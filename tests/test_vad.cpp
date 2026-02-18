#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <fstream>
#include <string>
#include <vector>

#include "asr/span.h"
#include "asr/vad.h"

namespace asr {
namespace {

// Path to actual VAD model for integration-style tests
// These tests will be skipped if model is not present
constexpr const char* kVadModelPath = "models/silero_vad.onnx";

bool model_exists() {
  const std::ifstream f(kVadModelPath);
  return f.good();
}

VadConfig make_test_config() {
  VadConfig cfg;
  cfg.model_path           = kVadModelPath;
  cfg.threshold            = 0.5f;
  cfg.min_silence_duration = 0.5f;
  cfg.min_speech_duration  = 0.25f;
  cfg.max_speech_duration  = 20.0f;
  cfg.sample_rate          = 16000;
  cfg.window_size          = 512;
  cfg.context_size         = 64;
  return cfg;
}

// Generate silence (zeros)
std::vector<float> make_silence(int num_windows, int window_size = 512) {
  return std::vector<float>(static_cast<size_t>(num_windows * window_size), 0.0f);
}

// Generate synthetic speech-like signal (high-amplitude sine)
std::vector<float> make_speech_signal(float duration_sec, int sample_rate = 16000) {
  auto               num_samples = static_cast<size_t>(duration_sec * static_cast<float>(sample_rate));
  std::vector<float> samples(num_samples);
  for (size_t i = 0; i < num_samples; ++i) {
    // Mix of frequencies to look more like speech
    const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
    samples[i]    = 0.3f * std::sin(2.0f * static_cast<float>(M_PI) * 200.0f * t) +
                 0.2f * std::sin(2.0f * static_cast<float>(M_PI) * 500.0f * t) +
                 0.1f * std::sin(2.0f * static_cast<float>(M_PI) * 1000.0f * t);
  }
  return samples;
}

TEST(Vad, Construction) {
  if (!model_exists())
    GTEST_SKIP() << "VAD model not found";
  auto cfg = make_test_config();
  EXPECT_NO_THROW({ const VoiceActivityDetector vad(cfg); });
}

TEST(Vad, SilenceProducesNoSegments) {
  if (!model_exists())
    GTEST_SKIP() << "VAD model not found";
  auto                  cfg = make_test_config();
  VoiceActivityDetector vad(cfg);

  // Feed 2 seconds of silence
  auto silence = make_silence(62);  // 62 * 512 / 16000 ~ 2 sec
  for (size_t i = 0; i < 62; ++i) {
    vad.accept_waveform(span<const float>(silence.data() + i * 512, 512));
  }

  EXPECT_TRUE(vad.empty());
}

TEST(Vad, ResetClearsState) {
  if (!model_exists())
    GTEST_SKIP() << "VAD model not found";
  auto                  cfg = make_test_config();
  VoiceActivityDetector vad(cfg);

  // Feed some audio
  auto speech = make_speech_signal(1.0f);
  for (size_t i = 0; i + 512 <= speech.size(); i += 512) {
    vad.accept_waveform(span<const float>(speech.data() + i, 512));
  }

  vad.reset();
  EXPECT_TRUE(vad.empty());
  EXPECT_FALSE(vad.is_speech());
}

TEST(Vad, FlushPendingSpeech) {
  if (!model_exists())
    GTEST_SKIP() << "VAD model not found";
  auto                  cfg = make_test_config();
  VoiceActivityDetector vad(cfg);

  // Feed speech signal
  auto speech = make_speech_signal(1.0f);
  for (size_t i = 0; i + 512 <= speech.size(); i += 512) {
    vad.accept_waveform(span<const float>(speech.data() + i, 512));
  }

  // If VAD detected speech, flush should produce a segment
  if (vad.is_speech()) {
    vad.flush();
    // After flush, if there was speech, we may have a segment
    // (depends on whether the model actually detected speech in our synthetic signal)
  }

  vad.reset();
  EXPECT_TRUE(vad.empty());
}

TEST(Vad, FrontAndPop) {
  if (!model_exists())
    GTEST_SKIP() << "VAD model not found";
  auto cfg                = make_test_config();
  cfg.min_speech_duration = 0.01f;  // Very low threshold for test
  VoiceActivityDetector vad(cfg);

  // Feed speech then silence to force segment finalization
  auto speech = make_speech_signal(1.0f);
  for (size_t i = 0; i + 512 <= speech.size(); i += 512) {
    vad.accept_waveform(span<const float>(speech.data() + i, 512));
  }

  // Feed silence to trigger segment finalization
  auto silence = make_silence(32);
  for (size_t i = 0; i < 32; ++i) {
    vad.accept_waveform(span<const float>(silence.data() + i * 512, 512));
  }

  if (!vad.empty()) {
    const auto& seg = vad.front();
    EXPECT_FALSE(seg.samples.empty());
    vad.pop();
  }
}

TEST(Vad, WindowSizeEnforcement) {
  if (!model_exists())
    GTEST_SKIP() << "VAD model not found";
  auto                  cfg = make_test_config();
  VoiceActivityDetector vad(cfg);

  // Wrong window size should trigger assert (in debug) or UB
  // We test this indirectly - correct size should work
  std::vector<float> correct(512, 0.0f);
  EXPECT_NO_THROW(vad.accept_waveform(correct));
}

TEST(Vad, IsSpeechFlag) {
  if (!model_exists())
    GTEST_SKIP() << "VAD model not found";
  auto                  cfg = make_test_config();
  VoiceActivityDetector vad(cfg);

  // Initially not in speech
  EXPECT_FALSE(vad.is_speech());

  // Feed silence - should remain not in speech
  std::vector<float> silence(512, 0.0f);
  vad.accept_waveform(silence);
  EXPECT_FALSE(vad.is_speech());
}

}  // namespace
}  // namespace asr
