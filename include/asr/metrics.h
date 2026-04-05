#pragma once

#include <prometheus/registry.h>

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace prometheus {
class Counter;
class Gauge;
class Histogram;
template <typename T>
class Family;
}  // namespace prometheus

namespace asr {

class ASRMetrics {
 public:
  static ASRMetrics& instance();

  void        initialize();
  static void shutdown();

  // Pipeline metrics
  void observe_ttfr(double sec, const std::string& mode);
  void observe_segment(double audio_sec, double decode_sec);
  void observe_request(double total_sec, double audio_sec, double decode_sec, int chunk_count,
                       size_t bytes_count, double preprocess_sec, double io_sec, const std::string& mode,
                       const std::string& status);
  void observe_recognizer_wait(double sec, bool timed_out);
  void observe_error(const std::string& error_type);

  // Connection metrics
  void connection_opened();
  void connection_closed(const std::string& reason, double duration_sec);
  void session_started();
  void session_ended(double duration_sec = 0.0);

  // Recognition metrics
  void record_result(std::string_view text);
  void record_audio_level(double rms);
  void record_silence();
  void set_speech_ratio(double ratio);

  std::shared_ptr<prometheus::Registry> registry();

 private:
  ASRMetrics();

  std::once_flag                        init_flag_;
  bool                                  initialized_ = false;
  std::shared_ptr<prometheus::Registry> registry_;

  // ===== Pipeline Metrics =====
  // Histograms (families)
  prometheus::Family<prometheus::Histogram>* ttfr_family_                = nullptr;
  prometheus::Family<prometheus::Histogram>* rtf_family_                 = nullptr;
  prometheus::Family<prometheus::Histogram>* rtf_decode_family_          = nullptr;
  prometheus::Family<prometheus::Histogram>* request_duration_family_    = nullptr;
  prometheus::Family<prometheus::Histogram>* decode_duration_family_     = nullptr;
  prometheus::Family<prometheus::Histogram>* audio_duration_family_      = nullptr;
  prometheus::Family<prometheus::Histogram>* segment_duration_family_    = nullptr;
  prometheus::Family<prometheus::Histogram>* preprocess_duration_family_ = nullptr;
  prometheus::Family<prometheus::Histogram>* io_duration_family_         = nullptr;
  prometheus::Family<prometheus::Histogram>* segment_rtf_family_         = nullptr;
  prometheus::Family<prometheus::Histogram>* recognizer_wait_family_     = nullptr;

  // Counters (families)
  prometheus::Family<prometheus::Counter>* requests_total_family_           = nullptr;
  prometheus::Family<prometheus::Counter>* segments_total_family_           = nullptr;
  prometheus::Family<prometheus::Counter>* audio_seconds_total_family_      = nullptr;
  prometheus::Family<prometheus::Counter>* errors_total_family_             = nullptr;
  prometheus::Family<prometheus::Counter>* chunks_total_family_             = nullptr;
  prometheus::Family<prometheus::Counter>* bytes_total_family_              = nullptr;
  prometheus::Family<prometheus::Counter>* recognizer_wait_timeouts_family_ = nullptr;

  // Gauges (families)
  prometheus::Family<prometheus::Gauge>* active_connections_family_ = nullptr;
  prometheus::Family<prometheus::Gauge>* current_rtf_family_        = nullptr;
  prometheus::Family<prometheus::Gauge>* current_ttfr_family_       = nullptr;
  prometheus::Family<prometheus::Gauge>* current_decode_family_     = nullptr;
  prometheus::Family<prometheus::Gauge>* current_request_family_    = nullptr;
  prometheus::Family<prometheus::Gauge>* current_audio_family_      = nullptr;
  prometheus::Family<prometheus::Gauge>* current_rtf_total_family_  = nullptr;
  prometheus::Family<prometheus::Gauge>* current_preprocess_family_ = nullptr;
  prometheus::Family<prometheus::Gauge>* current_io_family_         = nullptr;

  // ===== Connection Metrics =====
  prometheus::Family<prometheus::Histogram>* connection_duration_family_  = nullptr;
  prometheus::Family<prometheus::Histogram>* session_duration_family_     = nullptr;
  prometheus::Family<prometheus::Counter>*   connections_total_family_    = nullptr;
  prometheus::Family<prometheus::Counter>*   disconnections_total_family_ = nullptr;
  prometheus::Family<prometheus::Counter>*   sessions_total_family_       = nullptr;
  prometheus::Family<prometheus::Gauge>*     active_sessions_family_      = nullptr;

