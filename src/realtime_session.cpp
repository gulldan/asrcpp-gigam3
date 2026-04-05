#include "asr/realtime_session.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string_view>
#include <utility>

#include "asr/config.h"
#include "asr/json_utils.h"
#include "asr/string_utils.h"
#include "nlohmann/json.hpp"

namespace asr {

namespace {

void set_error(std::string* out, std::string message) {
  if (out != nullptr) {
    *out = std::move(message);
  }
}

std::string event_speech_started_json(std::string_view event_id, std::string_view item_id,
                                      int64_t audio_start_ms) {
  std::string out;
  out.reserve(128);
  out.append(R"({"type":"input_audio_buffer.speech_started","event_id":")");
  out.append(event_id);
  out.append(R"(","audio_start_ms":)");
  append_decimal(out, audio_start_ms);
  out.append(R"(,"item_id":")");
  out.append(item_id);
  out.append("\"}");
  return out;
}

std::string event_speech_stopped_json(std::string_view event_id, std::string_view item_id,
                                      int64_t audio_end_ms) {
  std::string out;
  out.reserve(128);
  out.append(R"({"type":"input_audio_buffer.speech_stopped","event_id":")");
  out.append(event_id);
  out.append(R"(","audio_end_ms":)");
  append_decimal(out, audio_end_ms);
  out.append(R"(,"item_id":")");
  out.append(item_id);
  out.append("\"}");
  return out;
}

std::string event_buffer_committed_json(std::string_view event_id, const RealtimeCommittedItem& commit) {
  std::string out;
  out.reserve(160);
  out.append(R"({"type":"input_audio_buffer.committed","event_id":")");
  out.append(event_id);
  out.append(R"(","item_id":")");
  out.append(commit.item_id);
  out.append(R"(","previous_item_id":)");
  if (commit.previous_item_id.empty()) {
    out.append("null");
  } else {
    out.push_back('"');
    out.append(commit.previous_item_id);
    out.push_back('"');
  }
  out.push_back('}');
  return out;
}

std::string event_buffer_cleared_json(std::string_view event_id) {
  std::string out;
  out.reserve(96);
  out.append(R"({"type":"input_audio_buffer.cleared","event_id":")");
  out.append(event_id);
  out.append("\"}");
  return out;
}

std::string event_transcription_completed_json(std::string_view event_id, std::string_view item_id,
                                               std::string_view transcript) {
  std::string out;
  out.reserve(160 + transcript.size());
  out.append(R"({"type":"conversation.item.input_audio_transcription.completed","event_id":")");
  out.append(event_id);
  out.append(R"(","item_id":")");
  out.append(item_id);
  out.append(R"(","content_index":0,"transcript":")");
  append_json_escaped(out, transcript);
  out.append("\"}");
  return out;
}

bool is_supported_input_audio_format(const std::string& format) {
  return format == "pcm16" || format == "opus" || format == "opus_raw" || format == "opus_rtp";
}

bool is_opus_input_audio_format(const std::string& format) {
  return format == "opus" || format == "opus_raw" || format == "opus_rtp";
}

bool is_supported_opus_output_sample_rate(int sample_rate) {
  return sample_rate == 8000 || sample_rate == 12000 || sample_rate == 16000 || sample_rate == 24000 ||
         sample_rate == 48000;
}

}  // namespace

RealtimeSessionConfig make_default_realtime_session_config(const Config& config) {
  RealtimeSessionConfig out;
  out.input_audio_format                 = "pcm16";
  out.input_sample_rate                  = config.sample_rate;
  out.input_audio_transcription.model    = "default";
  out.input_audio_transcription.language = "";
  out.input_audio_transcription.prompt   = "";
  out.turn_detection                     = RealtimeSessionConfig::TurnDetection{};
  if (out.turn_detection.has_value()) {
    out.turn_detection->type                = "server_vad";
    out.turn_detection->threshold           = config.vad_threshold;
    out.turn_detection->prefix_padding_ms   = 300;
    out.turn_detection->silence_duration_ms = static_cast<int>(config.vad_min_silence * 1000.0F);
  }
  return out;
}

VadConfig make_realtime_vad_config(const Config& base_config, const RealtimeSessionConfig& realtime_config) {
  VadConfig vad;
  vad.model_path           = base_config.vad_model;
  vad.threshold            = base_config.vad_threshold;
  vad.min_silence_duration = base_config.vad_min_silence;
  vad.min_speech_duration  = base_config.vad_min_speech;
  vad.max_speech_duration  = base_config.vad_max_speech;
  vad.sample_rate          = base_config.sample_rate;
  vad.window_size          = base_config.vad_window_size;
  vad.context_size         = base_config.vad_context_size;

  if (realtime_config.turn_detection.has_value()) {
    vad.threshold         = std::clamp(realtime_config.turn_detection->threshold, 0.01F, 0.99F);
    vad.prefix_padding_ms = std::max(0, realtime_config.turn_detection->prefix_padding_ms);
    vad.min_silence_duration =
        std::max(0.01F, static_cast<float>(realtime_config.turn_detection->silence_duration_ms) / 1000.0F);
  }

  return vad;
}

RealtimeSession::RealtimeSession(uint64_t connection_id, RealtimeSessionConfig config)
    : session_id_("sess_" + std::to_string(connection_id)), config_(std::move(config)) {}

const RealtimeSessionConfig& RealtimeSession::config() const {
  return config_;
}

void RealtimeSession::set_config(const RealtimeSessionConfig& config) {
  config_ = config;
}

bool RealtimeSession::apply_session_update(const nlohmann::json& update, std::string* error_message) {
  if (!update.is_object()) {
    set_error(error_message, "session must be an object");
    return false;
  }

  auto       next              = config_;
  const bool has_format_update = update.contains("input_audio_format");
  const bool has_rate_update   = update.contains("input_sample_rate");

  if (update.contains("input_audio_format")) {
    if (!update["input_audio_format"].is_string()) {
      set_error(error_message, "session.input_audio_format must be a string");
      return false;
    }
    auto format = to_lower_ascii(update["input_audio_format"].get<std::string>());
    if (!is_supported_input_audio_format(format)) {
      set_error(error_message, "unsupported session.input_audio_format: " + format);
      return false;
    }
    next.input_audio_format = std::move(format);
  }

  if (update.contains("input_sample_rate")) {
    if (!update["input_sample_rate"].is_number_integer()) {
      set_error(error_message, "session.input_sample_rate must be an integer");
      return false;
    }
    const int rate = update["input_sample_rate"].get<int>();
    if (rate < 8000 || rate > 192000) {
      set_error(error_message, "session.input_sample_rate must be in range [8000, 192000]");
      return false;
    }
    next.input_sample_rate = rate;
  }

  if (is_opus_input_audio_format(next.input_audio_format)) {
    if (has_format_update && !has_rate_update && !is_opus_input_audio_format(config_.input_audio_format)) {
      // RFC 7587 uses 48k RTP timestamp clock. Keep this as default for Opus sessions.
      next.input_sample_rate = 48000;
    }
    if (!is_supported_opus_output_sample_rate(next.input_sample_rate)) {
      set_error(error_message,
                "session.input_sample_rate for opus* must be one of: 8000, 12000, 16000, 24000, 48000");
      return false;
    }
  }

  if (update.contains("model")) {
    if (!update["model"].is_string()) {
      set_error(error_message, "session.model must be a string");
      return false;
    }
    next.input_audio_transcription.model = trim_ascii(update["model"].get<std::string>());
  }

  if (update.contains("input_audio_transcription")) {
    const auto& tr = update["input_audio_transcription"];
    if (!tr.is_object()) {
      set_error(error_message, "session.input_audio_transcription must be an object");
      return false;
    }
    if (tr.contains("model")) {
      if (!tr["model"].is_string()) {
        set_error(error_message, "session.input_audio_transcription.model must be a string");
        return false;
      }
      next.input_audio_transcription.model = trim_ascii(tr["model"].get<std::string>());
    }
    if (tr.contains("language")) {
      if (!tr["language"].is_string()) {
        set_error(error_message, "session.input_audio_transcription.language must be a string");
        return false;
      }
      next.input_audio_transcription.language = trim_ascii(tr["language"].get<std::string>());
    }
    if (tr.contains("prompt")) {
      if (!tr["prompt"].is_string()) {
        set_error(error_message, "session.input_audio_transcription.prompt must be a string");
        return false;
      }
      next.input_audio_transcription.prompt = trim_ascii(tr["prompt"].get<std::string>());
    }
  }

  if (update.contains("turn_detection")) {
    const auto& td = update["turn_detection"];
    if (td.is_null()) {
      next.turn_detection.reset();
    } else {
      if (!td.is_object()) {
        set_error(error_message, "session.turn_detection must be an object or null");
        return false;
      }
      if (!next.turn_detection.has_value()) {
        next.turn_detection = RealtimeSessionConfig::TurnDetection{};
      }

      auto td_next = *next.turn_detection;
      if (td.contains("type")) {
        if (!td["type"].is_string()) {
          set_error(error_message, "session.turn_detection.type must be a string");
          return false;
        }
        const auto type = to_lower_ascii(td["type"].get<std::string>());
        if (type == "server_vad") {
          td_next.type = type;
        } else if (type == "null" || type == "none") {
          next.turn_detection.reset();
        } else {
          set_error(error_message, "session.turn_detection.type must be 'server_vad' or null");
          return false;
        }
      }

      if (next.turn_detection.has_value()) {
        if (td.contains("threshold")) {
          if (!td["threshold"].is_number()) {
            set_error(error_message, "session.turn_detection.threshold must be numeric");
            return false;
          }
          td_next.threshold = td["threshold"].get<float>();
        }
        if (td.contains("prefix_padding_ms")) {
          if (!td["prefix_padding_ms"].is_number_integer()) {
            set_error(error_message, "session.turn_detection.prefix_padding_ms must be integer");
            return false;
          }
          td_next.prefix_padding_ms = td["prefix_padding_ms"].get<int>();
        }
        if (td.contains("silence_duration_ms")) {
          if (!td["silence_duration_ms"].is_number_integer()) {
            set_error(error_message, "session.turn_detection.silence_duration_ms must be integer");
            return false;
          }
          td_next.silence_duration_ms = td["silence_duration_ms"].get<int>();
        }
        next.turn_detection = td_next;
      }
    }
  }

  if (!next.input_audio_transcription.model.empty() && next.input_audio_transcription.model != "default") {
    set_error(error_message, "session.input_audio_transcription.model currently supports only 'default'");
    return false;
  }
  if (!next.input_audio_transcription.language.empty()) {
    set_error(error_message, "session.input_audio_transcription.language is not supported by this backend");
    return false;
  }
  if (!next.input_audio_transcription.prompt.empty()) {
    set_error(error_message, "session.input_audio_transcription.prompt is not supported by this backend");
    return false;
  }

  config_ = std::move(next);
  return true;
}

std::string RealtimeSession::ensure_current_item_id() {
  if (current_item_id_.empty()) {
    current_item_id_ = next_item_id();
  }
  return current_item_id_;
}

RealtimeCommittedItem RealtimeSession::commit_current_item() {
  RealtimeCommittedItem out;
  out.item_id          = ensure_current_item_id();
  out.previous_item_id = previous_item_id_;
  previous_item_id_    = out.item_id;
  current_item_id_.clear();
  return out;
}

void RealtimeSession::clear_current_item() {
  current_item_id_.clear();
}

nlohmann::json RealtimeSession::session_json() const {
  nlohmann::json session;
  session["id"]                 = session_id_;
  session["object"]             = "realtime.transcription_session";
  session["model"]              = config_.input_audio_transcription.model;
  session["input_audio_format"] = config_.input_audio_format;
  session["input_sample_rate"]  = config_.input_sample_rate;

  nlohmann::json transcription;
  transcription["model"] = config_.input_audio_transcription.model;
  if (!config_.input_audio_transcription.language.empty()) {
    transcription["language"] = config_.input_audio_transcription.language;
  }
  if (!config_.input_audio_transcription.prompt.empty()) {
    transcription["prompt"] = config_.input_audio_transcription.prompt;
  }
  session["input_audio_transcription"] = std::move(transcription);

  if (config_.turn_detection.has_value()) {
    nlohmann::json turn_detection;
    turn_detection["type"]                = config_.turn_detection->type;
    turn_detection["threshold"]           = config_.turn_detection->threshold;
    turn_detection["prefix_padding_ms"]   = config_.turn_detection->prefix_padding_ms;
    turn_detection["silence_duration_ms"] = config_.turn_detection->silence_duration_ms;
    session["turn_detection"]             = std::move(turn_detection);
  } else {
    session["turn_detection"] = nullptr;
  }

  return session;
}

std::string RealtimeSession::event_session_created() {
  nlohmann::json event;
  event["type"]     = "session.created";
  event["event_id"] = next_event_id();
  event["session"]  = session_json();
  return event.dump();
}

std::string RealtimeSession::event_session_updated() {
  nlohmann::json event;
  event["type"]     = "transcription_session.updated";
  event["event_id"] = next_event_id();
  event["session"]  = session_json();
  return event.dump();
}

std::string RealtimeSession::event_speech_started(int64_t audio_start_ms) {
  const auto event_id = next_event_id();
  const auto item_id  = ensure_current_item_id();
  return event_speech_started_json(event_id, item_id, audio_start_ms);
}

std::string RealtimeSession::event_speech_stopped(int64_t audio_end_ms) {
  const auto event_id = next_event_id();
  const auto item_id  = ensure_current_item_id();
  return event_speech_stopped_json(event_id, item_id, audio_end_ms);
}

std::string RealtimeSession::event_buffer_committed(const RealtimeCommittedItem& commit) {
  const auto event_id = next_event_id();
  return event_buffer_committed_json(event_id, commit);
}

std::string RealtimeSession::event_buffer_cleared() {
  const auto event_id = next_event_id();
  return event_buffer_cleared_json(event_id);
}

std::string RealtimeSession::event_transcription_delta(const std::string& item_id, const std::string& delta) {
  nlohmann::json event;
  event["type"]          = "conversation.item.input_audio_transcription.delta";
  event["event_id"]      = next_event_id();
  event["item_id"]       = item_id;
  event["content_index"] = 0;
  event["delta"]         = delta;
  return event.dump();
}

std::string RealtimeSession::event_transcription_completed(const std::string& item_id,
                                                           const std::string& transcript) {
  const auto event_id = next_event_id();
  return event_transcription_completed_json(event_id, item_id, transcript);
}

std::string RealtimeSession::event_error(const std::string& code, const std::string& message,
                                         const std::string& param, const std::string& client_event_id) {
  nlohmann::json err;
  err["type"]     = "invalid_request_error";
  err["code"]     = code;
  err["message"]  = message;
  err["param"]    = param.empty() ? nlohmann::json(nullptr) : nlohmann::json(param);
  err["event_id"] = client_event_id.empty() ? nlohmann::json(nullptr) : nlohmann::json(client_event_id);

  nlohmann::json event;
  event["type"]     = "error";
  event["event_id"] = next_event_id();
  event["error"]    = std::move(err);
  return event.dump();
}

std::string RealtimeSession::next_event_id() {
  ++event_seq_;
  return "evt_" + std::to_string(event_seq_);
}

std::string RealtimeSession::next_item_id() {
  ++item_seq_;
  return "item_" + std::to_string(item_seq_);
}

}  // namespace asr
