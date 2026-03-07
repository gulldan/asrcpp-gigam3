#include "asr/server.h"

#include <drogon/WebSocketController.h>
#include <drogon/drogon.h>
#include <prometheus/registry.h>
#include <prometheus/text_serializer.h>
#include <spdlog/spdlog.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "asr/audio.h"
#include "asr/config.h"
#include "asr/handler.h"
#include "asr/metrics.h"
#include "asr/offline_transcription.h"
#include "asr/realtime_session.h"
#include "asr/recognizer.h"
#include "asr/span.h"
#include "asr/whisper_api.h"
#include "trantor/net/EventLoop.h"

namespace asr {

// Shared state for WsController — initialized before server starts accepting connections
struct WsSharedState {
  Recognizer*   recognizer = nullptr;
  VadConfig     vad_config;
  const Config* config = nullptr;
};

namespace {
// Global shared state, set once before drogon starts, read-only after
WsSharedState         g_ws_state;        // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<uint64_t> g_ws_conn_seq{0};  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

constexpr float kHttpRecognitionChunkSec = 20.0f;
}  // namespace

struct WsConnectionContext {
  std::shared_ptr<ASRSession>           session;
  std::chrono::steady_clock::time_point connected_at;
  std::chrono::steady_clock::time_point first_audio_at;
  std::chrono::steady_clock::time_point last_audio_at;
  std::vector<float>                    audio_buf;  // reusable buffer for binary WS messages
  std::string                           close_reason  = "peer_closed_or_normal";
  uint64_t                              connection_id = 0;
  std::unique_ptr<StreamResampler>      resampler;
  bool                                  sample_rate_received   = false;
  int                                   input_sample_rate      = 0;
  uint64_t                              binary_frames          = 0;
  uint64_t                              binary_bytes           = 0;
  uint64_t                              input_samples          = 0;
  uint64_t                              output_samples         = 0;
  uint64_t                              text_messages          = 0;
  uint64_t                              cmd_recognize          = 0;
  uint64_t                              cmd_reset              = 0;
  uint64_t                              sent_interim           = 0;
  uint64_t                              sent_final             = 0;
  uint64_t                              sent_done              = 0;
  double                                max_interframe_gap_sec = 0.0;
};

struct RealtimeConnectionContext {
  std::shared_ptr<Config>               runtime_config;
  RealtimeSession                       realtime{0};
  std::shared_ptr<ASRSession>           session;
  std::unique_ptr<StreamResampler>      resampler;
  std::chrono::steady_clock::time_point connected_at;
  std::chrono::steady_clock::time_point last_event_at;
  std::string                           close_reason = "peer_closed_or_normal";
  std::string                           last_client_event_type;
  std::string                           last_error;
  uint64_t                              connection_id{0};
  uint64_t                              raw_input_samples{0};  // decoded client-format samples
  uint64_t                              input_samples{0};  // samples seen by ASR after resampling
  uint64_t                              append_events{0};
  uint64_t                              ping_events{0};
  uint64_t                              invalid_events{0};
  uint64_t                              decode_errors{0};
  uint64_t                              committed_events{0};
  uint64_t                              completed_events{0};
  uint64_t                              interim_events{0};
  uint64_t                              speech_started_events{0};
  uint64_t                              speech_stopped_events{0};
  double                                max_interevent_gap_sec{0.0};
  bool                                  speech_active{false};
};

// WebSocket controller for /ws
class WsController : public drogon::WebSocketController<WsController> {
 public:
  void handleNewConnection(const drogon::HttpRequestPtr&         req,
                           const drogon::WebSocketConnectionPtr& conn) override {
    if (g_ws_state.recognizer == nullptr || g_ws_state.config == nullptr) {
      spdlog::error("WS: Server not initialized");
      conn->shutdown(drogon::CloseCode::kUnexpectedCondition, "Server not ready");
      return;
    }

    auto ctx = std::make_shared<WsConnectionContext>();
    ctx->session =
        std::make_shared<ASRSession>(*g_ws_state.recognizer, g_ws_state.vad_config, *g_ws_state.config);
    ctx->connected_at  = std::chrono::steady_clock::now();
    ctx->connection_id = g_ws_conn_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    conn->setContext(ctx);

    spdlog::info(
        "WS[{}]: connection opened from {}:{} target_sample_rate={} max_audio_sec={} idle_timeout_sec={}",
        ctx->connection_id, req->peerAddr().toIp(), req->peerAddr().toPort(), g_ws_state.config->sample_rate,
        g_ws_state.config->max_audio_sec, g_ws_state.config->idle_connection_timeout_sec);

    ASRMetrics::instance().connection_opened();
  }

