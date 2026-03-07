#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace asr {

enum class WhisperResponseFormat {
  Json,
  Text,
  Srt,
  VerboseJson,
  Vtt,
};

enum class WhisperTimestampGranularity {
  Word,
  Segment,
};

struct WhisperTranscriptionRequest {
  std::string                              model;
  std::string                              language;
  std::string                              prompt;
  WhisperResponseFormat                    response_format = WhisperResponseFormat::Json;
  std::vector<WhisperTimestampGranularity> timestamp_granularities;
  std::optional<double>                    temperature;
  bool                                     stream           = false;
  bool                                     include_logprobs = false;
};

struct WhisperApiValidationError {
  std::string message;
  std::string type = "invalid_request_error";
  std::string param;
  std::string code = "invalid_value";
};

struct WhisperTranscriptionResponsePayload {
  std::string text;
  float       duration_sec = 0.0f;
  std::string language;
};

struct WhisperRenderedResponse {
  std::string body;
  std::string content_type;
};

std::optional<WhisperApiValidationError> parse_whisper_transcription_request(
    const std::unordered_map<std::string, std::string>& form_fields, WhisperTranscriptionRequest* out);

WhisperRenderedResponse render_whisper_transcription_response(
    const WhisperTranscriptionRequest& request, const WhisperTranscriptionResponsePayload& payload);

std::string build_whisper_api_error_json(std::string_view message, std::string_view type,
                                         std::string_view param = {}, std::string_view code = {});

}  // namespace asr
