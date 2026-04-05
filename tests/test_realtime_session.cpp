#include <gtest/gtest.h>
#include <math.h>
#include <opus/opus.h>
#include <opus/opus_defines.h>
#include <opus/opus_types.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "asr/audio.h"
#include "asr/config.h"
#include "asr/realtime_session.h"
#include "asr/span.h"
#include "asr/vad.h"
#include "nlohmann/detail/json_ref.hpp"

namespace asr {
namespace {

nlohmann::json parse_event_json(const std::string& payload) {
  const auto parsed = nlohmann::json::parse(payload, nullptr, false);
  EXPECT_FALSE(parsed.is_discarded()) << payload;
  EXPECT_TRUE(parsed.is_object()) << payload;
  return parsed;
}

std::vector<uint8_t> encode_opus_packet(int sample_rate, int frame_samples, float phase_shift) {
  int          error   = OPUS_OK;
  OpusEncoder* encoder = opus_encoder_create(sample_rate, 1, OPUS_APPLICATION_VOIP, &error);
  if (encoder == nullptr || error != OPUS_OK) {
    throw std::runtime_error("Failed to create Opus encoder in test");
  }

  (void)opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(0));
  (void)opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(0));

  std::vector<float> pcm(static_cast<size_t>(frame_samples));
  constexpr float    kPi = 3.14159265358979323846F;
  for (int i = 0; i < frame_samples; ++i) {
    const float t               = (static_cast<float>(i) / static_cast<float>(sample_rate));
    pcm[static_cast<size_t>(i)] = 0.25F * std::sin((2.0F * kPi * 440.0F * t) + phase_shift);
  }

  std::vector<uint8_t> packet(1500U);
  const int            encoded = opus_encode_float(encoder, pcm.data(), frame_samples, packet.data(),
                                                   static_cast<opus_int32>(packet.size()));
  opus_encoder_destroy(encoder);

  if (encoded <= 0) {
    throw std::runtime_error("Failed to encode Opus packet in test");
  }
  packet.resize(static_cast<size_t>(encoded));
  return packet;
}