  void handleNewMessage(const drogon::WebSocketConnectionPtr& conn, std::string&& msg,
                        const drogon::WebSocketMessageType& type) override {
    auto ctx = conn->getContext<WsConnectionContext>();
    if (!ctx || !ctx->session) {
      spdlog::error("WS: No session context");
      return;
    }
    try {
      auto&                                   session = ctx->session;
      asr::span<const ASRSession::OutMessage> responses;

      if (type == drogon::WebSocketMessageType::Binary) {
        ++ctx->binary_frames;
        ctx->binary_bytes += msg.size();
        // Guard against oversized messages (DoS/OOM protection)
        if (msg.size() > g_ws_state.config->max_ws_message_bytes) {
          spdlog::warn("WS[{}]: message too large ({} bytes, limit {})", ctx->connection_id, msg.size(),
                       g_ws_state.config->max_ws_message_bytes);
          ctx->close_reason = "message_too_large";
          conn->shutdown(drogon::CloseCode::kViolation, "Message too large");
          return;
        }
        // Binary: float32 audio samples — use memcpy to avoid alignment UB
        if (msg.size() < sizeof(float) || msg.size() % sizeof(float) != 0) {
          spdlog::warn("WS[{}]: invalid binary size {} bytes", ctx->connection_id, msg.size());
          return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (ctx->binary_frames == 1) {
          ctx->first_audio_at = now;
          spdlog::info("WS[{}]: first audio frame {} bytes", ctx->connection_id, msg.size());
        } else {
          const double gap_sec        = std::chrono::duration<double>(now - ctx->last_audio_at).count();
          ctx->max_interframe_gap_sec = std::max(ctx->max_interframe_gap_sec, gap_sec);
        }
        ctx->last_audio_at = now;

        const size_t num_samples = msg.size() / sizeof(float);
        ctx->input_samples += num_samples;
        ctx->audio_buf.resize(num_samples);  // no-op after first call (capacity reused)
        std::memcpy(ctx->audio_buf.data(), msg.data(), msg.size());
        if (ctx->resampler) {
          auto resampled = ctx->resampler->process(ctx->audio_buf);
          ctx->output_samples += resampled.size();
          responses = session->on_audio(resampled);
        } else {
          ctx->output_samples += ctx->audio_buf.size();
          responses = session->on_audio(ctx->audio_buf);
        }

        if (ctx->binary_frames % 200 == 0) {
          const int    in_rate = ctx->input_sample_rate > 0 ? ctx->input_sample_rate : 0;
          const double input_sec =
              in_rate > 0 ? static_cast<double>(ctx->input_samples) / static_cast<double>(in_rate) : 0.0;
          const double ws_life_sec =
              std::chrono::duration<double>(std::chrono::steady_clock::now() - ctx->connected_at).count();
          const double frames_per_s =
              ws_life_sec > 0.0 ? static_cast<double>(ctx->binary_frames) / ws_life_sec : 0.0;
          const double output_sec =
              static_cast<double>(ctx->output_samples) / static_cast<double>(g_ws_state.config->sample_rate);
          spdlog::info(
              "WS[{}]: stream stats frames={} bytes={} input_rate={} input_audio_sec={:.2f} "
              "output_audio_sec={:.2f} "
              "fps={:.1f}",
              ctx->connection_id, ctx->binary_frames, ctx->binary_bytes, in_rate, input_sec, output_sec,
              frames_per_s);
        }
      } else if (type == drogon::WebSocketMessageType::Text) {
        ++ctx->text_messages;
        // Parse sample_rate JSON message from client (allow repeated updates).
        if (!msg.empty() && msg.front() == '{') {
          try {
            auto j = nlohmann::json::parse(msg);
            if (j.contains("client_event")) {
              std::string event_name;
              try {
                event_name = j["client_event"].get<std::string>();
              } catch (const nlohmann::json::exception&) {
                event_name = "<invalid_client_event>";
              }
              spdlog::info("WS[{}]: client_event={} payload={}", ctx->connection_id, event_name, msg);
              return;
            }
            if (j.contains("sample_rate")) {
              int input_rate = j["sample_rate"].get<int>();
              if (input_rate < 8000 || input_rate > 192000) {
                spdlog::warn("WS[{}]: invalid sample_rate {} (must be 8000..192000), ignoring",
                             ctx->connection_id, input_rate);
                return;
              }

              if (ctx->sample_rate_received && input_rate == ctx->input_sample_rate) {
                spdlog::info("WS[{}]: repeated sample_rate {}, keeping existing resampler",
                             ctx->connection_id, input_rate);
                return;
              }

              ctx->sample_rate_received = true;
              ctx->input_sample_rate    = input_rate;
              spdlog::info("WS[{}]: sample_rate received {}", ctx->connection_id, input_rate);
              if (input_rate != g_ws_state.config->sample_rate) {
                ctx->resampler =
                    std::make_unique<StreamResampler>(input_rate, g_ws_state.config->sample_rate);
                spdlog::info("WS[{}]: resampling {} -> {} Hz", ctx->connection_id, input_rate,
                             g_ws_state.config->sample_rate);
              } else {
                ctx->resampler = nullptr;
                spdlog::info("WS[{}]: client sample rate matches target ({}), no resampling needed",
                             ctx->connection_id, input_rate);
              }
              return;
            }
          } catch (const nlohmann::json::exception&) {  // NOLINT(bugprone-empty-catch)
            // Not a valid JSON message, fall through to command handling
          }
        }
        if (msg == "RECOGNIZE") {
          ++ctx->cmd_recognize;
          spdlog::info("WS[{}]: command RECOGNIZE received (frames={} input_samples={} output_samples={})",
                       ctx->connection_id, ctx->binary_frames, ctx->input_samples, ctx->output_samples);
          // Flush resampler filter tail before finalizing
          if (ctx->resampler) {
            auto tail = ctx->resampler->flush();
            if (!tail.empty()) {
              ctx->output_samples += tail.size();
              spdlog::info("WS[{}]: resampler tail samples={}", ctx->connection_id, tail.size());
              for (const auto& r : session->on_audio(tail)) {
                conn->send(r.json, drogon::WebSocketMessageType::Text);
              }
            }
          }
          responses = session->on_recognize();
        } else if (msg == "RESET") {
          ++ctx->cmd_reset;
          spdlog::info("WS[{}]: command RESET received", ctx->connection_id);
          session->on_reset();
          // Reset resampler state without generating tail samples.
          if (ctx->resampler) {
            (*ctx->resampler).reset();
          }
          return;
        } else {
          spdlog::warn("WS[{}]: Unknown text message (size={}): {}", ctx->connection_id, msg.size(), msg);
          return;
        }
      } else {
        return;
      }

      size_t sent_interim = 0;
      size_t sent_final   = 0;
      size_t sent_done    = 0;
      for (const auto& r : responses) {
        conn->send(r.json, drogon::WebSocketMessageType::Text);
        if (r.type == ASRSession::OutMessage::Interim) {
          ++sent_interim;
        } else if (r.type == ASRSession::OutMessage::Final) {
          ++sent_final;
        } else if (r.type == ASRSession::OutMessage::Done) {
          ++sent_done;
        }
      }

      ctx->sent_interim += sent_interim;
      ctx->sent_final += sent_final;
      ctx->sent_done += sent_done;

      if (sent_final > 0 || sent_done > 0) {
        const double output_sec =
            static_cast<double>(ctx->output_samples) / static_cast<double>(g_ws_state.config->sample_rate);
        spdlog::info(
            "WS[{}]: sent responses interim={} final={} done={} (totals interim={} final={} done={} "
            "output_audio_sec={:.2f})",
            ctx->connection_id, sent_interim, sent_final, sent_done, ctx->sent_interim, ctx->sent_final,
            ctx->sent_done, output_sec);
      }
    } catch (const std::exception& e) {
      spdlog::error("WS[{}]: Exception in message handler: {}", ctx->connection_id, e.what());
      ASRMetrics::instance().observe_error("ws_handler_exception");
      ctx->close_reason = "internal_error";
      conn->shutdown(drogon::CloseCode::kUnexpectedCondition, "Internal error");
    }
  }

  void handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) override {
    auto ctx = conn->getContext<WsConnectionContext>();
    if (ctx && ctx->session) {
      ctx->session->on_close();
    }

    const auto        now      = std::chrono::steady_clock::now();
    const double      duration = ctx ? std::chrono::duration<double>(now - ctx->connected_at).count() : 0.0;
    const std::string reason   = ctx ? ctx->close_reason : "peer_closed_or_normal";
    const char*       sample_rate_received = (ctx && ctx->sample_rate_received) ? "true" : "false";

    double input_sec          = 0.0;
    double output_sec         = 0.0;
    double active_audio_sec   = 0.0;
    double last_audio_gap_sec = -1.0;
    if (ctx) {
      const int in_rate =
          ctx->input_sample_rate > 0 ? ctx->input_sample_rate : g_ws_state.config->sample_rate;
      if (in_rate > 0) {
        input_sec = static_cast<double>(ctx->input_samples) / static_cast<double>(in_rate);
      }
      output_sec =
          static_cast<double>(ctx->output_samples) / static_cast<double>(g_ws_state.config->sample_rate);
      if (ctx->binary_frames > 0) {
        active_audio_sec   = std::chrono::duration<double>(ctx->last_audio_at - ctx->first_audio_at).count();
        last_audio_gap_sec = std::chrono::duration<double>(now - ctx->last_audio_at).count();
      }
    }

    spdlog::info(
        "WS[{}]: connection closed duration={:.1f}s reason={} frames={} bytes={} text={} "
        "cmd_recognize={} cmd_reset={} sent_final={} sent_done={} sample_rate_received={} input_rate={} "
        "input_audio_sec={:.2f} output_audio_sec={:.2f} active_audio_sec={:.2f} last_audio_gap_sec={:.2f} "
        "max_interframe_gap_sec={:.2f}",
        ctx ? ctx->connection_id : 0, duration, reason, ctx ? ctx->binary_frames : 0,
        ctx ? ctx->binary_bytes : 0, ctx ? ctx->text_messages : 0, ctx ? ctx->cmd_recognize : 0,
        ctx ? ctx->cmd_reset : 0, ctx ? ctx->sent_final : 0, ctx ? ctx->sent_done : 0, sample_rate_received,
        ctx ? ctx->input_sample_rate : 0, input_sec, output_sec, active_audio_sec, last_audio_gap_sec,
        ctx ? ctx->max_interframe_gap_sec : 0.0);

    if (ctx && !ctx->sample_rate_received && ctx->binary_frames > 0) {
      spdlog::warn("WS[{}]: audio frames arrived before sample_rate JSON; assumed {} Hz", ctx->connection_id,
                   g_ws_state.config->sample_rate);
    }
    if (ctx && reason == "peer_closed_or_normal" && duration < 12.0 && ctx->binary_frames > 0 &&
        ctx->cmd_recognize == 0 && ctx->cmd_reset == 0) {
      spdlog::warn(
          "WS[{}]: closed quickly without RECOGNIZE/RESET (duration={:.1f}s). Likely client-side stop/reload "
          "or "
          "network drop.",
          ctx->connection_id, duration);
    }
    ASRMetrics::instance().connection_closed(reason, duration);
  }

