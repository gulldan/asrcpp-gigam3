#include "asr/handler.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iterator>

#include "asr/audio.h"
#include "asr/config.h"
#include "asr/metrics.h"
#include "asr/recognizer.h"
#include "asr/span.h"

namespace asr {

namespace {

// Escape a string for JSON output, appending directly to `out`.
// Handles all mandatory JSON escapes (RFC 8259 §7).
void json_escape_to(std::string& out, const std::string& s) {
  out.reserve(out.size() + s.size());
  for (const char c : s) {
    switch (c) {
      case '"':
        out.append("\\\"", 2);
        break;
      case '\\':
        out.append("\\\\", 2);
        break;
      case '\b':
        out.append("\\b", 2);
        break;
      case '\f':
        out.append("\\f", 2);
        break;
      case '\n':
        out.append("\\n", 2);
        break;
      case '\r':
        out.append("\\r", 2);
        break;
      case '\t':
        out.append("\\t", 2);
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[7];
          // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
          out.append(buf);
        } else {
          out += c;
        }
        break;
    }
  }
}

}  // namespace

ASRSession::ASRSession(Recognizer& recognizer, const VadConfig& vad_config, const Config& config)
    : recognizer_(recognizer), vad_(vad_config), config_(config) {
  pending_.reserve(static_cast<size_t>(vad_config.window_size));
  out_messages_.reserve(4);
  reset_session();
}

// --- Zero-alloc message buffer ---

ASRSession::OutMessage& ASRSession::next_message() {
  if (out_size_ >= out_messages_.size()) {
    out_messages_.emplace_back();
    out_messages_.back().json.reserve(128);
  }
  return out_messages_[out_size_++];
}

span<const ASRSession::OutMessage> ASRSession::current_messages() const {
  return {out_messages_.data(), out_size_};
}

void ASRSession::write_interim(float duration, float rms, bool is_speech) {
  auto& msg = next_message();
  msg.type  = OutMessage::Interim;
  msg.json.clear();
  fmt::format_to(std::back_inserter(msg.json),
                 R"({{"type":"interim","duration":{:.1f},"rms":{:.4f},"is_speech":{}}})",
                 std::round(duration * 10.0f) / 10.0f, std::round(rms * 10000.0f) / 10000.0f,
                 is_speech ? "true" : "false");
}

void ASRSession::write_final(const std::string& text, float duration) {
  auto& msg = next_message();
  msg.type  = OutMessage::Final;
  msg.json.clear();
  msg.json.append(R"({"type":"final","text":")");
  json_escape_to(msg.json, text);
  fmt::format_to(std::back_inserter(msg.json), R"(","duration":{:.3f}}})",
                 std::round(duration * 1000.0f) / 1000.0f);
}

void ASRSession::write_done() {
  auto& msg = next_message();
  msg.type  = OutMessage::Done;
  msg.json.clear();
  msg.json.append(R"({"type":"done"})");
}

// --- Session lifecycle ---

void ASRSession::reset_session() {
  ++session_seq_;
  start_ts_                = SteadyClock::now();
  first_result_ts_         = {};
  has_first_result_        = false;
  segments_                = 0;
  silence_segments_        = 0;
  decode_sec_              = 0.0;
  preprocess_sec_          = 0.0;
  audio_samples_           = 0;
  last_live_flush_samples_ = 0;
  chunks_                  = 0;
  bytes_                   = 0;
  total_samples_received_  = 0;
  max_duration_exceeded_   = false;
}

