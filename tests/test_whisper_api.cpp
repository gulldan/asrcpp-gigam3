#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>

#include "asr/whisper_api.h"

namespace asr {
namespace {

TEST(WhisperApi, ParseRequiresModel) {
  WhisperTranscriptionRequest                        request;
  const std::unordered_map<std::string, std::string> fields = {
      {"response_format", "json"},
  };

  auto                            error = parse_whisper_transcription_request(fields, &request);
  const WhisperApiValidationError err   = error.value_or(WhisperApiValidationError{});
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(err.param, "model");
  EXPECT_EQ(err.code, "missing_field");
}

TEST(WhisperApi, ParseRejectsInvalidResponseFormat) {
  WhisperTranscriptionRequest                        request;
  const std::unordered_map<std::string, std::string> fields = {
      {"model", "whisper-1"},
      {"response_format", "yaml"},
  };

  auto                            error = parse_whisper_transcription_request(fields, &request);
  const WhisperApiValidationError err   = error.value_or(WhisperApiValidationError{});
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(err.param, "response_format");
}

TEST(WhisperApi, ParseRejectsTimestampWithoutVerboseJson) {
  WhisperTranscriptionRequest                        request;
  const std::unordered_map<std::string, std::string> fields = {
      {"model", "whisper-1"},
      {"response_format", "json"},
      {"timestamp_granularities[]", "word"},
  };

  auto                            error = parse_whisper_transcription_request(fields, &request);
  const WhisperApiValidationError err   = error.value_or(WhisperApiValidationError{});
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(err.param, "timestamp_granularities[]");
}

TEST(WhisperApi, ParseVerboseJsonGranularities) {
  WhisperTranscriptionRequest                        request;
  const std::unordered_map<std::string, std::string> fields = {
      {"model", "whisper-1"},
      {"response_format", "verbose_json"},
      {"timestamp_granularities[]", "word,segment"},
      {"language", " ru "},
      {"prompt", " test "},
      {"temperature", "0.2"},
      {"stream", "false"},
  };

  auto error = parse_whisper_transcription_request(fields, &request);
  ASSERT_FALSE(error.has_value());
  EXPECT_EQ(request.model, "whisper-1");
  EXPECT_EQ(request.language, "ru");
  EXPECT_EQ(request.prompt, "test");
  ASSERT_TRUE(request.temperature.has_value());
  EXPECT_NEAR(request.temperature.value_or(0.0), 0.2, 1e-6);
  EXPECT_FALSE(request.stream);
  EXPECT_EQ(request.response_format, WhisperResponseFormat::VerboseJson);
  EXPECT_EQ(request.timestamp_granularities.size(), 2);
}

TEST(WhisperApi, ParseRejectsUnsupportedIncludeLogprobs) {
  WhisperTranscriptionRequest                        request;
  const std::unordered_map<std::string, std::string> fields = {
      {"model", "whisper-1"},
      {"response_format", "json"},
      {"include[]", "logprobs"},
  };

  auto                            error = parse_whisper_transcription_request(fields, &request);
  const WhisperApiValidationError err   = error.value_or(WhisperApiValidationError{});
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(err.param, "include[]");
  EXPECT_EQ(err.code, "unsupported_value");
}

TEST(WhisperApi, ParseRejectsInvalidTemperature) {
  WhisperTranscriptionRequest                        request;
  const std::unordered_map<std::string, std::string> fields = {
      {"model", "whisper-1"},
      {"temperature", "2.1"},
  };

  auto                            error = parse_whisper_transcription_request(fields, &request);
  const WhisperApiValidationError err   = error.value_or(WhisperApiValidationError{});
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(err.param, "temperature");
}

TEST(WhisperApi, RenderJsonResponse) {
  WhisperTranscriptionRequest request;
  request.model           = "whisper-1";
  request.response_format = WhisperResponseFormat::Json;

  WhisperTranscriptionResponsePayload payload;
  payload.text         = "hello world";
  payload.duration_sec = 1.5f;
  payload.language     = "en";

  auto rendered = render_whisper_transcription_response(request, payload);
  EXPECT_EQ(rendered.content_type, "application/json");

  auto json = nlohmann::json::parse(rendered.body);
  EXPECT_EQ(json["text"], "hello world");
}

TEST(WhisperApi, RenderTextResponse) {
  WhisperTranscriptionRequest request;
  request.model           = "whisper-1";
  request.response_format = WhisperResponseFormat::Text;

  WhisperTranscriptionResponsePayload payload;
  payload.text = "plain text";

  auto rendered = render_whisper_transcription_response(request, payload);
  EXPECT_EQ(rendered.content_type, "text/plain; charset=utf-8");
  EXPECT_EQ(rendered.body, "plain text");
}

TEST(WhisperApi, RenderSrtResponse) {
  WhisperTranscriptionRequest request;
  request.model           = "whisper-1";
  request.response_format = WhisperResponseFormat::Srt;

  WhisperTranscriptionResponsePayload payload;
  payload.text         = "subtitle";
  payload.duration_sec = 2.345f;

  auto rendered = render_whisper_transcription_response(request, payload);
  EXPECT_EQ(rendered.content_type, "application/x-subrip; charset=utf-8");
  EXPECT_NE(rendered.body.find("1\n00:00:00,000 --> 00:00:02,345"), std::string::npos);
  EXPECT_NE(rendered.body.find("subtitle"), std::string::npos);
}

TEST(WhisperApi, RenderVttResponse) {
  WhisperTranscriptionRequest request;
  request.model           = "whisper-1";
  request.response_format = WhisperResponseFormat::Vtt;

  WhisperTranscriptionResponsePayload payload;
  payload.text         = "caption";
  payload.duration_sec = 1.0f;

  auto rendered = render_whisper_transcription_response(request, payload);
  EXPECT_EQ(rendered.content_type, "text/vtt; charset=utf-8");
  EXPECT_NE(rendered.body.find("WEBVTT"), std::string::npos);
  EXPECT_NE(rendered.body.find("00:00:00.000 --> 00:00:01.000"), std::string::npos);
}

TEST(WhisperApi, RenderVerboseJsonDefaultsToSegments) {
  WhisperTranscriptionRequest request;
  request.model           = "whisper-1";
  request.response_format = WhisperResponseFormat::VerboseJson;

  WhisperTranscriptionResponsePayload payload;
  payload.text         = "hello world";
  payload.duration_sec = 3.0f;
  payload.language     = "en";

  auto rendered = render_whisper_transcription_response(request, payload);
  auto json     = nlohmann::json::parse(rendered.body);

  EXPECT_EQ(rendered.content_type, "application/json");
  EXPECT_EQ(json["task"], "transcribe");
  EXPECT_EQ(json["language"], "en");
  EXPECT_EQ(json["text"], "hello world");
  ASSERT_TRUE(json.contains("segments"));
  ASSERT_EQ(json["segments"].size(), 1);
}

TEST(WhisperApi, RenderVerboseJsonWordsAndSegments) {
  WhisperTranscriptionRequest request;
  request.model                   = "whisper-1";
  request.response_format         = WhisperResponseFormat::VerboseJson;
  request.timestamp_granularities = {WhisperTimestampGranularity::Word, WhisperTimestampGranularity::Segment};

  WhisperTranscriptionResponsePayload payload;
  payload.text         = "one two";
  payload.duration_sec = 2.0f;
  payload.language     = "en";

  auto rendered = render_whisper_transcription_response(request, payload);
  auto json     = nlohmann::json::parse(rendered.body);

  ASSERT_TRUE(json.contains("words"));
  ASSERT_TRUE(json.contains("segments"));
  ASSERT_EQ(json["words"].size(), 2);
  EXPECT_EQ(json["words"][0]["word"], "one");
}

TEST(WhisperApi, BuildErrorJson) {
  auto body = build_whisper_api_error_json("bad request", "invalid_request_error", "model", "missing_field");
  auto json = nlohmann::json::parse(body);
  ASSERT_TRUE(json.contains("error"));
  EXPECT_EQ(json["error"]["message"], "bad request");
  EXPECT_EQ(json["error"]["type"], "invalid_request_error");
  EXPECT_EQ(json["error"]["param"], "model");
  EXPECT_EQ(json["error"]["code"], "missing_field");
}

}  // namespace
}  // namespace asr