  WS_PATH_LIST_BEGIN
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wc++20-extensions"
  WS_PATH_ADD("/ws");
#pragma GCC diagnostic pop
  WS_PATH_LIST_END
};

class RealtimeWsController : public drogon::WebSocketController<RealtimeWsController> {
 public:
  void handleNewConnection(const drogon::HttpRequestPtr&         req,
                           const drogon::WebSocketConnectionPtr& conn) override {
    if (g_ws_state.recognizer == nullptr || g_ws_state.config == nullptr) {
      spdlog::error("Realtime WS: Server not initialized");
      conn->shutdown(drogon::CloseCode::kUnexpectedCondition, "Server not ready");
      return;
    }

    auto ctx = std::make_shared<RealtimeConnectionContext>();
    ctx->connected_at  = std::chrono::steady_clock::now();
    ctx->last_event_at = ctx->connected_at;
    ctx->connection_id = g_ws_conn_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    ctx->runtime_config = std::make_shared<Config>(*g_ws_state.config);
    ctx->runtime_config->max_audio_sec           = 0.0F;
    ctx->runtime_config->live_flush_interval_sec = 5.0F;
    ctx->realtime = RealtimeSession(ctx->connection_id, make_default_realtime_session_config(*ctx->runtime_config));

    if (ctx->realtime.config().input_sample_rate != ctx->runtime_config->sample_rate) {
      ctx->resampler = std::make_unique<StreamResampler>(ctx->realtime.config().input_sample_rate,
                                                         ctx->runtime_config->sample_rate);
    }
    const auto vad_cfg = make_realtime_vad_config(*ctx->runtime_config, ctx->realtime.config());
    ctx->session       = std::make_shared<ASRSession>(*g_ws_state.recognizer, vad_cfg, *ctx->runtime_config);
    conn->setContext(ctx);

    spdlog::info(
        "RealtimeWS[{}]: connection opened from {}:{} target_sample_rate={} input_format={} input_rate={}",
        ctx->connection_id, req->peerAddr().toIp(), req->peerAddr().toPort(), ctx->runtime_config->sample_rate,
        ctx->realtime.config().input_audio_format, ctx->realtime.config().input_sample_rate);

    conn->send(ctx->realtime.event_session_created(), drogon::WebSocketMessageType::Text);
    ASRMetrics::instance().connection_opened();
  }

