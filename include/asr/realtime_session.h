#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "asr/vad.h"

namespace asr {

struct Config;

struct RealtimeSessionConfig {
  std::string input_audio_format = "pcm16";  // pcm16 | opus | opus_raw | opus_rtp
  int         input_sample_rate  = 16000;

  struct InputAudioTranscription {
    std::string model    = "default";
    std::string language = "";
    std::string prompt   = "";
  } input_audio_transcription;

  struct TurnDetection {
    std::string type                = "server_vad";
    float       threshold           = 0.5F;
    int         prefix_padding_ms   = 300;
    int         silence_duration_ms = 500;
  };

  // null turn_detection disables explicit speech_started/speech_stopped events.
  std::optional<TurnDetection> turn_detection = TurnDetection{};
};

struct RealtimeCommittedItem {
  std::string item_id;
  std::string previous_item_id;
};

RealtimeSessionConfig make_default_realtime_session_config(const Config& config);
VadConfig make_realtime_vad_config(const Config& base_config, const RealtimeSessionConfig& realtime_config);

class RealtimeSession {
 public:
  explicit RealtimeSession(uint64_t connection_id, RealtimeSessionConfig config = RealtimeSessionConfig{});

  [[nodiscard]] const RealtimeSessionConfig& config() const;
  void                                       set_config(const RealtimeSessionConfig& config);
  bool apply_session_update(const nlohmann::json& update, std::string* error_message);

  [[nodiscard]] std::string ensure_current_item_id();
  RealtimeCommittedItem     commit_current_item();
  void                      clear_current_item();

  [[nodiscard]] std::string event_session_created();
  [[nodiscard]] std::string event_session_updated();
  [[nodiscard]] std::string event_speech_started(int64_t audio_start_ms);
  [[nodiscard]] std::string event_speech_stopped(int64_t audio_end_ms);
  [[nodiscard]] std::string event_buffer_committed(const RealtimeCommittedItem& commit);
  [[nodiscard]] std::string event_buffer_cleared();
  [[nodiscard]] std::string event_transcription_delta(const std::string& item_id, const std::string& delta);
  [[nodiscard]] std::string event_transcription_completed(const std::string& item_id,
                                                          const std::string& transcript);
  [[nodiscard]] std::string event_error(const std::string& code, const std::string& message,
                                        const std::string& param           = "",
                                        const std::string& client_event_id = "");

 private:
  [[nodiscard]] nlohmann::json session_json() const;
  [[nodiscard]] std::string    next_event_id();
  [[nodiscard]] std::string    next_item_id();

  std::string           session_id_;
  RealtimeSessionConfig config_;
  uint64_t              event_seq_ = 0;
  uint64_t              item_seq_  = 0;
  std::string           current_item_id_;
  std::string           previous_item_id_;
};

}  // namespace asr
