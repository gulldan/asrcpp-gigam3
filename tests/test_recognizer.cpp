#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "asr/config.h"
#include "asr/recognizer.h"
#include "asr/span.h"

namespace asr {
namespace {

constexpr const char* kModelDir = "models/sherpa-onnx-nemo-transducer-punct-giga-am-v3-russian-2025-12-16";

bool model_exists() {
  const std::ifstream f(std::string(kModelDir) + "/encoder.int8.onnx");
  return f.good();
}

Config make_test_config() {
  Config cfg;
  cfg.model_dir            = kModelDir;
  cfg.provider             = "cpu";
  cfg.num_threads          = 2;
  cfg.sample_rate          = 16000;
  cfg.feature_dim          = 64;
  cfg.recognizer_pool_size = 2;
  return cfg;
}

TEST(Recognizer, Construction) {
  if (!model_exists())
    GTEST_SKIP() << "Model not found";
  auto cfg = make_test_config();
  EXPECT_NO_THROW({ const Recognizer rec(cfg); });
}

TEST(Recognizer, RecognizeSilence) {
  if (!model_exists())
    GTEST_SKIP() << "Model not found";
  auto       cfg = make_test_config();
  Recognizer rec(cfg);

  // 1 second of silence
  std::vector<float> silence(16000, 0.0f);
  auto               text = rec.recognize(silence, 16000);
  // Silence should produce empty or near-empty text
  // (model might produce empty string or just whitespace)
  EXPECT_LE(text.size(), 5u);  // Allow small artifacts
}

TEST(Recognizer, RecognizeEmpty) {
  if (!model_exists())
    GTEST_SKIP() << "Model not found";
  auto       cfg = make_test_config();
  Recognizer rec(cfg);

  const span<const float> empty;
  auto                    text = rec.recognize(empty, 16000);
  EXPECT_TRUE(text.empty());
}

TEST(Recognizer, NonMovable) {
  // Recognizer pool is non-copyable and non-movable
  EXPECT_FALSE(std::is_move_constructible_v<Recognizer>);
  EXPECT_FALSE(std::is_move_assignable_v<Recognizer>);
  EXPECT_FALSE(std::is_copy_constructible_v<Recognizer>);
  EXPECT_FALSE(std::is_copy_assignable_v<Recognizer>);
}

TEST(Recognizer, ThreadSafety) {
  if (!model_exists())
    GTEST_SKIP() << "Model not found";
  auto       cfg = make_test_config();
  Recognizer rec(cfg);

  std::vector<float> audio(16000, 0.0f);  // 1s silence

  constexpr int            kNumThreads = 4;
  std::vector<std::thread> threads;
  std::vector<std::string> results(kNumThreads);

  threads.reserve(kNumThreads);
  for (int i = 0; i < kNumThreads; ++i) {
    threads.emplace_back([&rec, &audio, &results, i]() { results[i] = rec.recognize(audio, 16000); });
  }

  for (auto& t : threads) {
    t.join();
  }

  // All threads should complete without crash
  // Results should be consistent (all empty for silence)
  for (const auto& r : results) {
    EXPECT_LE(r.size(), 5u);
  }
}

}  // namespace
}  // namespace asr