  void handleNewMessage(const drogon::WebSocketConnectionPtr& conn, std::string&& msg,
                        const drogon::WebSocketMessageType& type) override {
    auto ctx = conn->getContext<RealtimeConnectionContext>();
    if (!ctx || !ctx->session || !ctx->runtime_config) {
      spdlog::error("Realtime WS: No session context");
      return;
    }

    if (msg.size() > ctx->runtime_config->max_ws_message_bytes) {
      ctx->close_reason = "message_too_large";
      conn->shutdown(drogon::CloseCode::kViolation, "Message too large");
      return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (ctx->last_event_at.time_since_epoch().count() != 0) {
      const double gap_sec = std::chrono::duration<double>(now - ctx->last_event_at).count();
      ctx->max_interevent_gap_sec = std::max(ctx->max_interevent_gap_sec, gap_sec);
    }
    ctx->last_event_at = now;

    if (type != drogon::WebSocketMessageType::Text) {
      spdlog::debug("RealtimeWS[{}]: ignoring non-text frame type={}", ctx->connection_id,
                    static_cast<int>(type));
      return;
    }

    nlohmann::json event;
    try {
      event = nlohmann::json::parse(msg);
    } catch (const nlohmann::json::exception&) {
      ++ctx->invalid_events;
      send_error(conn, *ctx, "invalid_json", "Invalid JSON payload");
      return;
    }

    if (!event.is_object()) {
      ++ctx->invalid_events;
      send_error(conn, *ctx, "invalid_event", "JSON event must be an object");
      return;
    }

    const std::string client_event_id = read_client_event_id(event);
    if (!event.contains("type") || !event["type"].is_string()) {
      ++ctx->invalid_events;
      send_error(conn, *ctx, "invalid_event_type", "Event 'type' must be a string", "type",
                 client_event_id);
      return;
    }

    const std::string event_type = event["type"].get<std::string>();
    ctx->last_client_event_type  = event_type;

    try {
      if (event_type == "transcription_session.update" || event_type == "session.update") {
        handle_session_update(conn, *ctx, event, client_event_id);
        return;
      }

      if (event_type == "input_audio_buffer.append") {
        handle_audio_append(conn, *ctx, event, client_event_id);
        return;
      }

      if (event_type == "input_audio_buffer.commit") {
        handle_audio_commit(conn, *ctx);
        return;
      }

      if (event_type == "input_audio_buffer.clear") {
        handle_audio_clear(conn, *ctx);
        return;
      }

      if (event_type == "ping" || event_type == "noop") {
        ++ctx->ping_events;
        return;
      }

      ++ctx->invalid_events;
      spdlog::warn("RealtimeWS[{}]: unknown event type='{}' event_id='{}'", ctx->connection_id, event_type,
                   client_event_id);
      send_error(conn, *ctx, "unknown_event_type", "Unsupported event type", "type", client_event_id);
    } catch (const AudioError& e) {
      ++ctx->decode_errors;
      send_error(conn, *ctx, "audio_decode_error", e.what(), "audio", client_event_id);
    } catch (const std::exception& e) {
      spdlog::error("RealtimeWS[{}]: exception: {}", ctx->connection_id, e.what());
      ASRMetrics::instance().observe_error("realtime_ws_handler_exception");
      send_error(conn, *ctx, "internal_error", e.what(), "", client_event_id);
    }
  }

  void handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) override {
    auto ctx = conn->getContext<RealtimeConnectionContext>();
    if (ctx && ctx->session) {
      ctx->session->on_close();
    }

    const auto   now      = std::chrono::steady_clock::now();
    const double duration = ctx ? std::chrono::duration<double>(now - ctx->connected_at).count() : 0.0;
    const auto&  reason   = ctx ? ctx->close_reason : std::string("peer_closed_or_normal");

    spdlog::info(
        "RealtimeWS[{}]: connection closed duration={:.1f}s reason={} append_events={} committed={} "
        "completed={} interim={} speech_started={} speech_stopped={} ping={} invalid={} decode_errors={} "
        "raw_audio_sec={:.2f} input_audio_sec={:.2f} last_event='{}' last_error='{}' max_interevent_gap_sec={:.2f}",
        ctx ? ctx->connection_id : 0, duration, reason, ctx ? ctx->append_events : 0,
        ctx ? ctx->committed_events : 0, ctx ? ctx->completed_events : 0,
        ctx ? ctx->interim_events : 0, ctx ? ctx->speech_started_events : 0,
        ctx ? ctx->speech_stopped_events : 0, ctx ? ctx->ping_events : 0, ctx ? ctx->invalid_events : 0,
        ctx ? ctx->decode_errors : 0,
        (ctx && ctx->realtime.config().input_sample_rate > 0)
            ? static_cast<double>(ctx->raw_input_samples) /
                  static_cast<double>(ctx->realtime.config().input_sample_rate)
            : 0.0,
        (ctx && ctx->runtime_config && ctx->runtime_config->sample_rate > 0)
            ? static_cast<double>(ctx->input_samples) /
                  static_cast<double>(ctx->runtime_config->sample_rate)
            : 0.0,
        ctx ? ctx->last_client_event_type : "<none>", ctx ? ctx->last_error : "<none>",
        ctx ? ctx->max_interevent_gap_sec : 0.0);

    ASRMetrics::instance().connection_closed(reason, duration);
  }

  WS_PATH_LIST_BEGIN
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wc++20-extensions"
  WS_PATH_ADD("/v1/realtime");
#pragma GCC diagnostic pop
  WS_PATH_LIST_END

 private:
  static std::string read_client_event_id(const nlohmann::json& event) {
    if (event.contains("event_id") && event["event_id"].is_string()) {
      return event["event_id"].get<std::string>();
    }
    return {};
  }

  static int64_t audio_position_ms(const RealtimeConnectionContext& ctx) {
    if (!ctx.runtime_config || ctx.runtime_config->sample_rate <= 0) {
      return 0;
    }
    const auto rate = static_cast<uint64_t>(ctx.runtime_config->sample_rate);
    return static_cast<int64_t>((ctx.input_samples * 1000ULL) / rate);
  }

  static void send_error(const drogon::WebSocketConnectionPtr& conn, RealtimeConnectionContext& ctx,
                         const std::string& code, const std::string& message,
                         const std::string& param           = "",
                         const std::string& client_event_id = "") {
    ctx.last_error = code + ":" + message;
    conn->send(ctx.realtime.event_error(code, message, param, client_event_id),
               drogon::WebSocketMessageType::Text);
  }

  static void rebuild_pipeline(RealtimeConnectionContext& ctx) {
    ctx.runtime_config->max_audio_sec           = 0.0F;
    ctx.runtime_config->live_flush_interval_sec = 5.0F;
    if (ctx.realtime.config().turn_detection.has_value()) {
      ctx.runtime_config->vad_threshold = ctx.realtime.config().turn_detection->threshold;
      ctx.runtime_config->vad_min_silence =
          static_cast<float>(ctx.realtime.config().turn_detection->silence_duration_ms) / 1000.0F;
    }

    if (ctx.realtime.config().input_sample_rate != ctx.runtime_config->sample_rate) {
      ctx.resampler = std::make_unique<StreamResampler>(ctx.realtime.config().input_sample_rate,
                                                        ctx.runtime_config->sample_rate);
    } else {
      ctx.resampler.reset();
    }

    const auto vad_cfg = make_realtime_vad_config(*ctx.runtime_config, ctx.realtime.config());
    ctx.session        = std::make_shared<ASRSession>(*g_ws_state.recognizer, vad_cfg, *ctx.runtime_config);
    ctx.speech_active  = false;
    ctx.realtime.clear_current_item();
  }

