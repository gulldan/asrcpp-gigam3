#include <gtest/gtest.h>

#include <cmath>
#include <string>

#include <nlohmann/json.hpp>

#include "asr/audio.h"
#include "asr/config.h"
#include "asr/realtime_session.h"

namespace asr {
namespace {

nlohmann::json parse_event_json(const std::string& payload) {
  const auto parsed = nlohmann::json::parse(payload, nullptr, false);
  EXPECT_FALSE(parsed.is_discarded()) << payload;
  EXPECT_TRUE(parsed.is_object()) << payload;
  return parsed;
}

TEST(RealtimeSession, SessionCreatedHasExpectedSchema) {
  RealtimeSession session(42);
  const auto      event = parse_event_json(session.event_session_created());

  EXPECT_EQ(event.at("type"), "session.created");
  EXPECT_EQ(event.at("event_id"), "evt_1");

  const auto& s = event.at("session");
  EXPECT_EQ(s.at("id"), "sess_42");
  EXPECT_EQ(s.at("object"), "realtime.transcription_session");
  EXPECT_EQ(s.at("input_audio_format"), "pcm16");
  EXPECT_EQ(s.at("input_sample_rate"), 16000);
  ASSERT_TRUE(s.contains("turn_detection"));
  EXPECT_TRUE(s.at("turn_detection").is_object());
}

TEST(RealtimeSession, ApplySessionUpdateAcceptsValidFields) {
  RealtimeSession session(1);

  nlohmann::json update;
  update["input_audio_format"] = "PCM16";
  update["input_sample_rate"]  = 24000;
  update["input_audio_transcription"] = {
      {"model", "gigam-v3"},
      {"language", "ru"},
      {"prompt", "test prompt"},
  };
  update["turn_detection"] = {
      {"type", "server_vad"},
      {"threshold", 0.7},
      {"prefix_padding_ms", 150},
      {"silence_duration_ms", 600},
  };

  std::string error;
  ASSERT_TRUE(session.apply_session_update(update, &error)) << error;
  EXPECT_TRUE(error.empty());

  const auto& cfg = session.config();
  EXPECT_EQ(cfg.input_audio_format, "pcm16");
  EXPECT_EQ(cfg.input_sample_rate, 24000);
  EXPECT_EQ(cfg.input_audio_transcription.model, "gigam-v3");
  EXPECT_EQ(cfg.input_audio_transcription.language, "ru");
  EXPECT_EQ(cfg.input_audio_transcription.prompt, "test prompt");
  ASSERT_TRUE(cfg.turn_detection.has_value());
  EXPECT_EQ(cfg.turn_detection->type, "server_vad");
  EXPECT_FLOAT_EQ(cfg.turn_detection->threshold, 0.7F);
  EXPECT_EQ(cfg.turn_detection->prefix_padding_ms, 150);
  EXPECT_EQ(cfg.turn_detection->silence_duration_ms, 600);
}

TEST(RealtimeSession, ApplySessionUpdateRejectsInvalidFields) {
  RealtimeSession session(7);

  std::string error;
  EXPECT_FALSE(session.apply_session_update(nlohmann::json::array(), &error));
  EXPECT_NE(error.find("session must be an object"), std::string::npos);

  error.clear();
  nlohmann::json bad_rate;
  bad_rate["input_sample_rate"] = 7999;
  EXPECT_FALSE(session.apply_session_update(bad_rate, &error));
  EXPECT_NE(error.find("input_sample_rate"), std::string::npos);

  error.clear();
  nlohmann::json bad_turn;
  bad_turn["turn_detection"] = {{"type", "client_vad"}};
  EXPECT_FALSE(session.apply_session_update(bad_turn, &error));
  EXPECT_NE(error.find("turn_detection.type"), std::string::npos);
}

TEST(RealtimeSession, TurnDetectionCanBeDisabledWithNull) {
  RealtimeSession session(9);

  std::string   error;
  nlohmann::json update;
  update["turn_detection"] = nullptr;
  ASSERT_TRUE(session.apply_session_update(update, &error)) << error;
  EXPECT_FALSE(session.config().turn_detection.has_value());

  const auto event   = parse_event_json(session.event_session_updated());
  const auto& nested = event.at("session");
  ASSERT_TRUE(nested.contains("turn_detection"));
  EXPECT_TRUE(nested.at("turn_detection").is_null());
}

TEST(RealtimeSession, ItemAndEventIdsAreMonotonic) {
  RealtimeSession session(2);

  const auto created = parse_event_json(session.event_session_created());
  EXPECT_EQ(created.at("event_id"), "evt_1");

  const auto started = parse_event_json(session.event_speech_started(10));
  EXPECT_EQ(started.at("event_id"), "evt_2");
  EXPECT_EQ(started.at("item_id"), "item_1");

  const auto stopped = parse_event_json(session.event_speech_stopped(20));
  EXPECT_EQ(stopped.at("event_id"), "evt_3");
  EXPECT_EQ(stopped.at("item_id"), "item_1");

  const auto first_commit = session.commit_current_item();
  EXPECT_EQ(first_commit.item_id, "item_1");
  EXPECT_TRUE(first_commit.previous_item_id.empty());

  const auto committed_evt_1 = parse_event_json(session.event_buffer_committed(first_commit));
  EXPECT_EQ(committed_evt_1.at("event_id"), "evt_4");
  EXPECT_EQ(committed_evt_1.at("item_id"), "item_1");
  EXPECT_TRUE(committed_evt_1.at("previous_item_id").is_null());

  const auto started_2 = parse_event_json(session.event_speech_started(30));
  EXPECT_EQ(started_2.at("event_id"), "evt_5");
  EXPECT_EQ(started_2.at("item_id"), "item_2");

  const auto second_commit = session.commit_current_item();
  EXPECT_EQ(second_commit.item_id, "item_2");
  EXPECT_EQ(second_commit.previous_item_id, "item_1");

  const auto committed_evt_2 = parse_event_json(session.event_buffer_committed(second_commit));
  EXPECT_EQ(committed_evt_2.at("event_id"), "evt_6");
  EXPECT_EQ(committed_evt_2.at("item_id"), "item_2");
  EXPECT_EQ(committed_evt_2.at("previous_item_id"), "item_1");
}

TEST(RealtimeSession, ErrorEventIncludesClientEventId) {
  RealtimeSession session(3);

  const auto event = parse_event_json(
      session.event_error("invalid_audio", "bad base64", "audio", "evt_client_123"));

  EXPECT_EQ(event.at("type"), "error");
  EXPECT_EQ(event.at("event_id"), "evt_1");
  ASSERT_TRUE(event.contains("error"));

  const auto& err = event.at("error");
  EXPECT_EQ(err.at("type"), "invalid_request_error");
  EXPECT_EQ(err.at("code"), "invalid_audio");
  EXPECT_EQ(err.at("message"), "bad base64");
  EXPECT_EQ(err.at("param"), "audio");
  EXPECT_EQ(err.at("event_id"), "evt_client_123");
}

TEST(RealtimeSessionAudio, DecodeRealtimeAudioPcm16) {
  // Bytes: 0x00 0x00 (0), 0xFF 0x7F (32767) -> base64 AAD/fw==
  const auto samples = decode_realtime_audio("AAD/fw==", "pcm16");
  ASSERT_EQ(samples.size(), 2U);
  EXPECT_FLOAT_EQ(samples[0], 0.0F);
  EXPECT_NEAR(samples[1], 32767.0F / 32768.0F, 1e-6F);
}

TEST(RealtimeSessionAudio, DecodeRealtimeAudioRejectsInvalidPayloadAndFormat) {
  EXPECT_THROW(decode_realtime_audio("$", "pcm16"), AudioError);
  EXPECT_THROW(decode_realtime_audio("AAAA", "g711_ulaw"), AudioError);
  EXPECT_THROW(decode_realtime_audio("AAAA", "opus"), AudioError);
}

TEST(RealtimeSessionConfig, DefaultConfigUsesServerDefaults) {
  Config cfg;
  cfg.sample_rate     = 16000;
  cfg.vad_threshold   = 0.6F;
  cfg.vad_min_silence = 0.75F;

  const auto realtime_cfg = make_default_realtime_session_config(cfg);
  EXPECT_EQ(realtime_cfg.input_audio_format, "pcm16");
  EXPECT_EQ(realtime_cfg.input_sample_rate, 16000);
  ASSERT_TRUE(realtime_cfg.turn_detection.has_value());
  EXPECT_EQ(realtime_cfg.turn_detection->type, "server_vad");
  EXPECT_FLOAT_EQ(realtime_cfg.turn_detection->threshold, 0.6F);
  EXPECT_EQ(realtime_cfg.turn_detection->silence_duration_ms, 750);
}

TEST(RealtimeSessionConfig, RealtimeVadConfigUsesRealtimeThreshold) {
  Config cfg;
  cfg.vad_model        = "models/silero_vad.onnx";
  cfg.vad_threshold    = 0.2F;
  cfg.vad_min_silence  = 0.3F;
  cfg.vad_min_speech   = 0.25F;
  cfg.vad_max_speech   = 20.0F;
  cfg.sample_rate      = 16000;
  cfg.vad_window_size  = 512;
  cfg.vad_context_size = 64;

  RealtimeSessionConfig realtime_cfg;
  realtime_cfg.turn_detection = RealtimeSessionConfig::TurnDetection{};
  realtime_cfg.turn_detection->threshold = 0.9F;
  realtime_cfg.turn_detection->silence_duration_ms = 1200;

  const auto vad_cfg = make_realtime_vad_config(cfg, realtime_cfg);
  EXPECT_FLOAT_EQ(vad_cfg.threshold, 0.9F);
  EXPECT_NEAR(vad_cfg.min_silence_duration, 1.2F, 1e-6F);
}

}  // namespace
}  // namespace asr
