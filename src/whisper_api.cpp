#include "asr/whisper_api.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace asr {
namespace {

std::string trim_copy(std::string_view input) {
  size_t begin = 0;
  while (begin < input.size() && std::isspace(static_cast<unsigned char>(input[begin])) != 0) {
    ++begin;
  }

  size_t end = input.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
    --end;
  }
  return std::string(input.substr(begin, end - begin));
}

std::string to_lower_ascii(std::string_view input) {
  std::string out(input.begin(), input.end());
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return out;
}

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
    auto trimmed = trim_copy(part);
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
    auto fallback = trim_copy(raw);
    if (!fallback.empty()) {
      values.push_back(std::move(fallback));
    }
  }
  return values;
}

std::optional<bool> parse_bool_field(std::string_view raw) {
  const auto value = to_lower_ascii(trim_copy(raw));
  if (value == "true" || value == "1") {
    return true;
  }
  if (value == "false" || value == "0") {
    return false;
  }
  return std::nullopt;
}

std::optional<WhisperResponseFormat> parse_response_format(std::string_view raw) {
  const auto value = to_lower_ascii(trim_copy(raw));
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
  const auto value = to_lower_ascii(trim_copy(raw));
  if (value == "word") {
    return WhisperTimestampGranularity::Word;
  }
  if (value == "segment") {
    return WhisperTimestampGranularity::Segment;
  }
  return std::nullopt;
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

nlohmann::json build_verbose_words(const WhisperTranscriptionResponsePayload& payload) {
  nlohmann::json           words_json = nlohmann::json::array();
  std::vector<std::string> words;
  {
    std::istringstream text_stream(payload.text);
    std::string        word;
    while (text_stream >> word) {
      words.push_back(word);
    }
  }

  if (words.empty()) {
    return words_json;
  }

  const double duration = std::max(0.0f, payload.duration_sec);
  const double step     = duration / static_cast<double>(words.size());

  for (size_t i = 0; i < words.size(); ++i) {
    const double start = static_cast<double>(i) * step;
    const double end   = (i + 1 == words.size()) ? duration : static_cast<double>(i + 1) * step;
    words_json.push_back({
        {"word", words[i]},
        {"start", start},
        {"end", end},
    });
  }
  return words_json;
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
  if (model_field == nullptr || trim_copy(*model_field).empty()) {
    return make_validation_error("Missing required field 'model'", "model", "missing_field");
  }
  req.model = trim_copy(*model_field);

  if (const auto* language = find_field(form_fields, "language"); language != nullptr) {
    req.language = trim_copy(*language);
  }

  if (const auto* prompt = find_field(form_fields, "prompt"); prompt != nullptr) {
    req.prompt = trim_copy(*prompt);
  }

  if (const auto* response_format = find_field(form_fields, "response_format");
      response_format != nullptr && !trim_copy(*response_format).empty()) {
    auto parsed = parse_response_format(*response_format);
    if (!parsed.has_value()) {
      return make_validation_error("Invalid 'response_format'. Supported: json, text, srt, verbose_json, vtt",
                                   "response_format");
    }
    req.response_format = *parsed;
  }

  if (const auto* temperature = find_field(form_fields, "temperature");
      temperature != nullptr && !trim_copy(*temperature).empty()) {
    try {
      req.temperature = std::stod(trim_copy(*temperature));
    } catch (const std::exception&) {
      return make_validation_error("Invalid numeric value for 'temperature'", "temperature");
    }
    if (!std::isfinite(*req.temperature) || *req.temperature < 0.0 || *req.temperature > 1.0) {
      return make_validation_error("'temperature' must be in range [0, 1]", "temperature");
    }
  }

  if (const auto* stream_field = find_field(form_fields, "stream");
      stream_field != nullptr && !trim_copy(*stream_field).empty()) {
    auto parsed = parse_bool_field(*stream_field);
    if (!parsed.has_value()) {
      return make_validation_error("Invalid boolean value for 'stream'", "stream");
    }
    req.stream = *parsed;
  }

  if (const auto* include = find_field(form_fields, "include[]");
      include != nullptr && !trim_copy(*include).empty()) {
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
      include != nullptr && !trim_copy(*include).empty()) {
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
      granularities != nullptr && !trim_copy(*granularities).empty()) {
    for (const auto& token : parse_multi_value_field(*granularities)) {
      auto parsed = parse_timestamp_granularity(token);
      if (!parsed.has_value()) {
        return make_validation_error("Invalid 'timestamp_granularities[]'. Supported: word, segment",
                                     "timestamp_granularities[]");
      }
      append_timestamp_granularity_unique(&req.timestamp_granularities, *parsed);
    }
  }

  if (const auto* granularities = find_field(form_fields, "timestamp_granularities");
      granularities != nullptr && !trim_copy(*granularities).empty()) {
    for (const auto& token : parse_multi_value_field(*granularities)) {
      auto parsed = parse_timestamp_granularity(token);
      if (!parsed.has_value()) {
        return make_validation_error("Invalid 'timestamp_granularities'. Supported: word, segment",
                                     "timestamp_granularities");
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

  if (contains_timestamp_granularity(granularities, WhisperTimestampGranularity::Word)) {
    j["words"] = build_verbose_words(payload);
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