  static std::string extract_final_text(const std::string& payload) {
    const auto parsed = nlohmann::json::parse(payload, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
      return {};
    }
    if (!parsed.contains("type") || !parsed["type"].is_string()) {
      return {};
    }
    if (parsed["type"].get<std::string>() != "final") {
      return {};
    }
    if (!parsed.contains("text") || !parsed["text"].is_string()) {
      return {};
    }
    return parsed["text"].get<std::string>();
  }

  static size_t emit_transcription_events(const drogon::WebSocketConnectionPtr&      conn,
                                          RealtimeConnectionContext&                  ctx,
                                          asr::span<const ASRSession::OutMessage> out_messages) {
    size_t finals = 0;
    for (const auto& out : out_messages) {
      if (out.type != ASRSession::OutMessage::Final) {
        continue;
      }
      const auto text = extract_final_text(out.json);
      if (text.empty()) {
        continue;
      }

      const auto commit = ctx.realtime.commit_current_item();
      conn->send(ctx.realtime.event_buffer_committed(commit), drogon::WebSocketMessageType::Text);
      conn->send(ctx.realtime.event_transcription_completed(commit.item_id, text),
                 drogon::WebSocketMessageType::Text);
      ++ctx.committed_events;
      ++ctx.completed_events;
      ++finals;
      spdlog::info(
          "RealtimeWS[{}]: transcription.completed item_id={} previous_item_id={} text_len={} "
          "append_events={} input_audio_sec={:.2f}",
          ctx.connection_id, commit.item_id, commit.previous_item_id.empty() ? "<none>" : commit.previous_item_id,
          text.size(), ctx.append_events,
          (ctx.runtime_config && ctx.runtime_config->sample_rate > 0)
              ? static_cast<double>(ctx.input_samples) /
                    static_cast<double>(ctx.runtime_config->sample_rate)
              : 0.0);
    }
    return finals;
  }

  static void emit_speech_transition_events(const drogon::WebSocketConnectionPtr& conn,
                                            RealtimeConnectionContext&             ctx) {
    if (!ctx.realtime.config().turn_detection.has_value()) {
      return;
    }

    const bool now_speech = ctx.session->is_speech();
    if (now_speech && !ctx.speech_active) {
      const auto item_id = ctx.realtime.ensure_current_item_id();
      const auto pos_ms  = audio_position_ms(ctx);
      conn->send(ctx.realtime.event_speech_started(pos_ms), drogon::WebSocketMessageType::Text);
      ctx.speech_active = true;
      ++ctx.speech_started_events;
      spdlog::info("RealtimeWS[{}]: speech_started item_id={} audio_start_ms={} append_events={}",
                   ctx.connection_id, item_id, pos_ms, ctx.append_events);
    } else if (!now_speech && ctx.speech_active) {
      const auto item_id = ctx.realtime.ensure_current_item_id();
      const auto pos_ms  = audio_position_ms(ctx);
      conn->send(ctx.realtime.event_speech_stopped(pos_ms), drogon::WebSocketMessageType::Text);
      ctx.speech_active = false;
      ++ctx.speech_stopped_events;
      spdlog::info("RealtimeWS[{}]: speech_stopped item_id={} audio_end_ms={} append_events={}",
                   ctx.connection_id, item_id, pos_ms, ctx.append_events);
    }
  }

  static void handle_session_update(const drogon::WebSocketConnectionPtr& conn,
                                    RealtimeConnectionContext&             ctx,
                                    const nlohmann::json&                  event,
                                    const std::string&                     client_event_id) {
    if (!event.contains("session")) {
      send_error(conn, ctx, "missing_session", "Event must include 'session' object", "session",
                 client_event_id);
      return;
    }

    nlohmann::json sanitized_update = event["session"];
    bool           ignored_turn_detection = false;
    if (sanitized_update.is_object() && sanitized_update.contains("turn_detection")) {
      sanitized_update.erase("turn_detection");
      ignored_turn_detection = true;
    }

    std::string error_message;
    if (!ctx.realtime.apply_session_update(sanitized_update, &error_message)) {
      send_error(conn, ctx, "invalid_session_update", error_message, "session", client_event_id);
      return;
    }

    rebuild_pipeline(ctx);
    conn->send(ctx.realtime.event_session_updated(), drogon::WebSocketMessageType::Text);
    if (ignored_turn_detection) {
      spdlog::info(
          "RealtimeWS[{}]: session.update event_id='{}' ignored client turn_detection; using server VAD "
          "settings threshold={:.3f} silence_ms={}",
          ctx.connection_id, client_event_id, ctx.runtime_config->vad_threshold,
          static_cast<int>(ctx.runtime_config->vad_min_silence * 1000.0F));
    }
    if (ctx.realtime.config().turn_detection.has_value()) {
      spdlog::info(
          "RealtimeWS[{}]: session.update applied event_id='{}' input_format={} input_rate={} turn=server_vad "
          "threshold={:.3f} silence_ms={}",
          ctx.connection_id, client_event_id, ctx.realtime.config().input_audio_format,
          ctx.realtime.config().input_sample_rate, ctx.realtime.config().turn_detection->threshold,
          ctx.realtime.config().turn_detection->silence_duration_ms);
    } else {
      spdlog::info(
          "RealtimeWS[{}]: session.update applied event_id='{}' input_format={} input_rate={} turn=null",
          ctx.connection_id, client_event_id, ctx.realtime.config().input_audio_format,
          ctx.realtime.config().input_sample_rate);
    }
  }

