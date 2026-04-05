#include "asr/whisper_api.h"

#include <math.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "asr/string_utils.h"
#include "nlohmann/detail/json_ref.hpp"

namespace asr {
namespace {

bool contains_timestamp_granularity(const std::vector<WhisperTimestampGranularity>& values,
                                    WhisperTimestampGranularity                     needle) {
  return std::find(values.begin(), values.end(), needle) != values.end();
}

void append_timestamp_granularity_unique(std::vector<WhisperTimestampGranularity>* values,
                                         WhisperTimestampGranularity               value) {
  if (values == nullptr) {
    return;
  }
  if (!contains_timestamp_granularity(*values, value)) {
    values->push_back(value);
  }
}

std::vector<std::string> parse_multi_value_field(std::string_view raw) {
  std::string normalized(raw);
  std::replace_if(
      normalized.begin(), normalized.end(),
      [](char ch) { return ch == '[' || ch == ']' || ch == '"' || ch == '\''; }, ' ');

  std::vector<std::string> values;
  std::stringstream        ss(normalized);
  std::string              part;
  while (std::getline(ss, part, ',')) {
    auto trimmed = trim_ascii(part);
    if (trimmed.empty()) {
      continue;
    }

    std::stringstream words(trimmed);
    std::string       token;
    while (words >> token) {
      values.push_back(token);
    }
  }

  if (values.empty()) {
    auto fallback = trim_ascii(raw);
    if (!fallback.empty()) {
      values.push_back(std::move(fallback));
    }
  }
  return values;
}

std::optional<bool> parse_bool_field(std::string_view raw) {
  const auto value = to_lower_ascii(trim_ascii(raw));
  if (value == "true" || value == "1") {
    return true;
  }
  if (value == "false" || value == "0") {
    return false;
  }
  return std::nullopt;
}

std::optional<WhisperResponseFormat> parse_response_format(std::string_view raw) {
  const auto value = to_lower_ascii(trim_ascii(raw));
  if (value == "json") {
    return WhisperResponseFormat::Json;
  }
  if (value == "text") {
    return WhisperResponseFormat::Text;
  }
  if (value == "srt") {
    return WhisperResponseFormat::Srt;
  }
  if (value == "verbose_json") {
    return WhisperResponseFormat::VerboseJson;
  }
  if (value == "vtt") {
    return WhisperResponseFormat::Vtt;
  }
  return std::nullopt;
}

std::optional<WhisperTimestampGranularity> parse_timestamp_granularity(std::string_view raw) {
  const auto value = to_lower_ascii(trim_ascii(raw));
  if (value == "word") {
    return WhisperTimestampGranularity::Word;
  }
  if (value == "segment") {
    return WhisperTimestampGranularity::Segment;
  }
  return std::nullopt;
}

bool is_supported_whisper_model_alias(std::string_view model) {
  const auto normalized = to_lower_ascii(trim_ascii(model));
  return normalized == "whisper-1" || normalized == "gpt-4o-mini-transcribe" ||
         normalized == "whisper-large-v3-turbo" || normalized == "default" || normalized == "gigaam-v3";
}

bool is_supported_whisper_language_hint(std::string_view language) {
  if (trim_ascii(language).empty()) {
    return true;
  }
  const auto normalized = to_lower_ascii(trim_ascii(language));
  return normalized == "ru" || normalized == "ru-ru" || normalized == "russian";
}

std::string format_timestamp(double seconds, char decimal_separator) {
  const auto clamped_seconds = std::max(0.0, seconds);
  auto       total_ms        = std::llround(clamped_seconds * 1000.0);
  total_ms                   = std::max(total_ms, 0LL);

  const auto ms      = static_cast<int>(total_ms % 1000);
  const auto total_s = total_ms / 1000;
  const auto s       = static_cast<int>(total_s % 60);
  const auto total_m = total_s / 60;
  const auto m       = static_cast<int>(total_m % 60);
  const auto h       = static_cast<int>(total_m / 60);

  std::ostringstream out;
  out << std::setfill('0') << std::setw(2) << h << ':' << std::setw(2) << m << ':' << std::setw(2) << s
      << decimal_separator << std::setw(3) << ms;
  return out.str();
}

nlohmann::json build_verbose_segment(const WhisperTranscriptionResponsePayload& payload) {
  nlohmann::json segment;
  segment["id"]                = 0;
  segment["seek"]              = 0;
  segment["start"]             = 0.0;
  segment["end"]               = std::max(0.0f, payload.duration_sec);
  segment["text"]              = payload.text;
  segment["tokens"]            = nlohmann::json::array();
  segment["temperature"]       = 0.0;
  segment["avg_logprob"]       = 0.0;
  segment["compression_ratio"] = 1.0;
  segment["no_speech_prob"]    = 0.0;
  return segment;
}

const std::string* find_field(const std::unordered_map<std::string, std::string>& form_fields,
                              std::string_view                                    key) {
  auto it = form_fields.find(std::string(key));
  if (it == form_fields.end()) {
    return nullptr;
  }
  return &it->second;
}

WhisperApiValidationError make_validation_error(std::string message, std::string param,
                                                std::string code = "invalid_value") {
  WhisperApiValidationError error;
  error.message = std::move(message);
  error.param   = std::move(param);
  error.code    = std::move(code);
  return error;
}

}  // namespace

std::optional<WhisperApiValidationError> parse_whisper_transcription_request(
    const std::unordered_map<std::string, std::string>& form_fields, WhisperTranscriptionRequest* out) {
  if (out == nullptr) {
    WhisperApiValidationError error;
    error.message = "Internal error: output request pointer is null";
    error.type    = "server_error";
    error.code    = "internal_error";
    return error;
  }

  WhisperTranscriptionRequest req;

  const std::string* model_field = find_field(form_fields, "model");
  if (model_field == nullptr || trim_ascii(*model_field).empty()) {
    return make_validation_error("Missing required field 'model'", "model", "missing_field");
  }
  req.model = trim_ascii(*model_field);
  if (!is_supported_whisper_model_alias(req.model)) {
    return make_validation_error(
        "Unsupported 'model'. Supported aliases: whisper-1, gpt-4o-mini-transcribe, "
        "whisper-large-v3-turbo, default, gigaam-v3",
        "model", "unsupported_value");
  }

  if (const auto* language = find_field(form_fields, "language"); language != nullptr) {
    req.language = trim_ascii(*language);
    if (!is_supported_whisper_language_hint(req.language)) {
      return make_validation_error("Only Russian language hint is supported by this backend", "language",
                                   "unsupported_value");
    }
  }

  if (const auto* prompt = find_field(form_fields, "prompt"); prompt != nullptr) {
    req.prompt = trim_ascii(*prompt);
    if (!req.prompt.empty()) {
      return make_validation_error("'prompt' is not supported by this backend", "prompt",
                                   "unsupported_value");
    }
  }

  if (const auto* response_format = find_field(form_fields, "response_format");
      response_format != nullptr && !trim_ascii(*response_format).empty()) {
    auto parsed = parse_response_format(*response_format);
    if (!parsed.has_value()) {
      return make_validation_error("Invalid 'response_format'. Supported: json, text, srt, verbose_json, vtt",
                                   "response_format");
    }
    req.response_format = *parsed;
  }

  if (const auto* temperature = find_field(form_fields, "temperature");
      temperature != nullptr && !trim_ascii(*temperature).empty()) {
    try {
      req.temperature = std::stod(trim_ascii(*temperature));
    } catch (const std::exception&) {
      return make_validation_error("Invalid numeric value for 'temperature'", "temperature");
    }
    if (!std::isfinite(*req.temperature) || *req.temperature < 0.0 || *req.temperature > 1.0) {
      return make_validation_error("'temperature' must be in range [0, 1]", "temperature");
    }
    if (*req.temperature != 0.0) {
      return make_validation_error("Only temperature=0 is supported by this backend", "temperature",
                                   "unsupported_value");
    }
  }

  if (const auto* stream_field = find_field(form_fields, "stream");
      stream_field != nullptr && !trim_ascii(*stream_field).empty()) {
    auto parsed = parse_bool_field(*stream_field);
    if (!parsed.has_value()) {
      return make_validation_error("Invalid boolean value for 'stream'", "stream");
    }
    req.stream = *parsed;
    if (req.stream) {
      return make_validation_error("'stream=true' is not supported by this backend", "stream",
                                   "unsupported_value");
    }
  }

  if (const auto* include = find_field(form_fields, "include[]");
      include != nullptr && !trim_ascii(*include).empty()) {
    for (const auto& token : parse_multi_value_field(*include)) {
      const auto lowered = to_lower_ascii(token);
      if (lowered == "logprobs") {
        req.include_logprobs = true;
        continue;
      }
      return make_validation_error("Unsupported value in 'include[]': " + token, "include[]");
    }
  }

  if (const auto* include = find_field(form_fields, "include");
      include != nullptr && !trim_ascii(*include).empty()) {
    for (const auto& token : parse_multi_value_field(*include)) {
      const auto lowered = to_lower_ascii(token);
      if (lowered == "logprobs") {
        req.include_logprobs = true;
        continue;
      }
      return make_validation_error("Unsupported value in 'include': " + token, "include");
    }
  }

  if (const auto* granularities = find_field(form_fields, "timestamp_granularities[]");
      granularities != nullptr && !trim_ascii(*granularities).empty()) {
    for (const auto& token : parse_multi_value_field(*granularities)) {
      auto parsed = parse_timestamp_granularity(token);
      if (!parsed.has_value()) {
        return make_validation_error("Invalid 'timestamp_granularities[]'. Supported: word, segment",
                                     "timestamp_granularities[]");
      }
      if (*parsed == WhisperTimestampGranularity::Word) {
        return make_validation_error(
            "'timestamp_granularities[]=word' is not supported by this backend; use 'segment' instead",
            "timestamp_granularities[]", "unsupported_value");
      }
      append_timestamp_granularity_unique(&req.timestamp_granularities, *parsed);
    }
  }

  if (const auto* granularities = find_field(form_fields, "timestamp_granularities");
      granularities != nullptr && !trim_ascii(*granularities).empty()) {
    for (const auto& token : parse_multi_value_field(*granularities)) {
      auto parsed = parse_timestamp_granularity(token);
      if (!parsed.has_value()) {
        return make_validation_error("Invalid 'timestamp_granularities'. Supported: word, segment",
                                     "timestamp_granularities");
      }
      if (*parsed == WhisperTimestampGranularity::Word) {
        return make_validation_error(
            "'timestamp_granularities=word' is not supported by this backend; use 'segment' instead",
            "timestamp_granularities", "unsupported_value");
      }
      append_timestamp_granularity_unique(&req.timestamp_granularities, *parsed);
    }
  }

  if (!req.timestamp_granularities.empty() && req.response_format != WhisperResponseFormat::VerboseJson) {
    return make_validation_error("'timestamp_granularities[]' requires response_format='verbose_json'",
                                 "timestamp_granularities[]");
  }

  if (req.include_logprobs) {
    return make_validation_error("'include[]=logprobs' is not supported by this server", "include[]",
                                 "unsupported_value");
  }

  *out = std::move(req);
  return std::nullopt;
}

WhisperRenderedResponse render_whisper_transcription_response(
    const WhisperTranscriptionRequest& request, const WhisperTranscriptionResponsePayload& payload) {
  WhisperRenderedResponse rendered;

  if (request.response_format == WhisperResponseFormat::Json) {
    nlohmann::json j;
    j["text"]             = payload.text;
    rendered.body         = j.dump();
    rendered.content_type = "application/json";
    return rendered;
  }

  if (request.response_format == WhisperResponseFormat::Text) {
    rendered.body         = payload.text;
    rendered.content_type = "text/plain; charset=utf-8";
    return rendered;
  }

  const double duration = std::max(0.0f, payload.duration_sec);
  if (request.response_format == WhisperResponseFormat::Srt) {
    std::ostringstream out;
    out << "1\n"
        << format_timestamp(0.0, ',') << " --> " << format_timestamp(duration, ',') << "\n"
        << payload.text << "\n";
    rendered.body         = out.str();
    rendered.content_type = "application/x-subrip; charset=utf-8";
    return rendered;
  }

  if (request.response_format == WhisperResponseFormat::Vtt) {
    std::ostringstream out;
    out << "WEBVTT\n\n"
        << format_timestamp(0.0, '.') << " --> " << format_timestamp(duration, '.') << "\n"
        << payload.text << "\n";
    rendered.body         = out.str();
    rendered.content_type = "text/vtt; charset=utf-8";
    return rendered;
  }

  nlohmann::json j;
  j["task"]     = "transcribe";
  j["language"] = payload.language.empty() ? "unknown" : payload.language;
  j["duration"] = duration;
  j["text"]     = payload.text;

  auto granularities = request.timestamp_granularities;
  if (granularities.empty()) {
    granularities.push_back(WhisperTimestampGranularity::Segment);
  }

  if (contains_timestamp_granularity(granularities, WhisperTimestampGranularity::Segment)) {
    j["segments"] = nlohmann::json::array({build_verbose_segment(payload)});
  }

  rendered.body         = j.dump();
  rendered.content_type = "application/json";
  return rendered;
}

std::string build_whisper_api_error_json(std::string_view message, std::string_view type,
                                         std::string_view param, std::string_view code) {
  nlohmann::json err;
  err["error"]            = nlohmann::json::object();
  err["error"]["message"] = std::string(message);
  err["error"]["type"]    = std::string(type);
  if (!param.empty()) {
    err["error"]["param"] = std::string(param);
  } else {
    err["error"]["param"] = nullptr;
  }
  if (!code.empty()) {
    err["error"]["code"] = std::string(code);
  } else {
    err["error"]["code"] = nullptr;
  }
  return err.dump();
}

}  // namespace asr
