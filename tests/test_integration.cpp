#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "asr/audio.h"
#include "asr/config.h"
#include "asr/handler.h"
#include "asr/recognizer.h"
#include "asr/span.h"
#include "asr/vad.h"

namespace asr {
namespace {

constexpr const char* kModelDir = "models/sherpa-onnx-nemo-transducer-punct-giga-am-v3-russian-2025-12-16";
constexpr const char* kVadModel = "models/silero_vad.onnx";
constexpr const char* kTestWav =
    "models/sherpa-onnx-nemo-transducer-punct-giga-am-v3-russian-2025-12-16/test_wavs/example.wav";

bool models_exist() {
  const std::ifstream f1(std::string(kModelDir) + "/encoder.int8.onnx");
  const std::ifstream f2(kVadModel);
  return f1.good() && f2.good();
}

bool test_wav_exists() {
  const std::ifstream f(kTestWav);
  return f.good();
}

Config make_config() {
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

std::vector<uint8_t> read_file(const char* path) {
  std::ifstream f(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}

TEST(Integration, WavFileToText) {
  if (!models_exist() || !test_wav_exists())
    GTEST_SKIP() << "Models or test WAV not found";

  auto cfg = make_config();

  // Decode WAV
  auto wav_data = read_file(kTestWav);
  ASSERT_FALSE(wav_data.empty());

  auto audio = decode_wav(wav_data, cfg.sample_rate);
  EXPECT_GT(audio.duration_sec, 0.0f);
  EXPECT_FALSE(audio.samples.empty());

  // Recognize directly
  Recognizer rec(cfg);
  auto       text = rec.recognize(audio.samples, cfg.sample_rate);
  EXPECT_FALSE(text.empty());
  spdlog::info("Integration test recognized: '{}'", text);
}

TEST(Integration, EmptyFileError) {
  const span<const uint8_t> empty;
  EXPECT_THROW(decode_wav(empty), AudioError);
}

TEST(Integration, StreamingSimulation) {
  if (!models_exist() || !test_wav_exists())
    GTEST_SKIP() << "Models or test WAV not found";

  auto       cfg     = make_config();
  auto       vad_cfg = make_vad_config(cfg);
  Recognizer rec(cfg);
  ASRSession session(rec, vad_cfg, cfg);

  // Decode test WAV
  auto wav_data = read_file(kTestWav);
  auto audio    = decode_wav(wav_data, cfg.sample_rate);

  // Feed in chunks (simulating WebSocket streaming)
  constexpr size_t kChunkSize = 4096;  // ~256ms at 16kHz
  bool             got_final  = false;

  for (size_t offset = 0; offset < audio.samples.size(); offset += kChunkSize) {
    const size_t remaining = std::min(kChunkSize, audio.samples.size() - offset);
    auto         chunk     = span<const float>(audio.samples.data() + offset, remaining);
    auto         msgs      = session.on_audio(chunk);

    for (const auto& msg : msgs) {
      if (msg.type == ASRSession::OutMessage::Final) {
        got_final = true;
      }
    }
  }

  // Finalize
  auto final_msgs = session.on_recognize();
  for (const auto& msg : final_msgs) {
    if (msg.type == ASRSession::OutMessage::Final) {
      got_final = true;
    }
    if (msg.type == ASRSession::OutMessage::Done) {
      // Should always get done
      SUCCEED();
    }
  }

  // We should have gotten at least the done message and a final result
  ASSERT_FALSE(final_msgs.empty());
  EXPECT_EQ(final_msgs.back().type, ASRSession::OutMessage::Done);
  EXPECT_TRUE(got_final);
}

}  // namespace
}  // namespace asr