  static void handle_audio_append(const drogon::WebSocketConnectionPtr& conn,
                                  RealtimeConnectionContext&             ctx,
                                  const nlohmann::json&                  event,
                                  const std::string&                     client_event_id) {
    if (!event.contains("audio") || !event["audio"].is_string()) {
      send_error(conn, ctx, "invalid_audio", "Event must include base64 string field 'audio'", "audio",
                 client_event_id);
      return;
    }

    ++ctx.append_events;
    auto samples = decode_realtime_audio(event["audio"].get<std::string>(),
                                         ctx.realtime.config().input_audio_format);
    ctx.raw_input_samples += samples.size();

    asr::span<const ASRSession::OutMessage> out_messages;
    size_t                                  asr_samples_in = 0;
    if (ctx.resampler) {
      auto resampled = ctx.resampler->process(samples);
      ctx.input_samples += resampled.size();
      asr_samples_in = resampled.size();
      out_messages = ctx.session->on_audio(resampled);
    } else {
      ctx.input_samples += samples.size();
      asr_samples_in = samples.size();
      out_messages = ctx.session->on_audio(samples);
    }

    size_t interim_count = 0;
    size_t final_count   = 0;
    for (const auto& out : out_messages) {
      if (out.type == ASRSession::OutMessage::Interim) {
        ++interim_count;
      } else if (out.type == ASRSession::OutMessage::Final) {
        ++final_count;
      }
    }
    ctx.interim_events += interim_count;

    emit_speech_transition_events(conn, ctx);
    const size_t emitted_finals = emit_transcription_events(conn, ctx, out_messages);

    if (ctx.append_events == 1 || ctx.append_events % 250 == 0 || emitted_finals > 0) {
      const double input_audio_sec =
          (ctx.runtime_config && ctx.runtime_config->sample_rate > 0)
              ? static_cast<double>(ctx.input_samples) /
                    static_cast<double>(ctx.runtime_config->sample_rate)
              : 0.0;
      spdlog::info(
          "RealtimeWS[{}]: append#{} event_id='{}' b64_len={} decoded_samples={} asr_samples={} "
          "interim={} final={} speech_active={} input_audio_sec={:.2f}",
          ctx.connection_id, ctx.append_events, client_event_id,
          event.contains("audio") && event["audio"].is_string() ? event["audio"].get<std::string>().size() : 0,
          samples.size(), asr_samples_in, interim_count, final_count, ctx.speech_active ? "true" : "false",
          input_audio_sec);
    }
  }

  static void handle_audio_commit(const drogon::WebSocketConnectionPtr& conn,
                                  RealtimeConnectionContext&             ctx) {
    spdlog::info("RealtimeWS[{}]: input_audio_buffer.commit requested append_events={} speech_active={}",
                 ctx.connection_id, ctx.append_events, ctx.speech_active ? "true" : "false");

    size_t finals = 0;
    if (ctx.resampler) {
      auto tail = ctx.resampler->flush();
      if (!tail.empty()) {
        ctx.input_samples += tail.size();
        finals += emit_transcription_events(conn, ctx, ctx.session->on_audio(tail));
      }
    }

    finals += emit_transcription_events(conn, ctx, ctx.session->on_recognize());
    if (finals == 0) {
      const auto commit = ctx.realtime.commit_current_item();
      conn->send(ctx.realtime.event_buffer_committed(commit), drogon::WebSocketMessageType::Text);
      ++ctx.committed_events;
      spdlog::info("RealtimeWS[{}]: commit without final item_id={} previous_item_id={}", ctx.connection_id,
                   commit.item_id, commit.previous_item_id.empty() ? "<none>" : commit.previous_item_id);
    }
    ctx.speech_active = false;
    spdlog::info("RealtimeWS[{}]: commit completed finals={} committed_total={} completed_total={}",
                 ctx.connection_id, finals, ctx.committed_events, ctx.completed_events);
  }

