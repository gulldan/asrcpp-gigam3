#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <vector>

#include "asr/config.h"
#include "asr/handler.h"
#include "asr/recognizer.h"
#include "asr/span.h"
#include "asr/vad.h"

namespace asr {
namespace {

constexpr const char* kModelDir = "models/sherpa-onnx-nemo-transducer-punct-giga-am-v3-russian-2025-12-16";
constexpr const char* kVadModel = "models/silero_vad.onnx";

bool models_exist() {
  const std::ifstream f1(std::string(kModelDir) + "/encoder.int8.onnx");
  const std::ifstream f2(kVadModel);
  return f1.good() && f2.good();
}

Config make_test_config() {
  Config cfg;
  cfg.model_dir         = kModelDir;
  cfg.vad_model         = kVadModel;
  cfg.provider          = "cpu";
  cfg.num_threads       = 2;
  cfg.sample_rate       = 16000;
  cfg.feature_dim       = 64;
  cfg.vad_threshold     = 0.5f;
  cfg.vad_min_silence   = 0.5f;
  cfg.vad_min_speech    = 0.25f;
  cfg.vad_max_speech    = 20.0f;
  cfg.vad_window_size   = 512;
  cfg.vad_context_size  = 64;
  cfg.silence_threshold = 0.008f;
  cfg.min_audio_sec     = 0.5f;
  cfg.max_audio_sec     = 30.0f;
  return cfg;
}

VadConfig make_vad_config(const Config& cfg) {
  VadConfig vc;
  vc.model_path           = cfg.vad_model;
  vc.threshold            = cfg.vad_threshold;
  vc.min_silence_duration = cfg.vad_min_silence;
  vc.min_speech_duration  = cfg.vad_min_speech;
  vc.max_speech_duration  = cfg.vad_max_speech;
  vc.sample_rate          = cfg.sample_rate;
  vc.window_size          = cfg.vad_window_size;
  vc.context_size         = cfg.vad_context_size;
  return vc;
}

TEST(Handler, OnAudioInterim) {
  if (!models_exist())
    GTEST_SKIP() << "Models not found";

  auto       cfg     = make_test_config();
  auto       vad_cfg = make_vad_config(cfg);
  Recognizer rec(cfg);
  ASRSession session(rec, vad_cfg, cfg);

  // Send small silent chunk - should get interim
  std::vector<float> silence(1024, 0.0f);
  auto               messages = session.on_audio(silence);

  ASSERT_FALSE(messages.empty());
  // First response should be interim for silence
  bool has_interim = false;
  for (const auto& msg : messages) {
    if (msg.type == ASRSession::OutMessage::Interim) {
      has_interim = true;
      EXPECT_NE(msg.json.find("\"type\":\"interim\""), std::string::npos);
    }
  }
  EXPECT_TRUE(has_interim);
}

TEST(Handler, OnRecognizeFlush) {
  if (!models_exist())
    GTEST_SKIP() << "Models not found";

  auto       cfg     = make_test_config();
  auto       vad_cfg = make_vad_config(cfg);
  Recognizer rec(cfg);
  ASRSession session(rec, vad_cfg, cfg);

  // Send some audio then recognize
  std::vector<float> silence(4096, 0.0f);
  session.on_audio(silence);
  auto messages = session.on_recognize();

  // Should end with done message
  ASSERT_FALSE(messages.empty());
  const auto& last = messages.back();
  EXPECT_EQ(last.type, ASRSession::OutMessage::Done);
  EXPECT_NE(last.json.find("\"type\":\"done\""), std::string::npos);
}

TEST(Handler, OnReset) {
  if (!models_exist())
    GTEST_SKIP() << "Models not found";

  auto       cfg     = make_test_config();
  auto       vad_cfg = make_vad_config(cfg);
  Recognizer rec(cfg);
  ASRSession session(rec, vad_cfg, cfg);

  std::vector<float> audio(2048, 0.0f);
  session.on_audio(audio);

  // Reset should not crash
  EXPECT_NO_THROW(session.on_reset());

  // Should be able to process audio after reset
  auto messages = session.on_audio(audio);
  EXPECT_FALSE(messages.empty());
}

TEST(Handler, MultipleSessionsOnConnection) {
  if (!models_exist())
    GTEST_SKIP() << "Models not found";

  auto       cfg     = make_test_config();
  auto       vad_cfg = make_vad_config(cfg);
  Recognizer rec(cfg);
  ASRSession session(rec, vad_cfg, cfg);

  // First session
  std::vector<float> silence(2048, 0.0f);
  session.on_audio(silence);
  auto msgs1 = session.on_recognize();
  ASSERT_FALSE(msgs1.empty());
  EXPECT_EQ(msgs1.back().type, ASRSession::OutMessage::Done);

  // Second session - should work after done
  session.on_audio(silence);
  auto msgs2 = session.on_recognize();
  ASSERT_FALSE(msgs2.empty());
  EXPECT_EQ(msgs2.back().type, ASRSession::OutMessage::Done);
}

}  // namespace
}  // namespace asr
