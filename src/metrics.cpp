#include "asr/metrics.h"

#include <prometheus/counter.h>
#include <prometheus/family.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <spdlog/spdlog.h>

namespace asr {

// Static bucket definitions — allocated once, reused on every observation
namespace buckets {
const prometheus::Histogram::BucketBoundaries& kTTFR() {
  static const auto kBuckets =
      prometheus::Histogram::BucketBoundaries{0.1, 0.2, 0.3, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0, 5.0, 10.0};
  return kBuckets;
}

const prometheus::Histogram::BucketBoundaries& kDecode() {
  static const auto kBuckets =
      prometheus::Histogram::BucketBoundaries{0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 5.0};
  return kBuckets;
}

const prometheus::Histogram::BucketBoundaries& kSegment() {
  static const auto kBuckets =
      prometheus::Histogram::BucketBoundaries{0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 20.0, 30.0};
  return kBuckets;
}

const prometheus::Histogram::BucketBoundaries& kRTF() {
  static const auto kBuckets =
      prometheus::Histogram::BucketBoundaries{0.05, 0.1, 0.15, 0.2, 0.3, 0.4, 0.5, 0.75, 1.0, 1.5, 2.0};
  return kBuckets;
}

const prometheus::Histogram::BucketBoundaries& kRequest() {
  static const auto kBuckets =
      prometheus::Histogram::BucketBoundaries{0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 30.0, 45.0, 60.0, 90.0, 120.0};
  return kBuckets;
}

const prometheus::Histogram::BucketBoundaries& kPreprocess() {
  static const auto kBuckets =
      prometheus::Histogram::BucketBoundaries{0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 5.0};
  return kBuckets;
}

const prometheus::Histogram::BucketBoundaries& kIO() {
  static const auto kBuckets =
      prometheus::Histogram::BucketBoundaries{0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 5.0};
  return kBuckets;
}

const prometheus::Histogram::BucketBoundaries& kQueueWait() {
  static const auto kBuckets =
      prometheus::Histogram::BucketBoundaries{0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 5.0};
  return kBuckets;
}

const prometheus::Histogram::BucketBoundaries& kAudio() {
  static const auto kBuckets =
      prometheus::Histogram::BucketBoundaries{0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 30.0, 60.0, 120.0};
  return kBuckets;
}

const prometheus::Histogram::BucketBoundaries& kConnection() {
  static const auto kBuckets =
      prometheus::Histogram::BucketBoundaries{1, 5, 10, 30, 60, 120, 300, 600, 1800, 3600};
  return kBuckets;
}

const prometheus::Histogram::BucketBoundaries& kSession() {
  static const auto kBuckets =
      prometheus::Histogram::BucketBoundaries{0.5, 1, 2, 5, 10, 20, 30, 60, 120, 300};
  return kBuckets;
}

const prometheus::Histogram::BucketBoundaries& kWords() {
  static const auto kBuckets = prometheus::Histogram::BucketBoundaries{1, 2, 5, 10, 20, 50, 100, 200};
  return kBuckets;
}

const prometheus::Histogram::BucketBoundaries& kRMS() {
  static const auto kBuckets =
      prometheus::Histogram::BucketBoundaries{0.001, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5};
  return kBuckets;
}
}  // namespace buckets

namespace {

std::string_view canonical_error_type(std::string_view error_type) {
  if (error_type == "capacity_exceeded" || error_type == "empty_file" || error_type == "file_too_large" ||
      error_type == "invalid_audio" || error_type == "internal_error" || error_type == "bad_multipart" ||
      error_type == "invalid_param" || error_type == "realtime_ws_handler_exception") {
    return error_type;
  }
  return "other";
}

std::string_view canonical_disconnect_reason(std::string_view reason) {
  if (reason == "normal" || reason == "message_too_large" || reason == "internal_error") {
    return reason;
  }
  return "other";
}

enum class MetricMode {
  RealtimeWebsocket,
  Http,
  WhisperApi,
};

MetricMode canonical_mode(std::string_view mode) {
  if (mode == "realtime_websocket") {
    return MetricMode::RealtimeWebsocket;
  }
  if (mode == "whisper_api") {
    return MetricMode::WhisperApi;
  }
  return MetricMode::Http;
}

}  // namespace

ASRMetrics& ASRMetrics::instance() {
  static ASRMetrics inst;
  return inst;
}

ASRMetrics::ASRMetrics() : registry_(std::make_shared<prometheus::Registry>()) {}

void ASRMetrics::initialize() {
  std::call_once(init_flag_, [this]() {
    // ===== Pipeline Histograms =====
    ttfr_family_                  = &prometheus::BuildHistogram()
                                         .Name("gigaam_ttfr_seconds")
                                         .Help("Time to first result")
                                         .Register(*registry_);
    realtime_websocket_mode_.ttfr = &ttfr_family_->Add({{"mode", "realtime_websocket"}}, buckets::kTTFR());
    http_mode_.ttfr               = &ttfr_family_->Add({{"mode", "http"}}, buckets::kTTFR());
    whisper_api_mode_.ttfr        = &ttfr_family_->Add({{"mode", "whisper_api"}}, buckets::kTTFR());

    rtf_family_ =
        &prometheus::BuildHistogram().Name("gigaam_rtf").Help("Real-time factor").Register(*registry_);

    rtf_decode_family_ = &prometheus::BuildHistogram()
                              .Name("gigaam_rtf_decode")
                              .Help("Real-time factor for decode only")
                              .Register(*registry_);

    request_duration_family_ = &prometheus::BuildHistogram()
                                    .Name("gigaam_request_duration_seconds")
                                    .Help("Total request duration")
                                    .Register(*registry_);

    decode_duration_family_ = &prometheus::BuildHistogram()
                                   .Name("gigaam_decode_duration_seconds")
                                   .Help("Decode duration per segment")
                                   .Register(*registry_);
    decode_duration_        = &decode_duration_family_->Add({}, buckets::kDecode());

    audio_duration_family_ = &prometheus::BuildHistogram()
                                  .Name("gigaam_audio_duration_seconds")
                                  .Help("Audio duration per request")
                                  .Register(*registry_);
    audio_duration_        = &audio_duration_family_->Add({}, buckets::kAudio());

    segment_duration_family_ = &prometheus::BuildHistogram()
                                    .Name("gigaam_segment_duration_seconds")
                                    .Help("Segment duration")
                                    .Register(*registry_);
    segment_duration_        = &segment_duration_family_->Add({}, buckets::kSegment());

    preprocess_duration_family_ = &prometheus::BuildHistogram()
                                       .Name("gigaam_preprocess_duration_seconds")
                                       .Help("Preprocessing duration")
                                       .Register(*registry_);
    preprocess_duration_        = &preprocess_duration_family_->Add({}, buckets::kPreprocess());

    io_duration_family_     = &prometheus::BuildHistogram()
                                   .Name("gigaam_io_duration_seconds")
                                   .Help("I/O duration")
                                   .Register(*registry_);
    io_duration_            = &io_duration_family_->Add({}, buckets::kIO());
    recognizer_wait_family_ = &prometheus::BuildHistogram()
                                   .Name("gigaam_recognizer_wait_seconds")
                                   .Help("Time spent waiting for a recognizer slot")
                                   .Register(*registry_);
    recognizer_wait_        = &recognizer_wait_family_->Add({}, buckets::kQueueWait());

    segment_rtf_family_ =
        &prometheus::BuildHistogram().Name("gigaam_segment_rtf").Help("RTF per segment").Register(*registry_);
    segment_rtf_ = &segment_rtf_family_->Add({}, buckets::kRTF());

    // ===== Pipeline Counters =====
    requests_total_family_ =
        &prometheus::BuildCounter().Name("gigaam_requests_total").Help("Total requests").Register(*registry_);

    segments_total_family_ = &prometheus::BuildCounter()
                                  .Name("gigaam_segments_total")
                                  .Help("Total segments processed")
                                  .Register(*registry_);
    segments_total_        = &segments_total_family_->Add({});

    audio_seconds_total_family_ = &prometheus::BuildCounter()
                                       .Name("gigaam_audio_seconds_total")
                                       .Help("Cumulative audio duration")
                                       .Register(*registry_);
    audio_seconds_total_        = &audio_seconds_total_family_->Add({});

    errors_total_family_ =
        &prometheus::BuildCounter().Name("gigaam_errors_total").Help("Total errors").Register(*registry_);

    chunks_total_family_ = &prometheus::BuildCounter()
                                .Name("gigaam_chunks_total")
                                .Help("Total audio chunks received")
                                .Register(*registry_);
    chunks_total_        = &chunks_total_family_->Add({});

    bytes_total_family_              = &prometheus::BuildCounter()
                                            .Name("gigaam_bytes_total")
                                            .Help("Total bytes received")
                                            .Register(*registry_);
    bytes_total_                     = &bytes_total_family_->Add({});
    recognizer_wait_timeouts_family_ = &prometheus::BuildCounter()
                                            .Name("gigaam_recognizer_wait_timeouts_total")
                                            .Help("Timed out waits for recognizer slots")
                                            .Register(*registry_);
    recognizer_wait_timeouts_        = &recognizer_wait_timeouts_family_->Add({});

    // ===== Pipeline Gauges =====
    active_connections_family_ = &prometheus::BuildGauge()
                                      .Name("gigaam_active_connections")
                                      .Help("Active WebSocket connections")
                                      .Register(*registry_);
    active_connections_        = &active_connections_family_->Add({});

    current_rtf_family_ =
        &prometheus::BuildGauge().Name("gigaam_current_rtf").Help("Current RTF").Register(*registry_);
    current_rtf_ = &current_rtf_family_->Add({});

    current_ttfr_family_ = &prometheus::BuildGauge()
                                .Name("gigaam_current_ttfr_seconds")
                                .Help("Current TTFR")
                                .Register(*registry_);
    current_ttfr_        = &current_ttfr_family_->Add({});

    current_decode_family_ = &prometheus::BuildGauge()
                                  .Name("gigaam_current_decode_seconds")
                                  .Help("Current decode time")
                                  .Register(*registry_);
    current_decode_        = &current_decode_family_->Add({});

    current_request_family_ = &prometheus::BuildGauge()
                                   .Name("gigaam_current_request_seconds")
                                   .Help("Current request duration")
                                   .Register(*registry_);
    current_request_        = &current_request_family_->Add({});

    current_audio_family_ = &prometheus::BuildGauge()
                                 .Name("gigaam_current_audio_seconds")
                                 .Help("Current audio duration")
                                 .Register(*registry_);
    current_audio_        = &current_audio_family_->Add({});

    current_rtf_total_family_ = &prometheus::BuildGauge()
                                     .Name("gigaam_current_rtf_total")
                                     .Help("Current total RTF")
                                     .Register(*registry_);
    current_rtf_total_        = &current_rtf_total_family_->Add({});

    current_preprocess_family_ = &prometheus::BuildGauge()
                                      .Name("gigaam_current_preprocess_seconds")
                                      .Help("Current preprocess time")
                                      .Register(*registry_);
    current_preprocess_        = &current_preprocess_family_->Add({});

    current_io_family_ = &prometheus::BuildGauge()
                              .Name("gigaam_current_io_seconds")
                              .Help("Current I/O time")
                              .Register(*registry_);
    current_io_        = &current_io_family_->Add({});

    // ===== Connection Metrics =====
    connection_duration_family_ = &prometheus::BuildHistogram()
                                       .Name("gigaam_connection_duration_seconds")
                                       .Help("WebSocket connection duration")
                                       .Register(*registry_);
    connection_duration_        = &connection_duration_family_->Add({}, buckets::kConnection());

    session_duration_family_ = &prometheus::BuildHistogram()
                                    .Name("gigaam_session_duration_seconds")
                                    .Help("Session duration")
                                    .Register(*registry_);
    session_duration_        = &session_duration_family_->Add({}, buckets::kSession());

    connections_total_family_ = &prometheus::BuildCounter()
                                     .Name("gigaam_connections_total")
                                     .Help("Total connections")
                                     .Register(*registry_);
    connections_total_        = &connections_total_family_->Add({});

    disconnections_total_family_ = &prometheus::BuildCounter()
                                        .Name("gigaam_disconnections_total")
                                        .Help("Total disconnections")
                                        .Register(*registry_);

    sessions_total_family_ =
        &prometheus::BuildCounter().Name("gigaam_sessions_total").Help("Total sessions").Register(*registry_);
    sessions_total_ = &sessions_total_family_->Add({});

    active_sessions_family_ =
        &prometheus::BuildGauge().Name("gigaam_active_sessions").Help("Active sessions").Register(*registry_);
    active_sessions_ = &active_sessions_family_->Add({});

    // ===== Recognition Metrics =====
    words_per_request_family_ = &prometheus::BuildHistogram()
                                     .Name("gigaam_words_per_request")
                                     .Help("Words per recognition request")
                                     .Register(*registry_);
    words_per_request_        = &words_per_request_family_->Add({}, buckets::kWords());

    audio_rms_family_ = &prometheus::BuildHistogram()
                             .Name("gigaam_audio_rms_level")
                             .Help("RMS level of input audio")
                             .Register(*registry_);
    audio_rms_        = &audio_rms_family_->Add({}, buckets::kRMS());

    empty_results_total_family_ = &prometheus::BuildCounter()
                                       .Name("gigaam_empty_results_total")
                                       .Help("Empty result count")
                                       .Register(*registry_);
    empty_results_total_        = &empty_results_total_family_->Add({});

    words_total_family_ =
        &prometheus::BuildCounter().Name("gigaam_words_total").Help("Cumulative words").Register(*registry_);
    words_total_ = &words_total_family_->Add({});

    characters_total_family_ = &prometheus::BuildCounter()
                                    .Name("gigaam_characters_total")
                                    .Help("Cumulative characters")
                                    .Register(*registry_);
    characters_total_        = &characters_total_family_->Add({});

    silence_segments_total_family_ = &prometheus::BuildCounter()
                                          .Name("gigaam_silence_segments_total")
                                          .Help("Silence segments")
                                          .Register(*registry_);
    silence_segments_total_        = &silence_segments_total_family_->Add({});

    low_volume_warnings_family_ = &prometheus::BuildCounter()
                                       .Name("gigaam_low_volume_warnings_total")
                                       .Help("Low volume warnings")
                                       .Register(*registry_);
    low_volume_warnings_        = &low_volume_warnings_family_->Add({});

    detected_language_family_ = &prometheus::BuildCounter()
                                     .Name("gigaam_detected_language_total")
                                     .Help("Detected language count")
                                     .Register(*registry_);

    speech_ratio_family_ = &prometheus::BuildGauge()
                                .Name("gigaam_speech_ratio")
                                .Help("Speech vs silence ratio")
                                .Register(*registry_);
    speech_ratio_        = &speech_ratio_family_->Add({});

    // Pre-cache labeled instances to avoid map<string,string> allocs on hot paths
    realtime_websocket_mode_.requests_success =
        &requests_total_family_->Add({{"status", "success"}, {"mode", "realtime_websocket"}});
    realtime_websocket_mode_.requests_failed =
        &requests_total_family_->Add({{"status", "failed"}, {"mode", "realtime_websocket"}});
    http_mode_.requests_success = &requests_total_family_->Add({{"status", "success"}, {"mode", "http"}});
    http_mode_.requests_failed  = &requests_total_family_->Add({{"status", "failed"}, {"mode", "http"}});
    whisper_api_mode_.requests_success =
        &requests_total_family_->Add({{"status", "success"}, {"mode", "whisper_api"}});
    whisper_api_mode_.requests_failed =
        &requests_total_family_->Add({{"status", "failed"}, {"mode", "whisper_api"}});

    realtime_websocket_mode_.request_duration_success = &request_duration_family_->Add(
        {{"mode", "realtime_websocket"}, {"status", "success"}}, buckets::kRequest());
    realtime_websocket_mode_.request_duration_failed = &request_duration_family_->Add(
        {{"mode", "realtime_websocket"}, {"status", "failed"}}, buckets::kRequest());
    http_mode_.request_duration_success =
        &request_duration_family_->Add({{"mode", "http"}, {"status", "success"}}, buckets::kRequest());
    http_mode_.request_duration_failed =
        &request_duration_family_->Add({{"mode", "http"}, {"status", "failed"}}, buckets::kRequest());
    whisper_api_mode_.request_duration_success =
        &request_duration_family_->Add({{"mode", "whisper_api"}, {"status", "success"}}, buckets::kRequest());
    whisper_api_mode_.request_duration_failed =
        &request_duration_family_->Add({{"mode", "whisper_api"}, {"status", "failed"}}, buckets::kRequest());

    realtime_websocket_mode_.rtf = &rtf_family_->Add({{"mode", "realtime_websocket"}}, buckets::kRTF());
    realtime_websocket_mode_.rtf_decode =
        &rtf_decode_family_->Add({{"mode", "realtime_websocket"}}, buckets::kRTF());
    http_mode_.rtf                    = &rtf_family_->Add({{"mode", "http"}}, buckets::kRTF());
    http_mode_.rtf_decode             = &rtf_decode_family_->Add({{"mode", "http"}}, buckets::kRTF());
    whisper_api_mode_.rtf             = &rtf_family_->Add({{"mode", "whisper_api"}}, buckets::kRTF());
    whisper_api_mode_.rtf_decode      = &rtf_decode_family_->Add({{"mode", "whisper_api"}}, buckets::kRTF());
    disconnections_normal_            = &disconnections_total_family_->Add({{"reason", "normal"}});
    disconnections_message_too_large_ = &disconnections_total_family_->Add({{"reason", "message_too_large"}});
    disconnections_internal_error_    = &disconnections_total_family_->Add({{"reason", "internal_error"}});
    disconnections_other_             = &disconnections_total_family_->Add({{"reason", "other"}});
    errors_capacity_exceeded_         = &errors_total_family_->Add({{"error_type", "capacity_exceeded"}});
    errors_empty_file_                = &errors_total_family_->Add({{"error_type", "empty_file"}});
    errors_file_too_large_            = &errors_total_family_->Add({{"error_type", "file_too_large"}});
    errors_invalid_audio_             = &errors_total_family_->Add({{"error_type", "invalid_audio"}});
    errors_internal_error_            = &errors_total_family_->Add({{"error_type", "internal_error"}});
    errors_bad_multipart_             = &errors_total_family_->Add({{"error_type", "bad_multipart"}});
    errors_invalid_param_             = &errors_total_family_->Add({{"error_type", "invalid_param"}});
    errors_realtime_ws_handler_exception_ =
        &errors_total_family_->Add({{"error_type", "realtime_ws_handler_exception"}});
    errors_other_ = &errors_total_family_->Add({{"error_type", "other"}});

    initialized_ = true;
    spdlog::info("Prometheus metrics initialized");
  });
}

void ASRMetrics::shutdown() {
  spdlog::info("Metrics shutdown");
}

std::shared_ptr<prometheus::Registry> ASRMetrics::registry() {
  return registry_;
}

void ASRMetrics::observe_ttfr(double sec, const std::string& mode) {
  if (!initialized_)
    return;
  switch (canonical_mode(mode)) {
    case MetricMode::RealtimeWebsocket:
      realtime_websocket_mode_.ttfr->Observe(sec);
      break;
    case MetricMode::Http:
      http_mode_.ttfr->Observe(sec);
      break;
    case MetricMode::WhisperApi:
      whisper_api_mode_.ttfr->Observe(sec);
      break;
  }
  current_ttfr_->Set(sec);
}

void ASRMetrics::observe_segment(double audio_sec, double decode_sec) {
  if (!initialized_)
    return;
  decode_duration_->Observe(decode_sec);
  segment_duration_->Observe(audio_sec);
  segments_total_->Increment();
  audio_seconds_total_->Increment(audio_sec);

  if (audio_sec > 0) {
    segment_rtf_->Observe(decode_sec / audio_sec);
  }

  current_decode_->Set(decode_sec);
}

void ASRMetrics::observe_request(double total_sec, double audio_sec, double decode_sec, int chunk_count,
                                 size_t bytes_count, double preprocess_sec, double io_sec,
                                 const std::string& mode, const std::string& status) {
  if (!initialized_)
    return;

  const auto  mode_kind  = canonical_mode(mode);
  const bool  is_success = (status == "success");
  const auto& mode_cache = [this, mode_kind]() -> const ModeMetricCache& {
    switch (mode_kind) {
      case MetricMode::RealtimeWebsocket:
        return realtime_websocket_mode_;
      case MetricMode::Http:
        return http_mode_;
      case MetricMode::WhisperApi:
        return whisper_api_mode_;
    }
    return http_mode_;
  }();

  if (is_success) {
    mode_cache.requests_success->Increment();
    mode_cache.request_duration_success->Observe(total_sec);
  } else {
    mode_cache.requests_failed->Increment();
    mode_cache.request_duration_failed->Observe(total_sec);
  }

  audio_duration_->Observe(audio_sec);
  preprocess_duration_->Observe(preprocess_sec);
  io_duration_->Observe(io_sec);

  if (audio_sec > 0) {
    const double rtf     = total_sec / audio_sec;
    const double rtf_dec = decode_sec / audio_sec;
    mode_cache.rtf->Observe(rtf);
    mode_cache.rtf_decode->Observe(rtf_dec);
    current_rtf_->Set(rtf);
    current_rtf_total_->Set(rtf);
  }

  chunks_total_->Increment(chunk_count);
  bytes_total_->Increment(static_cast<double>(bytes_count));

  current_request_->Set(total_sec);
  current_audio_->Set(audio_sec);
  current_preprocess_->Set(preprocess_sec);
  current_io_->Set(io_sec);
}

void ASRMetrics::observe_recognizer_wait(double sec, bool timed_out) {
  if (!initialized_) {
    return;
  }
  recognizer_wait_->Observe(sec);
  if (timed_out) {
    recognizer_wait_timeouts_->Increment();
  }
}

void ASRMetrics::observe_error(const std::string& error_type) {
  if (!initialized_)
    return;
  const auto canonical = canonical_error_type(error_type);
  if (canonical == "capacity_exceeded") {
    errors_capacity_exceeded_->Increment();
  } else if (canonical == "empty_file") {
    errors_empty_file_->Increment();
  } else if (canonical == "file_too_large") {
    errors_file_too_large_->Increment();
  } else if (canonical == "invalid_audio") {
    errors_invalid_audio_->Increment();
  } else if (canonical == "internal_error") {
    errors_internal_error_->Increment();
  } else if (canonical == "bad_multipart") {
    errors_bad_multipart_->Increment();
  } else if (canonical == "invalid_param") {
    errors_invalid_param_->Increment();
  } else if (canonical == "realtime_ws_handler_exception") {
    errors_realtime_ws_handler_exception_->Increment();
  } else {
    errors_other_->Increment();
  }
}

void ASRMetrics::connection_opened() {
  if (!initialized_)
    return;
  connections_total_->Increment();
  active_connections_->Increment();
}

void ASRMetrics::connection_closed(const std::string& reason, double duration_sec) {
  if (!initialized_)
    return;
  active_connections_->Decrement();
  const auto canonical = canonical_disconnect_reason(reason);
  if (canonical == "normal") {
    disconnections_normal_->Increment();
  } else if (canonical == "message_too_large") {
    disconnections_message_too_large_->Increment();
  } else if (canonical == "internal_error") {
    disconnections_internal_error_->Increment();
  } else {
    disconnections_other_->Increment();
  }
  connection_duration_->Observe(duration_sec);
}

void ASRMetrics::session_started() {
  if (!initialized_)
    return;
  sessions_total_->Increment();
  active_sessions_->Increment();
}

void ASRMetrics::session_ended(double duration_sec) {
  if (!initialized_)
    return;
  active_sessions_->Decrement();
  session_duration_->Observe(duration_sec);
}

void ASRMetrics::record_result(std::string_view text) {
  if (!initialized_)
    return;
  if (text.empty()) {
    empty_results_total_->Increment();
    return;
  }

  int  word_count = 0;
  bool in_word    = false;
  for (const char c : text) {
    if (c == ' ' || c == '\t' || c == '\n') {
      in_word = false;
    } else if (!in_word) {
      in_word = true;
      ++word_count;
    }
  }

  words_total_->Increment(word_count);
  characters_total_->Increment(static_cast<double>(text.size()));
  words_per_request_->Observe(word_count);
}

void ASRMetrics::record_audio_level(double rms) {
  if (!initialized_)
    return;
  audio_rms_->Observe(rms);
  if (rms < 0.005) {
    low_volume_warnings_->Increment();
  }
}

void ASRMetrics::record_silence() {
  if (!initialized_)
    return;
  silence_segments_total_->Increment();
}

void ASRMetrics::set_speech_ratio(double ratio) {
  if (!initialized_)
    return;
  speech_ratio_->Set(ratio);
}

}  // namespace asr