void ASRSession::process_vad_segments() {
  while (!vad_.empty()) {
    const auto& segment = vad_.front();
    const float audio_sec =
        static_cast<float>(segment.samples.size()) / static_cast<float>(config_.sample_rate);

    if (audio_sec < config_.min_audio_sec) {
      spdlog::debug("Skipping short segment: {:.3f}s", audio_sec);
      silence_segments_++;
      ASRMetrics::instance().record_silence();
      vad_.pop();
      continue;
    }

    // Recognize
    auto         t0             = SteadyClock::now();
    auto         text           = recognizer_.recognize(segment.samples, config_.sample_rate);
    auto         t1             = SteadyClock::now();
    const double seg_decode_sec = std::chrono::duration<double>(t1 - t0).count();
    decode_sec_ += seg_decode_sec;
    audio_samples_ += segment.samples.size();

    // TTFR tracking
    if (!has_first_result_) {
      first_result_ts_  = SteadyClock::now();
      has_first_result_ = true;
      const double ttfr = std::chrono::duration<double>(first_result_ts_ - start_ts_).count();
      ASRMetrics::instance().observe_ttfr(ttfr, "websocket");
    }

    // Metrics
    ASRMetrics::instance().observe_segment(static_cast<double>(audio_sec), seg_decode_sec);

    if (text.empty()) {
      spdlog::debug("ASR session #{}: empty decode result for segment {:.3f}s", session_seq_, audio_sec);
      silence_segments_++;
      ASRMetrics::instance().record_silence();
    } else {
      segments_++;
      ASRMetrics::instance().record_result(text);
      spdlog::info("ASR session #{}: final segment {:.3f}s text_len={}", session_seq_, audio_sec,
                   text.size());
      write_final(text, audio_sec);
    }

    vad_.pop();
  }
}

bool ASRSession::has_final_messages() const {
  for (size_t i = 0; i < out_size_; ++i) {
    if (out_messages_[i].type == OutMessage::Final) {
      return true;
    }
  }
  return false;
}

void ASRSession::process_live_chunk_fallback() {
  const auto min_samples = static_cast<size_t>(
      std::max(0.0F, config_.min_audio_sec) * static_cast<float>(config_.sample_rate));

  if (live_chunk_.size() < min_samples || live_chunk_.empty()) {
    return;
  }

  const float fallback_rms = compute_rms(live_chunk_);
  if (fallback_rms < config_.silence_threshold) {
    spdlog::info(
        "ASR session #{}: live fallback skipped (duration={:.2f}s rms={:.5f} < silence_threshold={:.5f})",
        session_seq_, static_cast<double>(live_chunk_.size()) / static_cast<double>(config_.sample_rate),
        fallback_rms, config_.silence_threshold);
    live_chunk_.clear();
    return;
  }

  const float audio_sec = static_cast<float>(live_chunk_.size()) / static_cast<float>(config_.sample_rate);
  auto        t0        = SteadyClock::now();
  auto        text      = recognizer_.recognize(live_chunk_, config_.sample_rate);
  auto        t1        = SteadyClock::now();
  const auto  decode_sec = std::chrono::duration<double>(t1 - t0).count();

  decode_sec_ += decode_sec;
  audio_samples_ += live_chunk_.size();

  if (!has_first_result_) {
    first_result_ts_  = SteadyClock::now();
    has_first_result_ = true;
    const double ttfr = std::chrono::duration<double>(first_result_ts_ - start_ts_).count();
    ASRMetrics::instance().observe_ttfr(ttfr, "websocket");
  }

  ASRMetrics::instance().observe_segment(static_cast<double>(audio_sec), decode_sec);

  if (text.empty()) {
    ++silence_segments_;
    ASRMetrics::instance().record_silence();
    spdlog::info("ASR session #{}: live fallback produced empty result (duration={:.2f}s rms={:.5f})",
                 session_seq_, audio_sec, fallback_rms);
  } else {
    ++segments_;
    ASRMetrics::instance().record_result(text);
    spdlog::info("ASR session #{}: live fallback final {:.3f}s text_len={} rms={:.5f}", session_seq_,
                 audio_sec, text.size(), fallback_rms);
    write_final(text, audio_sec);
  }

  live_chunk_.clear();
}