  static void handle_audio_clear(const drogon::WebSocketConnectionPtr& conn,
                                 RealtimeConnectionContext&             ctx) {
    ctx.session->on_reset();
    if (ctx.resampler) {
      ctx.resampler->reset();
    }
    ctx.speech_active = false;
    ctx.realtime.clear_current_item();
    conn->send(ctx.realtime.event_buffer_cleared(), drogon::WebSocketMessageType::Text);
    spdlog::info("RealtimeWS[{}]: input_audio_buffer.clear applied", ctx.connection_id);
  }
};

// Signal handling for graceful shutdown — async-signal-safe only
volatile sig_atomic_t Server::shutdown_requested_{0};

namespace {
void signal_handler(int /*sig*/) {
  Server::shutdown_requested_ = 1;
  // Async-signal-safe minimal output
  static const char msg[] = "Signal received, shutting down...\n";
  // write() is async-signal-safe per POSIX
  (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
}

// Concurrent request semaphore for HTTP /recognize
struct RequestSemaphore {
  std::mutex              mutex;
  std::condition_variable cv;
  size_t                  max_count = 0;
  size_t                  count     = 0;

  bool try_acquire() {
    const std::scoped_lock lock(mutex);
    if (count >= max_count) {
      return false;
    }
    ++count;
    return true;
  }

  void release() {
    {
      const std::scoped_lock lock(mutex);
      --count;
    }
    cv.notify_one();
  }
};

RequestSemaphore g_request_sem;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace

Server::Server(const Config& config, Recognizer& recognizer) : config_(config), recognizer_(recognizer) {
  vad_config_.model_path           = config.vad_model;
  vad_config_.threshold            = config.vad_threshold;
  vad_config_.min_silence_duration = config.vad_min_silence;
  vad_config_.min_speech_duration  = config.vad_min_speech;
  vad_config_.max_speech_duration  = config.vad_max_speech;
  vad_config_.sample_rate          = config.sample_rate;
  vad_config_.window_size          = config.vad_window_size;
  vad_config_.context_size         = config.vad_context_size;

  // Set global state before server starts — this is safe because drogon::app().run()
  // hasn't been called yet, so no connections can arrive
  g_ws_state.recognizer = &recognizer_;
  g_ws_state.vad_config = vad_config_;
  g_ws_state.config     = &config_;

  // Initialize concurrent request limiter
  g_request_sem.max_count = config.max_concurrent_requests;
}

void Server::install_signal_handlers() {
  std::signal(SIGINT, signal_handler);   // NOLINT(cert-err33-c)
  std::signal(SIGTERM, signal_handler);  // NOLINT(cert-err33-c)
}

void Server::setup_http_handlers() {
  auto& app = drogon::app();

  // GET / — serve static HTML
  app.registerHandler("/",
                      [](const drogon::HttpRequestPtr& /*req*/,
                         std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
                        auto resp = drogon::HttpResponse::newFileResponse("static/index.html");
                        callback(resp);
                      },
                      {drogon::Get});

  // GET /health — deep check: verify model is loaded
  app.registerHandler("/health",
                      [this](const drogon::HttpRequestPtr& /*req*/,
                             std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
                        nlohmann::json j;
                        j["status"]   = "ok";
                        j["provider"] = config_.provider;
                        j["threads"]  = config_.num_threads;
                        auto resp     = drogon::HttpResponse::newHttpResponse();
                        resp->setStatusCode(drogon::k200OK);
                        resp->setBody(j.dump());
                        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                        callback(resp);
                      },
                      {drogon::Get});

  // GET /metrics — Prometheus
  app.registerHandler("/metrics",
                      [](const drogon::HttpRequestPtr& /*req*/,
                         std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
                        const prometheus::TextSerializer serializer;
                        auto collected = ASRMetrics::instance().registry()->Collect();
                        auto text      = serializer.Serialize(collected);
                        auto resp      = drogon::HttpResponse::newHttpResponse();
                        resp->setStatusCode(drogon::k200OK);
                        resp->setBody(std::move(text));
                        resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
                        callback(resp);
                      },
                      {drogon::Get});

  // POST /recognize — file upload
  app.registerHandler(
      "/recognize",
      [this](const drogon::HttpRequestPtr&                         req,
             std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        auto& metrics = ASRMetrics::instance();

        auto make_error = [&metrics](drogon::HttpStatusCode status, const std::string& detail,
                                     const std::string& error_type) {
          nlohmann::json err;
          err["detail"] = detail;
          auto resp     = drogon::HttpResponse::newHttpResponse();
          resp->setStatusCode(status);
          resp->setBody(err.dump());
          resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
          metrics.observe_error(error_type);
          return resp;
        };

        // Concurrent request limiting
        if (!g_request_sem.try_acquire()) {
          callback(make_error(drogon::k503ServiceUnavailable, "Server at capacity, try again later",
                              "capacity_exceeded"));
          metrics.observe_request(0.0, 0.0, 0.0, 0, 0, 0.0, 0.0, "http", "failed");
          return;
        }

        metrics.session_started();
        auto start_ts = std::chrono::steady_clock::now();

        auto make_error_and_release = [&metrics, &make_error, &start_ts](drogon::HttpStatusCode status,
                                                                         const std::string&     detail,
                                                                         const std::string&     error_type) {
          auto resp = make_error(status, detail, error_type);
          auto end  = std::chrono::steady_clock::now();
          metrics.observe_request(std::chrono::duration<double>(end - start_ts).count(), 0.0, 0.0, 0, 0, 0.0,
                                  0.0, "http", "failed");
          metrics.session_ended(0.0);
          g_request_sem.release();
          return resp;
        };

        // Get uploaded file
        drogon::MultiPartParser fileUpload;
        if (fileUpload.parse(req) != 0 || fileUpload.getFiles().empty()) {
          callback(make_error_and_release(drogon::k400BadRequest, "No file uploaded", "empty_file"));
          return;
        }

        const auto& file = fileUpload.getFiles()[0];

        // Check upload size limit
        if (file.fileContent().size() > config_.max_upload_bytes) {
          callback(
              make_error_and_release(drogon::k413RequestEntityTooLarge, "File too large", "file_too_large"));
          return;
        }

        auto file_data = asr::span<const uint8_t>(reinterpret_cast<const uint8_t*>(file.fileContent().data()),
                                                  file.fileContent().size());

        if (file_data.empty()) {
          callback(make_error_and_release(drogon::k400BadRequest, "Empty file", "empty_file"));
          return;
        }

        try {
          // Decode uploaded audio
          auto         preprocess_start = std::chrono::steady_clock::now();
          auto         audio            = decode_audio(file_data, file.getFileName(), config_.sample_rate);
          auto         preprocess_end   = std::chrono::steady_clock::now();
          const double preprocess_sec =
              std::chrono::duration<double>(preprocess_end - preprocess_start).count();

          // Recognize
          auto decode_start = std::chrono::steady_clock::now();
          auto text = recognize_audio_chunked(audio.samples, config_.sample_rate, kHttpRecognitionChunkSec,
                                              [this](span<const float> chunk, int sample_rate) {
                                                return recognizer_.recognize(chunk, sample_rate);
                                              });
          auto decode_end         = std::chrono::steady_clock::now();
          const double decode_sec = std::chrono::duration<double>(decode_end - decode_start).count();

          auto         end_ts    = std::chrono::steady_clock::now();
          const double total_sec = std::chrono::duration<double>(end_ts - start_ts).count();

          // Record metrics
          metrics.observe_ttfr(decode_sec, "http");
          metrics.observe_segment(static_cast<double>(audio.duration_sec), decode_sec);
          metrics.observe_request(total_sec, static_cast<double>(audio.duration_sec), decode_sec, 1,
                                  file_data.size(), preprocess_sec, 0.0, "http", "success");
          metrics.record_result(text);
          metrics.session_ended(total_sec);

          g_request_sem.release();

          // Response
          nlohmann::json j;
          j["text"]     = text;
          j["duration"] = audio.duration_sec;
          auto resp     = drogon::HttpResponse::newHttpResponse();
          resp->setStatusCode(drogon::k200OK);
          resp->setBody(j.dump());
          resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
          callback(resp);

        } catch (const AudioError& e) {
          callback(make_error_and_release(drogon::k400BadRequest, e.what(), "invalid_audio"));
        } catch (const std::exception& e) {
          callback(make_error_and_release(drogon::k500InternalServerError, e.what(), "internal_error"));
        }
      },
      {drogon::Post});

  auto whisper_handler = [this](const drogon::HttpRequestPtr&                         req,
                                std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    auto& metrics = ASRMetrics::instance();

    auto make_whisper_error = [&metrics](drogon::HttpStatusCode status, const std::string& detail,
                                         const std::string& metrics_error_type,
                                         const std::string& api_error_type, const std::string& api_param,
                                         const std::string& api_code) {
      auto resp = drogon::HttpResponse::newHttpResponse();
      resp->setStatusCode(status);
      resp->setBody(build_whisper_api_error_json(detail, api_error_type, api_param, api_code));
      resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
      metrics.observe_error(metrics_error_type);
      return resp;
    };

    if (!g_request_sem.try_acquire()) {
      callback(make_whisper_error(drogon::k503ServiceUnavailable, "Server at capacity, try again later",
                                  "capacity_exceeded", "server_error", "", "capacity_exceeded"));
      metrics.observe_request(0.0, 0.0, 0.0, 0, 0, 0.0, 0.0, "whisper_api", "failed");
      return;
    }

    metrics.session_started();
    auto start_ts = std::chrono::steady_clock::now();

    auto make_error_and_release = [&metrics, &make_whisper_error, &start_ts](
                                      drogon::HttpStatusCode status, const std::string& detail,
                                      const std::string& metrics_error_type,
                                      const std::string& api_error_type, const std::string& api_param,
                                      const std::string& api_code) {
      auto resp = make_whisper_error(status, detail, metrics_error_type, api_error_type, api_param, api_code);
      auto end  = std::chrono::steady_clock::now();
      metrics.observe_request(std::chrono::duration<double>(end - start_ts).count(), 0.0, 0.0, 0, 0, 0.0, 0.0,
                              "whisper_api", "failed");
      metrics.session_ended(0.0);
      g_request_sem.release();
      return resp;
    };

    drogon::MultiPartParser multipart;
    if (multipart.parse(req) != 0) {
      callback(make_error_and_release(drogon::k400BadRequest, "Expected multipart/form-data request",
                                      "bad_multipart", "invalid_request_error", "file", "invalid_multipart"));
      return;
    }

    std::unordered_map<std::string, std::string> form_fields;
    form_fields.reserve(multipart.getParameters().size());
    for (const auto& [key, value] : multipart.getParameters()) {
      form_fields.emplace(key, value);
    }

    WhisperTranscriptionRequest whisper_request;
    if (auto parse_error = parse_whisper_transcription_request(form_fields, &whisper_request);
        parse_error.has_value()) {
      callback(make_error_and_release(drogon::k400BadRequest, parse_error->message, "invalid_param",
                                      parse_error->type, parse_error->param, parse_error->code));
      return;
    }

    const auto& files = multipart.getFiles();
    const auto  it    = std::find_if(files.begin(), files.end(),
                                     [](const drogon::HttpFile& file) { return file.getItemName() == "file"; });

    const drogon::HttpFile* upload = nullptr;
    if (it != files.end()) {
      upload = &(*it);
    } else if (!files.empty()) {
      upload = &files.front();
    }
    if (upload == nullptr) {
      callback(make_error_and_release(drogon::k400BadRequest, "Missing required field 'file'", "empty_file",
                                      "invalid_request_error", "file", "missing_field"));
      return;
    }

    const std::string upload_file_name = upload->getFileName();
    const std::string upload_file_content(upload->fileContent());

    if (upload_file_content.size() > config_.max_upload_bytes) {
      callback(make_error_and_release(drogon::k413RequestEntityTooLarge, "File too large", "file_too_large",
                                      "invalid_request_error", "file", "file_too_large"));
      return;
    }

    auto file_data = asr::span<const uint8_t>(reinterpret_cast<const uint8_t*>(upload_file_content.data()),
                                              upload_file_content.size());
    if (file_data.empty()) {
      callback(make_error_and_release(drogon::k400BadRequest, "Empty file uploaded", "empty_file",
                                      "invalid_request_error", "file", "empty_file"));
      return;
    }

    try {
      auto         preprocess_start = std::chrono::steady_clock::now();
      auto         audio            = decode_audio(file_data, upload_file_name, config_.sample_rate);
      auto         preprocess_end   = std::chrono::steady_clock::now();
      const double preprocess_sec = std::chrono::duration<double>(preprocess_end - preprocess_start).count();

      auto decode_start = std::chrono::steady_clock::now();
      auto text       = recognize_audio_chunked(audio.samples, config_.sample_rate, kHttpRecognitionChunkSec,
                                                [this](span<const float> chunk, int sample_rate) {
                                            return recognizer_.recognize(chunk, sample_rate);
                                          });
      auto decode_end = std::chrono::steady_clock::now();
      const double decode_sec = std::chrono::duration<double>(decode_end - decode_start).count();

      auto         end_ts    = std::chrono::steady_clock::now();
      const double total_sec = std::chrono::duration<double>(end_ts - start_ts).count();

      metrics.observe_ttfr(decode_sec, "whisper_api");
      metrics.observe_segment(static_cast<double>(audio.duration_sec), decode_sec);
      metrics.observe_request(total_sec, static_cast<double>(audio.duration_sec), decode_sec, 1,
                              file_data.size(), preprocess_sec, 0.0, "whisper_api", "success");
      metrics.record_result(text);
      metrics.session_ended(total_sec);

      g_request_sem.release();

      WhisperTranscriptionResponsePayload payload;
      payload.text         = text;
      payload.duration_sec = audio.duration_sec;
      payload.language     = whisper_request.language.empty() ? "unknown" : whisper_request.language;

      auto rendered = render_whisper_transcription_response(whisper_request, payload);
      auto resp     = drogon::HttpResponse::newHttpResponse();
      resp->setStatusCode(drogon::k200OK);
      resp->setBody(std::move(rendered.body));
      if (rendered.content_type == "application/json") {
        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
      } else {
        resp->setContentTypeString(rendered.content_type);
      }
      callback(resp);
    } catch (const AudioError& e) {
      callback(make_error_and_release(drogon::k400BadRequest, e.what(), "invalid_audio",
                                      "invalid_request_error", "file", "invalid_audio"));
    } catch (const std::exception& e) {
      callback(make_error_and_release(drogon::k500InternalServerError, e.what(), "internal_error",
                                      "server_error", "", "internal_error"));
    }
  };

  // Drogon's registerHandler template keeps lvalue callables as references;
  // pass an owning copy to avoid dangling after setup_http_handlers() returns.
  using WhisperHandler = std::decay_t<decltype(whisper_handler)>;
  app.registerHandler("/v1/audio/transcriptions", WhisperHandler(whisper_handler), {drogon::Post});
  app.registerHandler("/audio/transcriptions", WhisperHandler(whisper_handler), {drogon::Post});
}

void Server::setup_ws_handler() {
  // WebSocket controller is auto-registered via WS_PATH_ADD macro
}

void Server::run() {
  install_signal_handlers();
  setup_http_handlers();
  setup_ws_handler();

  spdlog::info("Starting server on {}:{} with {} threads", config_.host, config_.port, config_.threads);

  drogon::app()
      .setLogLevel(trantor::Logger::kInfo)
      .addListener(config_.host, config_.port)
      .setThreadNum(config_.threads)
      .setDocumentRoot("./static")
      .setClientMaxBodySize(config_.max_upload_bytes)
      .setIdleConnectionTimeout(config_.idle_connection_timeout_sec);

  // Poll for signal flag from event loop — avoids calling non-async-signal-safe
  // functions from the signal handler
  drogon::app().getLoop()->runEvery(1.0, []() {
    if (Server::shutdown_requested_ != 0) {
      spdlog::info("Shutdown flag detected, stopping server...");
      drogon::app().quit();
    }
  });

  drogon::app().run();

  spdlog::info("Server stopped");
}

}  // namespace asr
