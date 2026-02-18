#include <gtest/gtest.h>
#include <prometheus/registry.h>
#include <prometheus/text_serializer.h>

#include <memory>
#include <string>

#include "asr/metrics.h"

namespace asr {
namespace {

class MetricsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASRMetrics::instance().initialize();
  }
};

TEST_F(MetricsTest, Initialization) {
  EXPECT_NE(ASRMetrics::instance().registry(), nullptr);
}

TEST_F(MetricsTest, ObserveTTFR) {
  EXPECT_NO_THROW(ASRMetrics::instance().observe_ttfr(0.5, "websocket"));
  EXPECT_NO_THROW(ASRMetrics::instance().observe_ttfr(0.3, "http"));
}

TEST_F(MetricsTest, ObserveSegment) {
  EXPECT_NO_THROW(ASRMetrics::instance().observe_segment(2.5, 0.3));
}

TEST_F(MetricsTest, ConnectionOpenClose) {
  ASRMetrics::instance().connection_opened();
  ASRMetrics::instance().connection_opened();
  ASRMetrics::instance().connection_closed("normal", 5.0);
  // Should not crash - gauge operations
}

TEST_F(MetricsTest, SessionStartEnd) {
  ASRMetrics::instance().session_started();
  ASRMetrics::instance().session_ended(10.0);
}

TEST_F(MetricsTest, RecordResult) {
  ASRMetrics::instance().record_result("привет мир");
  ASRMetrics::instance().record_result("");  // empty
}

TEST_F(MetricsTest, RecordSilence) {
  EXPECT_NO_THROW(ASRMetrics::instance().record_silence());
}

TEST_F(MetricsTest, ErrorTypes) {
  EXPECT_NO_THROW(ASRMetrics::instance().observe_error("decode_error"));
  EXPECT_NO_THROW(ASRMetrics::instance().observe_error("invalid_audio"));
  EXPECT_NO_THROW(ASRMetrics::instance().observe_error("timeout"));
}

TEST_F(MetricsTest, RecordAudioLevel) {
  ASRMetrics::instance().record_audio_level(0.05);
  ASRMetrics::instance().record_audio_level(0.001);  // low volume
}

TEST_F(MetricsTest, SetSpeechRatio) {
  EXPECT_NO_THROW(ASRMetrics::instance().set_speech_ratio(0.75));
}

TEST_F(MetricsTest, ObserveRequest) {
  EXPECT_NO_THROW(
      ASRMetrics::instance().observe_request(2.0, 3.0, 0.8, 100, 160000, 0.05, 0.01, "websocket", "success"));
}

TEST_F(MetricsTest, PrometheusSerialize) {
  ASRMetrics::instance().observe_ttfr(0.5, "websocket");
  ASRMetrics::instance().observe_segment(2.0, 0.3);
  ASRMetrics::instance().connection_opened();

  const prometheus::TextSerializer serializer;
  auto                             collected = ASRMetrics::instance().registry()->Collect();
  auto                             text      = serializer.Serialize(collected);
  EXPECT_FALSE(text.empty());
  EXPECT_NE(text.find("gigaam_"), std::string::npos);
}

// Helper to get serialized metrics text
static std::string serialize_metrics() {
  const prometheus::TextSerializer serializer;
  auto                             collected = ASRMetrics::instance().registry()->Collect();
  return serializer.Serialize(collected);
}

TEST_F(MetricsTest, AllPipelineMetricsRegistered) {
  // Trigger some metrics so families with labels get created
  ASRMetrics::instance().observe_ttfr(0.1, "websocket");
  ASRMetrics::instance().observe_ttfr(0.1, "http");
  ASRMetrics::instance().observe_segment(1.0, 0.1);
  ASRMetrics::instance().observe_request(1.0, 2.0, 0.5, 10, 16000, 0.01, 0.0, "websocket", "success");
  ASRMetrics::instance().observe_error("test");

  auto text = serialize_metrics();
  EXPECT_NE(text.find("gigaam_ttfr_seconds"), std::string::npos);
  EXPECT_NE(text.find("gigaam_rtf"), std::string::npos);
  EXPECT_NE(text.find("gigaam_request_duration_seconds"), std::string::npos);
  EXPECT_NE(text.find("gigaam_decode_duration_seconds"), std::string::npos);
  EXPECT_NE(text.find("gigaam_audio_duration_seconds"), std::string::npos);
  EXPECT_NE(text.find("gigaam_segments_total"), std::string::npos);
  EXPECT_NE(text.find("gigaam_requests_total"), std::string::npos);
  EXPECT_NE(text.find("gigaam_errors_total"), std::string::npos);
  EXPECT_NE(text.find("gigaam_active_connections"), std::string::npos);
}

TEST_F(MetricsTest, AllConnectionMetricsRegistered) {
  ASRMetrics::instance().connection_opened();
  ASRMetrics::instance().session_started();
  ASRMetrics::instance().session_ended(1.0);
  ASRMetrics::instance().connection_closed("normal", 2.0);

  auto text = serialize_metrics();
  EXPECT_NE(text.find("gigaam_connections_total"), std::string::npos);
  EXPECT_NE(text.find("gigaam_disconnections_total"), std::string::npos);
  EXPECT_NE(text.find("gigaam_sessions_total"), std::string::npos);
  EXPECT_NE(text.find("gigaam_active_sessions"), std::string::npos);
  EXPECT_NE(text.find("gigaam_connection_duration_seconds"), std::string::npos);
  EXPECT_NE(text.find("gigaam_session_duration_seconds"), std::string::npos);
}

TEST_F(MetricsTest, AllRecognitionMetricsRegistered) {
  ASRMetrics::instance().record_result("hello world test");
  ASRMetrics::instance().record_audio_level(0.05);
  ASRMetrics::instance().record_silence();
  ASRMetrics::instance().set_speech_ratio(0.8);

  auto text = serialize_metrics();
  EXPECT_NE(text.find("gigaam_words_per_request"), std::string::npos);
  EXPECT_NE(text.find("gigaam_audio_rms_level"), std::string::npos);
  EXPECT_NE(text.find("gigaam_empty_results_total"), std::string::npos);
  EXPECT_NE(text.find("gigaam_words_total"), std::string::npos);
  EXPECT_NE(text.find("gigaam_characters_total"), std::string::npos);
  EXPECT_NE(text.find("gigaam_silence_segments_total"), std::string::npos);
  EXPECT_NE(text.find("gigaam_speech_ratio"), std::string::npos);
}

}  // namespace
}  // namespace asr