void ASRSession::flush_pending() {
  if (!pending_.empty()) {
    const size_t tail_samples = pending_.size();
    pending_.resize(static_cast<size_t>(config_.vad_window_size), 0.0f);
    vad_.accept_waveform(pending_);
    pending_.clear();
    spdlog::debug("ASR session #{}: flushed pending tail {} samples (padded to {})", session_seq_,
                  tail_samples, config_.vad_window_size);
  }
  vad_.flush();
}

void ASRSession::finalize_session(const char* reason) {
  // Record request-level metrics
  const auto   now       = SteadyClock::now();
  const double total_sec = std::chrono::duration<double>(now - start_ts_).count();
  const double audio_sec = static_cast<double>(audio_samples_) / static_cast<double>(config_.sample_rate);

  spdlog::info(
      "ASR session #{} finalize reason={} total_sec={:.3f} audio_sec={:.3f} chunks={} bytes={} finals={} "
      "silence_segments={}",
      session_seq_, reason, total_sec, audio_sec, chunks_, bytes_, segments_, silence_segments_);

  ASRMetrics::instance().observe_request(total_sec, audio_sec, decode_sec_, chunks_, bytes_, preprocess_sec_,
                                         0.0, "websocket", "success");

  // Speech ratio
  const int total_segments = segments_ + silence_segments_;
  if (total_segments > 0) {
    const double ratio = static_cast<double>(segments_) / static_cast<double>(total_segments);
    ASRMetrics::instance().set_speech_ratio(ratio);
  }

  // Done message
  write_done();

  // End session metrics
  if (session_active_) {
    ASRMetrics::instance().session_ended(total_sec);
    session_active_ = false;
  }

  // Reset for next session
  vad_.reset();
  pending_.clear();
  live_chunk_.clear();
  reset_session();
}

// --- Public API ---

span<const ASRSession::OutMessage> ASRSession::on_audio(span<const float> samples) {
  begin_messages();

  // Previous call may have force-finalized due max_audio_sec and sent {"type":"done"}.
  // Start a fresh session automatically on the next audio chunk so live mode
  // doesn't depend on an explicit RESET from the client.
  max_duration_exceeded_ = false;

  auto preprocess_start = SteadyClock::now();

  // Lazy session start — only count when audio actually arrives
  if (!session_active_) {
    session_active_ = true;
    ASRMetrics::instance().session_started();
    spdlog::info("ASR session #{}: started", session_seq_);
  }

  chunks_++;
  total_samples_received_ += samples.size();
  bytes_ += samples.size() * sizeof(float);
  live_chunk_.insert(live_chunk_.end(), samples.begin(), samples.end());

  // Compute RMS for the chunk
  const float rms = compute_rms(samples);
  ASRMetrics::instance().record_audio_level(static_cast<double>(rms));

  // Accumulate samples and feed to VAD in window-sized chunks
  size_t offset = 0;
  while (offset < samples.size()) {
    const size_t remaining_in_window = static_cast<size_t>(config_.vad_window_size) - pending_.size();
    const size_t to_copy             = std::min(remaining_in_window, samples.size() - offset);

    pending_.insert(pending_.end(), samples.begin() + static_cast<ptrdiff_t>(offset),
                    samples.begin() + static_cast<ptrdiff_t>(offset + to_copy));
    offset += to_copy;

    if (static_cast<int>(pending_.size()) == config_.vad_window_size) {
      vad_.accept_waveform(pending_);
      pending_.clear();
    }
  }

  auto preprocess_end = SteadyClock::now();
  preprocess_sec_ += std::chrono::duration<double>(preprocess_end - preprocess_start).count();

  // Process any finalized VAD segments
  process_vad_segments();
  if (has_final_messages()) {
    live_chunk_.clear();
  }

  // In long continuous speech VAD may delay finalization until silence.
  // Periodically flush live stream state to emit regular finals without
  // requiring client-side RECOGNIZE.
  if (config_.live_flush_interval_sec > 0.0f) {
    const auto interval_samples =
        static_cast<size_t>(config_.live_flush_interval_sec * static_cast<float>(config_.sample_rate));
    if (interval_samples > 0 && (total_samples_received_ - last_live_flush_samples_) >= interval_samples) {
      const double total_input_sec =
          static_cast<double>(total_samples_received_) / static_cast<double>(config_.sample_rate);
      const double since_last_flush_sec =
          static_cast<double>(total_samples_received_ - last_live_flush_samples_) /
          static_cast<double>(config_.sample_rate);
      spdlog::info(
          "ASR session #{}: periodic live flush total_input_sec={:.2f} since_last_flush_sec={:.2f} "
          "pending_samples={} in_speech={}",
          session_seq_, total_input_sec, since_last_flush_sec, pending_.size(),
          vad_.is_speech() ? "true" : "false");
      flush_pending();
      process_vad_segments();
      if (!has_final_messages()) {
        process_live_chunk_fallback();
      } else {
        live_chunk_.clear();
      }
      last_live_flush_samples_ = total_samples_received_;
    }
  }

  // If no segments were finalized, send interim status
  if (out_size_ == 0) {
    const float duration =
        static_cast<float>(total_samples_received_) / static_cast<float>(config_.sample_rate);
    write_interim(duration, rms, vad_.is_speech());
  }

  // Auto-finalize if max audio duration exceeded (DoS protection).
  // Limit is disabled when max_audio_sec == 0.
  if (config_.max_audio_sec > 0.0f) {
    float received_sec =
        static_cast<float>(total_samples_received_) / static_cast<float>(config_.sample_rate);
    if (received_sec > config_.max_audio_sec) {
      spdlog::warn("WS: max audio duration exceeded ({:.1f}s > {:.1f}s), forcing recognize", received_sec,
                   config_.max_audio_sec);
      flush_pending();
      process_vad_segments();
      finalize_session("max_audio_sec");
      max_duration_exceeded_ = true;
    }
  }

  return current_messages();
}