  // ===== Recognition Metrics =====
  prometheus::Family<prometheus::Histogram>* words_per_request_family_      = nullptr;
  prometheus::Family<prometheus::Histogram>* audio_rms_family_              = nullptr;
  prometheus::Family<prometheus::Counter>*   empty_results_total_family_    = nullptr;
  prometheus::Family<prometheus::Counter>*   words_total_family_            = nullptr;
  prometheus::Family<prometheus::Counter>*   characters_total_family_       = nullptr;
  prometheus::Family<prometheus::Counter>*   silence_segments_total_family_ = nullptr;
  prometheus::Family<prometheus::Counter>*   low_volume_warnings_family_    = nullptr;
  prometheus::Family<prometheus::Counter>*   detected_language_family_      = nullptr;
  prometheus::Family<prometheus::Gauge>*     speech_ratio_family_           = nullptr;

  // ===== Pre-fetched metric instances (created once in initialize()) =====
  // Histograms
  struct ModeMetricCache {
    prometheus::Histogram* ttfr                     = nullptr;
    prometheus::Counter*   requests_success         = nullptr;
    prometheus::Counter*   requests_failed          = nullptr;
    prometheus::Histogram* request_duration_success = nullptr;
    prometheus::Histogram* request_duration_failed  = nullptr;
    prometheus::Histogram* rtf                      = nullptr;
    prometheus::Histogram* rtf_decode               = nullptr;
  };
  ModeMetricCache realtime_websocket_mode_;
  ModeMetricCache http_mode_;
  ModeMetricCache whisper_api_mode_;

  prometheus::Histogram* decode_duration_     = nullptr;
  prometheus::Histogram* segment_duration_    = nullptr;
  prometheus::Histogram* segment_rtf_         = nullptr;
  prometheus::Histogram* audio_duration_      = nullptr;
  prometheus::Histogram* preprocess_duration_ = nullptr;
  prometheus::Histogram* io_duration_         = nullptr;
  prometheus::Histogram* recognizer_wait_     = nullptr;
  prometheus::Histogram* connection_duration_ = nullptr;
  prometheus::Histogram* session_duration_    = nullptr;
  prometheus::Histogram* words_per_request_   = nullptr;
  prometheus::Histogram* audio_rms_           = nullptr;

  // Counters
  prometheus::Counter* segments_total_           = nullptr;
  prometheus::Counter* audio_seconds_total_      = nullptr;
  prometheus::Counter* chunks_total_             = nullptr;
  prometheus::Counter* bytes_total_              = nullptr;
  prometheus::Counter* recognizer_wait_timeouts_ = nullptr;
  prometheus::Counter* connections_total_        = nullptr;
  prometheus::Counter* sessions_total_           = nullptr;
  prometheus::Counter* empty_results_total_      = nullptr;
  prometheus::Counter* words_total_              = nullptr;
  prometheus::Counter* characters_total_         = nullptr;
  prometheus::Counter* silence_segments_total_   = nullptr;
  prometheus::Counter* low_volume_warnings_      = nullptr;

  // Gauges
  prometheus::Gauge* active_connections_ = nullptr;
  prometheus::Gauge* active_sessions_    = nullptr;
  prometheus::Gauge* speech_ratio_       = nullptr;
  prometheus::Gauge* current_ttfr_       = nullptr;
  prometheus::Gauge* current_decode_     = nullptr;
  prometheus::Gauge* current_rtf_        = nullptr;
  prometheus::Gauge* current_rtf_total_  = nullptr;
  prometheus::Gauge* current_request_    = nullptr;
  prometheus::Gauge* current_audio_      = nullptr;
  prometheus::Gauge* current_preprocess_ = nullptr;
  prometheus::Gauge* current_io_         = nullptr;

  // ===== Pre-cached labeled instances (eliminates map<string,string> allocs) =====
  prometheus::Counter* disconnections_normal_                = nullptr;
  prometheus::Counter* disconnections_message_too_large_     = nullptr;
  prometheus::Counter* disconnections_internal_error_        = nullptr;
  prometheus::Counter* disconnections_other_                 = nullptr;
  prometheus::Counter* errors_capacity_exceeded_             = nullptr;
  prometheus::Counter* errors_empty_file_                    = nullptr;
  prometheus::Counter* errors_file_too_large_                = nullptr;
  prometheus::Counter* errors_invalid_audio_                 = nullptr;
  prometheus::Counter* errors_internal_error_                = nullptr;
  prometheus::Counter* errors_bad_multipart_                 = nullptr;
  prometheus::Counter* errors_invalid_param_                 = nullptr;
  prometheus::Counter* errors_realtime_ws_handler_exception_ = nullptr;
  prometheus::Counter* errors_other_                         = nullptr;
};

}  // namespace asr