std::vector<uint8_t> wrap_rtp_packet(span<const uint8_t> opus_payload, uint16_t sequence,
                                     uint32_t timestamp) {
  std::vector<uint8_t> packet(12U + opus_payload.size(), 0U);
  packet[0]  = 0x80U;  // RTP v2
  packet[1]  = 111U;   // dynamic payload type (typical for Opus in WebRTC)
  packet[2]  = static_cast<uint8_t>((sequence >> 8U) & 0xFFU);
  packet[3]  = static_cast<uint8_t>(sequence & 0xFFU);
  packet[4]  = static_cast<uint8_t>((timestamp >> 24U) & 0xFFU);
  packet[5]  = static_cast<uint8_t>((timestamp >> 16U) & 0xFFU);
  packet[6]  = static_cast<uint8_t>((timestamp >> 8U) & 0xFFU);
  packet[7]  = static_cast<uint8_t>(timestamp & 0xFFU);
  packet[8]  = 0x12U;
  packet[9]  = 0x34U;
  packet[10] = 0x56U;
  packet[11] = 0x78U;
  std::copy(opus_payload.begin(), opus_payload.end(), packet.begin() + 12);
  return packet;
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
  update["input_audio_format"]        = "PCM16";
  update["input_sample_rate"]         = 24000;
  update["input_audio_transcription"] = {
      {"model", "default"},
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
  EXPECT_EQ(cfg.input_audio_transcription.model, "default");
  EXPECT_TRUE(cfg.input_audio_transcription.language.empty());
  EXPECT_TRUE(cfg.input_audio_transcription.prompt.empty());
  ASSERT_TRUE(cfg.turn_detection.has_value());
  const auto turn = cfg.turn_detection.value_or(RealtimeSessionConfig::TurnDetection{});
  EXPECT_EQ(turn.type, "server_vad");
  EXPECT_FLOAT_EQ(turn.threshold, 0.7F);
  EXPECT_EQ(turn.prefix_padding_ms, 150);
  EXPECT_EQ(turn.silence_duration_ms, 600);
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

TEST(RealtimeSession, ApplySessionUpdateRejectsUnsupportedTranscriptionFields) {
  RealtimeSession session(8);
  std::string     error;

  nlohmann::json bad_model = {
      {"input_audio_transcription", {{"model", "gigaam-v3"}}},
  };
  EXPECT_FALSE(session.apply_session_update(bad_model, &error));
  EXPECT_NE(error.find("model"), std::string::npos);

  error.clear();
  nlohmann::json bad_language = {
      {"input_audio_transcription", {{"language", "ru"}}},
  };
  EXPECT_FALSE(session.apply_session_update(bad_language, &error));
  EXPECT_NE(error.find("language"), std::string::npos);

  error.clear();
  nlohmann::json bad_prompt = {
      {"input_audio_transcription", {{"prompt", "test"}}},
  };
  EXPECT_FALSE(session.apply_session_update(bad_prompt, &error));
  EXPECT_NE(error.find("prompt"), std::string::npos);
}

TEST(RealtimeSession, TurnDetectionCanBeDisabledWithNull) {
  RealtimeSession session(9);

  std::string    error;
  nlohmann::json update;
  update["turn_detection"] = nullptr;
  ASSERT_TRUE(session.apply_session_update(update, &error)) << error;
  EXPECT_FALSE(session.config().turn_detection.has_value());

  const auto  event  = parse_event_json(session.event_session_updated());
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

  const auto event =
      parse_event_json(session.event_error("invalid_audio", "bad base64", "audio", "evt_client_123"));

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
  EXPECT_THROW(decode_realtime_audio("AAAA", "bogus"), AudioError);
  EXPECT_THROW(decode_realtime_audio("", "opus"), AudioError);
}

TEST(RealtimeSessionAudio, DecodeRealtimeAudioBytesDecodesExplicitOpusRawAndRtp) {
  constexpr int kSampleRate   = 48000;
  constexpr int kFrameSamples = 960;

  const auto raw_packet = encode_opus_packet(kSampleRate, kFrameSamples, 0.0F);
  const auto raw_audio  = decode_realtime_audio_bytes(raw_packet, "opus_raw", kSampleRate);
  EXPECT_GE(raw_audio.size(), static_cast<size_t>(kFrameSamples));

  const auto rtp_packet = wrap_rtp_packet(raw_packet, 10, 48000);
  const auto rtp_audio  = decode_realtime_audio_bytes(rtp_packet, "opus_rtp", kSampleRate);
  EXPECT_GE(rtp_audio.size(), static_cast<size_t>(kFrameSamples));
}

TEST(RealtimeSessionAudio, RealtimeOpusDecoderDecodesRawPacket) {
  constexpr int kSampleRate   = 48000;
  constexpr int kFrameSamples = 960;  // 20 ms at 48 kHz

  RealtimeOpusDecoder decoder(kSampleRate, 8, true, OpusPacketMode::Raw);
  const auto          packet  = encode_opus_packet(kSampleRate, kFrameSamples, 0.0F);
  const auto          samples = decoder.decode_packet(packet);

  EXPECT_GE(samples.size(), static_cast<size_t>(kFrameSamples));
  const auto& stats = decoder.stats();
  EXPECT_EQ(stats.decoded_packets, 1U);
  EXPECT_EQ(stats.rtp_packets, 0U);
}

TEST(RealtimeSessionAudio, RealtimeOpusDecoderHandlesSinglePacketLossForRtp) {
  constexpr int kSampleRate   = 48000;
  constexpr int kFrameSamples = 960;  // 20 ms at 48 kHz

  RealtimeOpusDecoder decoder(kSampleRate, 8, true, OpusPacketMode::Rtp);
  const auto          payload1 = encode_opus_packet(kSampleRate, kFrameSamples, 0.0F);
  const auto          payload2 = encode_opus_packet(kSampleRate, kFrameSamples, 0.5F);
  const auto          rtp1     = wrap_rtp_packet(payload1, 100, 48000);
  const auto          rtp2     = wrap_rtp_packet(payload2, 102, 49920);  // sequence 101 is missing

  const auto first  = decoder.decode_packet(rtp1);
  const auto second = decoder.decode_packet(rtp2);

  EXPECT_GE(first.size(), static_cast<size_t>(kFrameSamples));
  EXPECT_GE(second.size(), static_cast<size_t>(kFrameSamples * 2));

  const auto& stats = decoder.stats();
  EXPECT_EQ(stats.rtp_packets, 2U);
  EXPECT_EQ(stats.lost_packets, 1U);
  EXPECT_EQ(stats.plc_packets + stats.fec_packets, 1U);
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
  const auto turn = realtime_cfg.turn_detection.value_or(RealtimeSessionConfig::TurnDetection{});
  EXPECT_EQ(turn.type, "server_vad");
  EXPECT_FLOAT_EQ(turn.threshold, 0.6F);
  EXPECT_EQ(turn.silence_duration_ms, 750);
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
  realtime_cfg.turn_detection                      = RealtimeSessionConfig::TurnDetection{};
  realtime_cfg.turn_detection->threshold           = 0.9F;
  realtime_cfg.turn_detection->prefix_padding_ms   = 180;
  realtime_cfg.turn_detection->silence_duration_ms = 1200;

  const auto vad_cfg = make_realtime_vad_config(cfg, realtime_cfg);
  EXPECT_FLOAT_EQ(vad_cfg.threshold, 0.9F);
  EXPECT_EQ(vad_cfg.prefix_padding_ms, 180);
  EXPECT_NEAR(vad_cfg.min_silence_duration, 1.2F, 1e-6F);
}

TEST(RealtimeSessionConfig, OpusUpdateDefaultsTo48kAndValidatesRates) {
  for (const std::string format : {"opus", "opus_raw", "opus_rtp"}) {
    RealtimeSession session(11);
    nlohmann::json  to_opus = {{"input_audio_format", format}};
    std::string     error;
    ASSERT_TRUE(session.apply_session_update(to_opus, &error)) << format << ": " << error;
    EXPECT_EQ(session.config().input_audio_format, format);
    EXPECT_EQ(session.config().input_sample_rate, 48000);

    nlohmann::json bad_rate = {
        {"input_audio_format", format},
        {"input_sample_rate", 44100},
    };
    error.clear();
    EXPECT_FALSE(session.apply_session_update(bad_rate, &error));
    EXPECT_NE(error.find("input_sample_rate for opus"), std::string::npos);
  }
}

}  // namespace
}  // namespace asr