span<const ASRSession::OutMessage> ASRSession::on_recognize() {
  begin_messages();
  spdlog::info("ASR session #{}: RECOGNIZE received (session_active={} max_duration_exceeded={})",
               session_seq_, session_active_ ? "true" : "false", max_duration_exceeded_ ? "true" : "false");

  // If auto-finalize already fired (max_audio_sec), don't finalize again —
  // done message and metrics were already recorded.
  if (max_duration_exceeded_) {
    spdlog::info("ASR session #{}: skipping duplicate finalize after max_audio_sec auto-finalize",
                 session_seq_);
    max_duration_exceeded_ = false;
    return current_messages();
  }

  flush_pending();
  process_vad_segments();
  if (!has_final_messages()) {
    process_live_chunk_fallback();
  } else {
    live_chunk_.clear();
  }
  finalize_session("recognize_command");
  return current_messages();
}

void ASRSession::on_reset() {
  spdlog::info("ASR session #{}: RESET received (chunks={} bytes={} samples={})", session_seq_, chunks_,
               bytes_, total_samples_received_);
  max_duration_exceeded_ = false;
  if (session_active_) {
    ASRMetrics::instance().session_ended(0.0);
    session_active_ = false;
  }
  vad_.reset();
  pending_.clear();
  live_chunk_.clear();
  reset_session();
}

bool ASRSession::is_speech() const {
  return vad_.is_speech();
}

void ASRSession::on_close() {
  if (session_active_) {
    const auto   now     = SteadyClock::now();
    const double elapsed = std::chrono::duration<double>(now - start_ts_).count();
    spdlog::info(
        "ASR session #{}: close active session elapsed={:.3f}s chunks={} bytes={} finals={} "
        "silence_segments={}",
        session_seq_, elapsed, chunks_, bytes_, segments_, silence_segments_);
    ASRMetrics::instance().session_ended(elapsed);
    session_active_ = false;
  } else {
    spdlog::debug("ASR session #{}: close on inactive session", session_seq_);
  }
}

}  // namespace asr
