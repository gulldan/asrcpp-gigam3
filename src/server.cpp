#include "asr/server.h"

#include <_string.h>
#include <drogon/DrObject.h>
#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <drogon/MultiPart.h>
#include <drogon/WebSocketConnection.h>
#include <drogon/WebSocketController.h>
#include <drogon/drogon_callbacks.h>
#include <drogon/utils/HttpConstraint.h>
#include <prometheus/registry.h>
#include <prometheus/text_serializer.h>
#include <spdlog/spdlog.h>
#include <trantor/net/InetAddress.h>
#include <trantor/utils/Logger.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <ratio>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "asr/audio.h"
#include "asr/config.h"
#include "asr/executor.h"
#include "asr/handler.h"
#include "asr/logging.h"
#include "asr/metrics.h"
#include "asr/realtime_session.h"
#include "asr/recognizer.h"
#include "asr/span.h"
#include "asr/string_utils.h"
#include "asr/whisper_api.h"
#include "trantor/net/EventLoop.h"

namespace asr {

// Shared server state, initialized before accepting connections.
struct ServerSharedState {
  Recognizer*   recognizer = nullptr;
  VadConfig     vad_config;
  const Config* config = nullptr;
};

namespace {
// Global shared state, set once before drogon starts, read-only after
ServerSharedState     g_server_state;            // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<uint64_t> g_ws_conn_seq{0};          // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<size_t> g_active_ws_connections{0};  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

constexpr float  kHttpRecognitionChunkSec = 20.0f;
constexpr auto   kCloseTryAgainLater      = static_cast<drogon::CloseCode>(1013);
constexpr size_t kRealtimeWsPendingTasks  = 16;
constexpr double kExecutorRetryDelaySec   = 0.01;
constexpr int    kHotPathWarnIntervalMs   = 2000;
constexpr int    kCapacityWarnIntervalMs  = 1000;

std::unique_ptr<BoundedExecutor>
    g_asr_executor;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

void append_transcription_chunk(std::string& text, std::string_view chunk_text) {
  auto trimmed = trim_ascii(chunk_text);
  if (trimmed.empty()) {
    return;
  }
  if (!text.empty()) {
    text.push_back(' ');
  }
  text += trimmed;
}

size_t http_chunk_samples(int sample_rate) {
  return std::max<size_t>(1, static_cast<size_t>(kHttpRecognitionChunkSec * static_cast<float>(sample_rate)));
}

bool is_realtime_opus_format(std::string_view format) {
  return format == "opus" || format == "opus_raw" || format == "opus_rtp";
}

OpusPacketMode realtime_opus_packet_mode(std::string_view format) {
  if (format == "opus_raw") {
    return OpusPacketMode::Raw;
  }
  if (format == "opus_rtp") {
    return OpusPacketMode::Rtp;
  }
  return OpusPacketMode::Auto;
}

std::string canonical_whisper_response_language(std::string_view requested_language) {
  auto normalized = to_lower_ascii(trim_ascii(requested_language));
  if (normalized.empty() || normalized == "ru" || normalized == "ru-ru" || normalized == "russian") {
    return "ru";
  }
  return normalized;
}

size_t asr_executor_worker_count(const Config& config) {
  return std::max<size_t>(1, static_cast<size_t>(std::max(config.recognizer_pool_size, 1)));
}

size_t asr_executor_queue_capacity(const Config& config) {
  const auto workers     = asr_executor_worker_count(config);
  const auto http_budget = std::max<size_t>(1, config.max_concurrent_requests);
  return std::max(workers * 8U, http_budget * 2U);
}

bool try_acquire_ws_slot(size_t max_connections) {
  if (max_connections == 0) {
    g_active_ws_connections.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  size_t current = g_active_ws_connections.load(std::memory_order_relaxed);
  while (current < max_connections) {
    if (g_active_ws_connections.compare_exchange_weak(current, current + 1, std::memory_order_acq_rel,
                                                      std::memory_order_relaxed)) {
      return true;
    }
  }
  return false;
}

void release_ws_slot() {
  g_active_ws_connections.fetch_sub(1, std::memory_order_acq_rel);
}

struct WsSlotGuard {
  bool active = false;

  explicit WsSlotGuard(size_t max_connections) : active(try_acquire_ws_slot(max_connections)) {}

  ~WsSlotGuard() {
    if (active) {
      release_ws_slot();
    }
  }

  WsSlotGuard(const WsSlotGuard&)            = delete;
  WsSlotGuard& operator=(const WsSlotGuard&) = delete;
  WsSlotGuard(WsSlotGuard&&)                 = delete;
  WsSlotGuard& operator=(WsSlotGuard&&)      = delete;

  void release() {
    active = false;
  }
};
}  // namespace

struct RealtimeConnectionContext {
  std::shared_ptr<Config>               runtime_config;
  RealtimeSession                       realtime{0};
  std::shared_ptr<ASRSession>           session;
  trantor::EventLoop*                   loop = nullptr;
  std::unique_ptr<StreamResampler>      resampler;
  std::unique_ptr<RealtimeOpusDecoder>  opus_decoder;
  std::mutex                            state_mutex;
  std::unique_ptr<SerializedTaskQueue>  task_queue;
  std::atomic<bool>                     stop_processing{false};
  bool                                  retry_scheduled       = false;
  bool                                  session_close_pending = false;
  std::chrono::steady_clock::time_point connected_at;
  std::chrono::steady_clock::time_point last_event_at;
  std::string                           close_reason = "normal";
  std::string                           last_client_event_type;
  std::string                           last_error;
  std::vector<uint8_t>                  decoded_audio_bytes;
  std::vector<float>                    decoded_audio_samples;
  uint64_t                              connection_id{0};
  uint64_t                              raw_input_samples{0};  // decoded client-format samples
  uint64_t                              input_samples{0};      // samples seen by ASR after resampling
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
  bool                                  metrics_accounted{false};
};

namespace {

int64_t sample_position_ms(const RealtimeConnectionContext& ctx, int64_t sample_position) {
  if (!ctx.runtime_config || ctx.runtime_config->sample_rate <= 0 || sample_position <= 0) {
    return 0;
  }
  const auto rate = static_cast<int64_t>(ctx.runtime_config->sample_rate);
  return static_cast<int64_t>((sample_position * 1000LL) / rate);
}

template <typename Context>
void schedule_serial_queue_retry(const std::shared_ptr<Context>& ctx);

template <typename Context>
void on_serial_task_finished(const std::shared_ptr<Context>& ctx) {
  if (!ctx || !ctx->task_queue) {
    return;
  }

  ctx->task_queue->finish_current();
  if (ctx->stop_processing.load(std::memory_order_acquire)) {
    ctx->task_queue->stop(true);
  } else if (!ctx->task_queue->maybe_start_next() && ctx->task_queue->pending() > 0) {
    schedule_serial_queue_retry(ctx);
  }

  if (ctx->session_close_pending && !ctx->task_queue->in_flight() && ctx->session) {
    ctx->session->on_close();
    ctx->session_close_pending = false;
  }
}

template <typename Context>
void schedule_serial_queue_retry(const std::shared_ptr<Context>& ctx) {
  if (!ctx || !ctx->loop || !ctx->task_queue || ctx->retry_scheduled || ctx->task_queue->stopped() ||
      ctx->task_queue->in_flight() || ctx->task_queue->pending() == 0) {
    return;
  }

  ctx->retry_scheduled = true;
  ctx->loop->runAfter(kExecutorRetryDelaySec, [ctx]() {
    if (!ctx) {
      return;
    }
    ctx->retry_scheduled = false;
    if (!ctx->task_queue || ctx->task_queue->stopped() ||
        ctx->stop_processing.load(std::memory_order_acquire)) {
      return;
    }
    if (!ctx->task_queue->maybe_start_next() && ctx->task_queue->pending() > 0) {
      schedule_serial_queue_retry(ctx);
    }
  });
}

template <typename Context>
bool enqueue_serial_task(const std::shared_ptr<Context>& ctx, std::function<bool()> start) {
  if (!ctx || !ctx->task_queue || ctx->stop_processing.load(std::memory_order_acquire)) {
    return false;
  }
  const bool accepted = ctx->task_queue->push_or_start(std::move(start));
  if (accepted && ctx->task_queue->pending() > 0 && !ctx->task_queue->in_flight()) {
    schedule_serial_queue_retry(ctx);
  }
  return accepted;
}

}  // namespace

class RealtimeWsController : public drogon::WebSocketController<RealtimeWsController> {
 public:
  void handleNewConnection(const drogon::HttpRequestPtr&         req,
                           const drogon::WebSocketConnectionPtr& conn) override {
    if (g_server_state.recognizer == nullptr || g_server_state.config == nullptr) {
      spdlog::error("Realtime WS: Server not initialized");
      conn->shutdown(drogon::CloseCode::kUnexpectedCondition, "Server not ready");
      return;
    }

    WsSlotGuard slot_guard(g_server_state.config->max_ws_connections);
    if (!slot_guard.active) {
      ASR_LOG_WARN_EVERY(kCapacityWarnIntervalMs,
                         "Realtime WS: rejecting connection from {}:{} due capacity limit active={} limit={}",
                         req->peerAddr().toIp(), req->peerAddr().toPort(),
                         g_active_ws_connections.load(std::memory_order_relaxed),
                         g_server_state.config->max_ws_connections);
      ASRMetrics::instance().observe_error("capacity_exceeded");
      conn->shutdown(kCloseTryAgainLater, "Server at capacity");
      return;
    }

    try {
      auto ctx                           = std::make_shared<RealtimeConnectionContext>();
      ctx->loop                          = drogon::app().getLoop();
      ctx->task_queue                    = std::make_unique<SerializedTaskQueue>(kRealtimeWsPendingTasks);
      ctx->connected_at                  = std::chrono::steady_clock::now();
      ctx->last_event_at                 = ctx->connected_at;
      ctx->connection_id                 = g_ws_conn_seq.fetch_add(1, std::memory_order_relaxed) + 1;
      ctx->runtime_config                = std::make_shared<Config>(*g_server_state.config);
      ctx->runtime_config->max_audio_sec = 0.0F;
      ctx->runtime_config->live_flush_interval_sec = 5.0F;
      ctx->realtime =
          RealtimeSession(ctx->connection_id, make_default_realtime_session_config(*ctx->runtime_config));

      if (ctx->realtime.config().input_sample_rate != ctx->runtime_config->sample_rate) {
        ctx->resampler = std::make_unique<StreamResampler>(ctx->realtime.config().input_sample_rate,
                                                           ctx->runtime_config->sample_rate);
      }
      const auto vad_cfg = make_realtime_vad_config(*ctx->runtime_config, ctx->realtime.config());
      ctx->session = std::make_shared<ASRSession>(*g_server_state.recognizer, vad_cfg, *ctx->runtime_config,
                                                  "realtime_websocket");
      ctx->metrics_accounted = true;
      conn->setContext(ctx);
      slot_guard.release();

      spdlog::info(
          "RealtimeWS[{}]: connection opened from {}:{} target_sample_rate={} input_format={} input_rate={} "
          "active_ws={} max_ws={}",
          ctx->connection_id, req->peerAddr().toIp(), req->peerAddr().toPort(),
          ctx->runtime_config->sample_rate, ctx->realtime.config().input_audio_format,
          ctx->realtime.config().input_sample_rate, g_active_ws_connections.load(std::memory_order_relaxed),
          g_server_state.config->max_ws_connections);

      conn->send(ctx->realtime.event_session_created(), drogon::WebSocketMessageType::Text);
      ASRMetrics::instance().connection_opened();
    } catch (const std::exception& e) {
      spdlog::error("Realtime WS: failed to initialize connection: {}", e.what());
      ASRMetrics::instance().observe_error("internal_error");
      conn->shutdown(drogon::CloseCode::kUnexpectedCondition, "Internal error");
    }
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
      const double gap_sec        = std::chrono::duration<double>(now - ctx->last_event_at).count();
      ctx->max_interevent_gap_sec = std::max(ctx->max_interevent_gap_sec, gap_sec);
    }
    ctx->last_event_at = now;

    if (type == drogon::WebSocketMessageType::Binary) {
      const std::weak_ptr<drogon::WebSocketConnection> weak_conn = conn;
      auto payload = std::make_shared<std::string>(std::move(msg));
      auto start   = [ctx, weak_conn, payload]() -> bool {
        if (!g_asr_executor) {
          return false;
        }
        return g_asr_executor->try_submit([ctx, weak_conn, payload]() {
          try {
            if (auto conn_locked = weak_conn.lock()) {
              handle_audio_append_binary(conn_locked, *ctx, *payload);
            }
          } catch (const RecognizerBusyError& e) {
            ASRMetrics::instance().observe_error("capacity_exceeded");
            if (auto conn_locked = weak_conn.lock()) {
              send_error(conn_locked, *ctx, "server_busy", e.what());
            }
          } catch (const AudioError& e) {
            {
              const std::scoped_lock lock(ctx->state_mutex);
              ++ctx->decode_errors;
            }
            if (auto conn_locked = weak_conn.lock()) {
              send_error(conn_locked, *ctx, "audio_decode_error", e.what(), "audio");
            }
          } catch (const std::exception& e) {
            spdlog::error("RealtimeWS[{}]: exception: {}", ctx->connection_id, e.what());
            ASRMetrics::instance().observe_error("realtime_ws_handler_exception");
            if (auto conn_locked = weak_conn.lock()) {
              send_error(conn_locked, *ctx, "internal_error", e.what());
            }
          }

          if (ctx->loop) {
            ctx->loop->queueInLoop([ctx]() { on_serial_task_finished(ctx); });
          }
        });
      };

      if (!enqueue_serial_task(ctx, std::move(start))) {
        ASRMetrics::instance().observe_error("capacity_exceeded");
        ASR_LOG_WARN_EVERY(kCapacityWarnIntervalMs, "RealtimeWS[{}]: connection task queue is full",
                           ctx->connection_id);
        send_error(conn, *ctx, "server_busy", "Connection queue is full");
        ctx->close_reason = "capacity_exceeded";
        ctx->stop_processing.store(true, std::memory_order_release);
        if (ctx->task_queue) {
          ctx->task_queue->stop(true);
        }
        conn->shutdown(kCloseTryAgainLater, "Connection queue is full");
      }
      return;
    }

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
      send_error(conn, *ctx, "invalid_event_type", "Event 'type' must be a string", "type", client_event_id);
      return;
    }

    const std::string event_type = event["type"].get<std::string>();
    ctx->last_client_event_type  = event_type;

    if (event_type == "ping" || event_type == "noop") {
      ++ctx->ping_events;
      return;
    }

    const std::weak_ptr<drogon::WebSocketConnection> weak_conn = conn;
    auto event_ptr           = std::make_shared<nlohmann::json>(std::move(event));
    auto event_type_ptr      = std::make_shared<const std::string>(event_type);
    auto client_event_id_ptr = std::make_shared<const std::string>(client_event_id);
    auto start               = [ctx, weak_conn, event_ptr, event_type_ptr, client_event_id_ptr]() -> bool {
      try {
        if (!g_asr_executor) {
          return false;
        }
        return g_asr_executor->try_submit([ctx, weak_conn, event_ptr, event_type_ptr, client_event_id_ptr]() {
          try {
            if (auto conn_locked = weak_conn.lock()) {
              const auto& event_type      = *event_type_ptr;
              const auto& client_event_id = *client_event_id_ptr;
              if (event_type == "transcription_session.update" || event_type == "session.update") {
                handle_session_update(conn_locked, *ctx, *event_ptr, client_event_id);
              } else if (event_type == "input_audio_buffer.append") {
                handle_audio_append(conn_locked, *ctx, *event_ptr, client_event_id);
              } else if (event_type == "input_audio_buffer.commit") {
                handle_audio_commit(conn_locked, *ctx);
              } else if (event_type == "input_audio_buffer.clear") {
                handle_audio_clear(conn_locked, *ctx);
              } else {
                {
                  const std::scoped_lock lock(ctx->state_mutex);
                  ++ctx->invalid_events;
                }
                ASR_LOG_WARN_EVERY(kHotPathWarnIntervalMs,
                                   "RealtimeWS[{}]: unknown event type='{}' event_id='{}'",
                                   ctx->connection_id, event_type, client_event_id);
                send_error(conn_locked, *ctx, "unknown_event_type", "Unsupported event type", "type",
                           client_event_id);
              }
            }
          } catch (const RecognizerBusyError& e) {
            ASRMetrics::instance().observe_error("capacity_exceeded");
            if (auto conn_locked = weak_conn.lock()) {
              send_error(conn_locked, *ctx, "server_busy", e.what());
            }
          } catch (const AudioError& e) {
            {
              const std::scoped_lock lock(ctx->state_mutex);
              ++ctx->decode_errors;
            }
            if (auto conn_locked = weak_conn.lock()) {
              send_error(conn_locked, *ctx, "audio_decode_error", e.what(), "audio", *client_event_id_ptr);
            }
          } catch (const std::exception& e) {
            spdlog::error("RealtimeWS[{}]: exception: {}", ctx->connection_id, e.what());
            ASRMetrics::instance().observe_error("realtime_ws_handler_exception");
            if (auto conn_locked = weak_conn.lock()) {
              send_error(conn_locked, *ctx, "internal_error", e.what(), "", *client_event_id_ptr);
            }
          } catch (...) {
            spdlog::error("RealtimeWS[{}]: exception: unknown", ctx->connection_id);
            ASRMetrics::instance().observe_error("realtime_ws_handler_exception");
            if (auto conn_locked = weak_conn.lock()) {
              send_error(conn_locked, *ctx, "internal_error", "Unknown internal error", "",
                         *client_event_id_ptr);
            }
          }

          if (ctx->loop) {
            ctx->loop->queueInLoop([ctx]() { on_serial_task_finished(ctx); });
          }
        });
      } catch (const std::exception& e) {
        spdlog::error("RealtimeWS[{}]: failed to enqueue event '{}': {}", ctx->connection_id, *event_type_ptr,
                      e.what());
        ASRMetrics::instance().observe_error("realtime_ws_handler_exception");
        return false;
      } catch (...) {
        spdlog::error("RealtimeWS[{}]: failed to enqueue event '{}': unknown exception", ctx->connection_id,
                      *event_type_ptr);
        ASRMetrics::instance().observe_error("realtime_ws_handler_exception");
        return false;
      }
    };

    if (!enqueue_serial_task(ctx, std::move(start))) {
      ASRMetrics::instance().observe_error("capacity_exceeded");
      ASR_LOG_WARN_EVERY(kCapacityWarnIntervalMs, "RealtimeWS[{}]: connection task queue is full",
                         ctx->connection_id);
      send_error(conn, *ctx, "server_busy", "Connection queue is full", "", client_event_id);
      ctx->close_reason = "capacity_exceeded";
      ctx->stop_processing.store(true, std::memory_order_release);
      if (ctx->task_queue) {
        ctx->task_queue->stop(true);
      }
      conn->shutdown(kCloseTryAgainLater, "Connection queue is full");
    }
  }

  void handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) override {
    auto ctx = conn->getContext<RealtimeConnectionContext>();
    if (ctx) {
      ctx->stop_processing.store(true, std::memory_order_release);
      if (ctx->task_queue) {
        ctx->task_queue->stop(true);
      }
      if (ctx->session) {
        if (ctx->task_queue && ctx->task_queue->in_flight()) {
          ctx->session_close_pending = true;
        } else {
          ctx->session->on_close();
        }
      }
    }

    const auto   now           = std::chrono::steady_clock::now();
    const double duration      = ctx ? std::chrono::duration<double>(now - ctx->connected_at).count() : 0.0;
    std::string  reason        = "normal";
    uint64_t     append_events = 0;
    uint64_t     committed_events      = 0;
    uint64_t     completed_events      = 0;
    uint64_t     interim_events        = 0;
    uint64_t     speech_started_events = 0;
    uint64_t     speech_stopped_events = 0;
    uint64_t     decode_errors         = 0;
    uint64_t     raw_input_samples     = 0;
    uint64_t     input_samples         = 0;
    std::string  last_error;
    if (ctx) {
      const std::scoped_lock lock(ctx->state_mutex);
      reason                = ctx->close_reason;
      append_events         = ctx->append_events;
      committed_events      = ctx->committed_events;
      completed_events      = ctx->completed_events;
      interim_events        = ctx->interim_events;
      speech_started_events = ctx->speech_started_events;
      speech_stopped_events = ctx->speech_stopped_events;
      decode_errors         = ctx->decode_errors;
      raw_input_samples     = ctx->raw_input_samples;
      input_samples         = ctx->input_samples;
      last_error            = ctx->last_error;
    }

    spdlog::info(
        "RealtimeWS[{}]: connection closed duration={:.1f}s reason={} append_events={} committed={} "
        "completed={} interim={} speech_started={} speech_stopped={} ping={} invalid={} decode_errors={} "
        "raw_audio_sec={:.2f} input_audio_sec={:.2f} last_event='{}' last_error='{}' "
        "max_interevent_gap_sec={:.2f}",
        ctx ? ctx->connection_id : 0, duration, reason, append_events, committed_events, completed_events,
        interim_events, speech_started_events, speech_stopped_events, ctx ? ctx->ping_events : 0,
        ctx ? ctx->invalid_events : 0, decode_errors,
        (ctx && ctx->realtime.config().input_sample_rate > 0)
            ? static_cast<double>(raw_input_samples) /
                  static_cast<double>(ctx->realtime.config().input_sample_rate)
            : 0.0,
        (ctx && ctx->runtime_config && ctx->runtime_config->sample_rate > 0)
            ? static_cast<double>(input_samples) / static_cast<double>(ctx->runtime_config->sample_rate)
            : 0.0,
        ctx ? ctx->last_client_event_type : "<none>", ctx ? last_error : "<none>",
        ctx ? ctx->max_interevent_gap_sec : 0.0);

    if (ctx && ctx->metrics_accounted) {
      ctx->metrics_accounted = false;
      release_ws_slot();
      ASRMetrics::instance().connection_closed(reason, duration);
    }
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

  static void send_error(const drogon::WebSocketConnectionPtr& conn, RealtimeConnectionContext& ctx,
                         const std::string& code, const std::string& message, const std::string& param = "",
                         const std::string& client_event_id = "") {
    {
      const std::scoped_lock lock(ctx.state_mutex);
      ctx.last_error = code + ":" + message;
    }
    conn->send(ctx.realtime.event_error(code, message, param, client_event_id),
               drogon::WebSocketMessageType::Text);
  }

  static void rebuild_pipeline(RealtimeConnectionContext& ctx) {
    const auto& realtime_cfg = ctx.realtime.config();

    *ctx.runtime_config                         = *g_server_state.config;
    ctx.runtime_config->max_audio_sec           = 0.0F;
    ctx.runtime_config->live_flush_interval_sec = 5.0F;
    if (realtime_cfg.turn_detection.has_value()) {
      const auto& turn                    = realtime_cfg.turn_detection.value();
      ctx.runtime_config->vad_threshold   = turn.threshold;
      ctx.runtime_config->vad_min_silence = static_cast<float>(turn.silence_duration_ms) / 1000.0F;
    }

    if (realtime_cfg.input_sample_rate != ctx.runtime_config->sample_rate) {
      ctx.resampler =
          std::make_unique<StreamResampler>(realtime_cfg.input_sample_rate, ctx.runtime_config->sample_rate);
    } else {
      ctx.resampler.reset();
    }

    if (is_realtime_opus_format(realtime_cfg.input_audio_format)) {
      ctx.opus_decoder =
          std::make_unique<RealtimeOpusDecoder>(realtime_cfg.input_sample_rate, 8, true,
                                                realtime_opus_packet_mode(realtime_cfg.input_audio_format));
    } else {
      ctx.opus_decoder.reset();
    }

    ctx.decoded_audio_bytes.clear();
    ctx.decoded_audio_samples.clear();
    ctx.decoded_audio_bytes.reserve(static_cast<size_t>(64U) * static_cast<size_t>(1024U));
    ctx.decoded_audio_samples.reserve(static_cast<size_t>(ctx.runtime_config->sample_rate));

    const auto vad_cfg = make_realtime_vad_config(*ctx.runtime_config, realtime_cfg);
    ctx.session       = std::make_shared<ASRSession>(*g_server_state.recognizer, vad_cfg, *ctx.runtime_config,
                                                     "realtime_websocket");
    ctx.speech_active = false;
    ctx.realtime.clear_current_item();
  }

  static size_t emit_transcription_events(const drogon::WebSocketConnectionPtr&   conn,
                                          RealtimeConnectionContext&              ctx,
                                          asr::span<const ASRSession::OutMessage> out_messages) {
    size_t finals = 0;
    for (const auto& out : out_messages) {
      if (out.type != ASRSession::OutMessage::Final) {
        continue;
      }
      if (out.text.empty()) {
        continue;
      }

      const auto commit = ctx.realtime.commit_current_item();
      conn->send(ctx.realtime.event_buffer_committed(commit), drogon::WebSocketMessageType::Text);
      conn->send(ctx.realtime.event_transcription_completed(commit.item_id, out.text),
                 drogon::WebSocketMessageType::Text);
      uint64_t append_events = 0;
      size_t   input_samples = 0;
      {
        const std::scoped_lock lock(ctx.state_mutex);
        ++ctx.committed_events;
        ++ctx.completed_events;
        append_events = ctx.append_events;
        input_samples = ctx.input_samples;
      }
      ++finals;
      spdlog::debug(
          "RealtimeWS[{}]: transcription.completed item_id={} previous_item_id={} text_len={} "
          "append_events={} input_audio_sec={:.2f}",
          ctx.connection_id, commit.item_id,
          commit.previous_item_id.empty() ? "<none>" : commit.previous_item_id, out.text.size(),
          append_events,
          (ctx.runtime_config && ctx.runtime_config->sample_rate > 0)
              ? static_cast<double>(input_samples) / static_cast<double>(ctx.runtime_config->sample_rate)
              : 0.0);
    }
    return finals;
  }

  static void emit_speech_transition_events(const drogon::WebSocketConnectionPtr& conn,
                                            RealtimeConnectionContext&            ctx) {
    if (!ctx.realtime.config().turn_detection.has_value()) {
      return;
    }

    while (ctx.session->has_speech_transition()) {
      const auto& transition    = ctx.session->front_speech_transition();
      const auto  pos_ms        = sample_position_ms(ctx, transition.sample);
      const auto  item_id       = ctx.realtime.ensure_current_item_id();
      uint64_t    append_events = 0;
      if (transition.kind == ASRSession::SpeechTransition::Started) {
        conn->send(ctx.realtime.event_speech_started(pos_ms), drogon::WebSocketMessageType::Text);
        ctx.speech_active = true;
        {
          const std::scoped_lock lock(ctx.state_mutex);
          ++ctx.speech_started_events;
          append_events = ctx.append_events;
        }
        spdlog::debug("RealtimeWS[{}]: speech_started item_id={} audio_start_ms={} append_events={}",
                      ctx.connection_id, item_id, pos_ms, append_events);
      } else {
        conn->send(ctx.realtime.event_speech_stopped(pos_ms), drogon::WebSocketMessageType::Text);
        ctx.speech_active = false;
        {
          const std::scoped_lock lock(ctx.state_mutex);
          ++ctx.speech_stopped_events;
          append_events = ctx.append_events;
        }
        spdlog::debug("RealtimeWS[{}]: speech_stopped item_id={} audio_end_ms={} append_events={}",
                      ctx.connection_id, item_id, pos_ms, append_events);
      }
      ctx.session->pop_speech_transition();
    }
  }

  static void handle_session_update(const drogon::WebSocketConnectionPtr& conn,
                                    RealtimeConnectionContext& ctx, const nlohmann::json& event,
                                    const std::string& client_event_id) {
    if (!event.contains("session")) {
      send_error(conn, ctx, "missing_session", "Event must include 'session' object", "session",
                 client_event_id);
      return;
    }

    std::string error_message;
    if (!ctx.realtime.apply_session_update(event["session"], &error_message)) {
      send_error(conn, ctx, "invalid_session_update", error_message, "session", client_event_id);
      return;
    }

    rebuild_pipeline(ctx);
    conn->send(ctx.realtime.event_session_updated(), drogon::WebSocketMessageType::Text);
    const auto& realtime_cfg = ctx.realtime.config();
    if (realtime_cfg.turn_detection.has_value()) {
      const auto& turn = realtime_cfg.turn_detection.value();
      spdlog::info(
          "RealtimeWS[{}]: session.update applied event_id='{}' input_format={} input_rate={} "
          "turn=server_vad threshold={:.3f} prefix_padding_ms={} silence_ms={}",
          ctx.connection_id, client_event_id, realtime_cfg.input_audio_format, realtime_cfg.input_sample_rate,
          turn.threshold, turn.prefix_padding_ms, turn.silence_duration_ms);
    } else {
      spdlog::info(
          "RealtimeWS[{}]: session.update applied event_id='{}' input_format={} input_rate={} turn=null",
          ctx.connection_id, client_event_id, realtime_cfg.input_audio_format,
          realtime_cfg.input_sample_rate);
    }
  }

  static span<const float> decode_append_samples(RealtimeConnectionContext& ctx,
                                                 span<const uint8_t>        audio_bytes) {
    const auto& format = ctx.realtime.config().input_audio_format;
    if (format.empty() || format == "pcm16") {
      pcm16_to_float32_into(audio_bytes, ctx.decoded_audio_samples);
      return ctx.decoded_audio_samples;
    }

    if (is_realtime_opus_format(format)) {
      if (!ctx.opus_decoder) {
        ctx.opus_decoder = std::make_unique<RealtimeOpusDecoder>(ctx.realtime.config().input_sample_rate, 8,
                                                                 true, realtime_opus_packet_mode(format));
      }
      return ctx.opus_decoder->decode_packet(audio_bytes);
    }

    throw AudioError("Unsupported realtime audio format '" + format + "'");
  }

  static void process_audio_append_samples(const drogon::WebSocketConnectionPtr& conn,
                                           RealtimeConnectionContext& ctx, span<const float> samples,
                                           const std::string& client_event_id, const char* payload_label,
                                           size_t payload_size) {
    {
      const std::scoped_lock lock(ctx.state_mutex);
      ++ctx.append_events;
      ctx.raw_input_samples += samples.size();
    }

    asr::span<const ASRSession::OutMessage> out_messages;
    size_t                                  asr_samples_in = 0;
    if (ctx.resampler) {
      auto resampled = ctx.resampler->process(samples);
      {
        const std::scoped_lock lock(ctx.state_mutex);
        ctx.input_samples += resampled.size();
      }
      asr_samples_in = resampled.size();
      out_messages   = ctx.session->on_audio(resampled);
    } else {
      {
        const std::scoped_lock lock(ctx.state_mutex);
        ctx.input_samples += samples.size();
      }
      asr_samples_in = samples.size();
      out_messages   = ctx.session->on_audio(samples);
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
    {
      const std::scoped_lock lock(ctx.state_mutex);
      ctx.interim_events += interim_count;
    }

    emit_speech_transition_events(conn, ctx);
    const size_t emitted_finals = emit_transcription_events(conn, ctx, out_messages);

    uint64_t append_events = 0;
    size_t   input_samples = 0;
    {
      const std::scoped_lock lock(ctx.state_mutex);
      append_events = ctx.append_events;
      input_samples = ctx.input_samples;
    }

    if (append_events == 1 || append_events % 250 == 0 || emitted_finals > 0) {
      const double input_audio_sec =
          (ctx.runtime_config && ctx.runtime_config->sample_rate > 0)
              ? static_cast<double>(input_samples) / static_cast<double>(ctx.runtime_config->sample_rate)
              : 0.0;
      uint64_t opus_lost = 0;
      uint64_t opus_plc  = 0;
      uint64_t opus_fec  = 0;
      uint64_t opus_dup  = 0;
      uint64_t opus_ooo  = 0;
      if (ctx.opus_decoder && is_realtime_opus_format(ctx.realtime.config().input_audio_format)) {
        const auto& stats = ctx.opus_decoder->stats();
        opus_lost         = stats.lost_packets;
        opus_plc          = stats.plc_packets;
        opus_fec          = stats.fec_packets;
        opus_dup          = stats.duplicate_packets;
        opus_ooo          = stats.out_of_order_packets;
      }

      spdlog::debug(
          "RealtimeWS[{}]: append#{} event_id='{}' {}={} decoded_samples={} asr_samples={} "
          "interim={} final={} speech_active={} input_audio_sec={:.2f} "
          "opus_lost={} opus_plc={} opus_fec={} opus_dup={} opus_ooo={}",
          ctx.connection_id, append_events, client_event_id, payload_label, payload_size, samples.size(),
          asr_samples_in, interim_count, final_count, ctx.speech_active ? "true" : "false", input_audio_sec,
          opus_lost, opus_plc, opus_fec, opus_dup, opus_ooo);
    }
  }

  static void handle_audio_append(const drogon::WebSocketConnectionPtr& conn, RealtimeConnectionContext& ctx,
                                  const nlohmann::json& event, const std::string& client_event_id) {
    if (!event.contains("audio") || !event["audio"].is_string()) {
      send_error(conn, ctx, "invalid_audio", "Event must include base64 string field 'audio'", "audio",
                 client_event_id);
      return;
    }

    const auto& b64_audio = event["audio"].get_ref<const std::string&>();
    base64_decode_into(b64_audio, ctx.decoded_audio_bytes);
    auto samples = decode_append_samples(ctx, ctx.decoded_audio_bytes);
    process_audio_append_samples(conn, ctx, samples, client_event_id, "b64_len", b64_audio.size());
  }

  static void handle_audio_append_binary(const drogon::WebSocketConnectionPtr& conn,
                                         RealtimeConnectionContext& ctx, const std::string& payload) {
    auto bytes   = asr::span<const uint8_t>(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
    auto samples = decode_append_samples(ctx, bytes);
    process_audio_append_samples(conn, ctx, samples, "<binary>", "bin_len", payload.size());
  }

  static void handle_audio_commit(const drogon::WebSocketConnectionPtr& conn,
                                  RealtimeConnectionContext&            ctx) {
    uint64_t append_events = 0;
    {
      const std::scoped_lock lock(ctx.state_mutex);
      append_events = ctx.append_events;
    }
    spdlog::debug("RealtimeWS[{}]: input_audio_buffer.commit requested append_events={} speech_active={}",
                  ctx.connection_id, append_events, ctx.speech_active ? "true" : "false");

    size_t finals = 0;
    if (ctx.resampler) {
      auto tail = ctx.resampler->flush();
      if (!tail.empty()) {
        {
          const std::scoped_lock lock(ctx.state_mutex);
          ctx.input_samples += tail.size();
        }
        const auto out_messages = ctx.session->on_audio(tail);
        emit_speech_transition_events(conn, ctx);
        finals += emit_transcription_events(conn, ctx, out_messages);
      }
    }

    const auto recognize_messages = ctx.session->on_recognize();
    emit_speech_transition_events(conn, ctx);
    finals += emit_transcription_events(conn, ctx, recognize_messages);
    if (finals == 0) {
      const auto commit = ctx.realtime.commit_current_item();
      conn->send(ctx.realtime.event_buffer_committed(commit), drogon::WebSocketMessageType::Text);
      {
        const std::scoped_lock lock(ctx.state_mutex);
        ++ctx.committed_events;
      }
      spdlog::debug("RealtimeWS[{}]: commit without final item_id={} previous_item_id={}", ctx.connection_id,
                    commit.item_id, commit.previous_item_id.empty() ? "<none>" : commit.previous_item_id);
    }
    ctx.speech_active        = false;
    uint64_t committed_total = 0;
    uint64_t completed_total = 0;
    {
      const std::scoped_lock lock(ctx.state_mutex);
      committed_total = ctx.committed_events;
      completed_total = ctx.completed_events;
    }
    spdlog::debug("RealtimeWS[{}]: commit completed finals={} committed_total={} completed_total={}",
                  ctx.connection_id, finals, committed_total, completed_total);
  }

  static void handle_audio_clear(const drogon::WebSocketConnectionPtr& conn, RealtimeConnectionContext& ctx) {
    ctx.session->on_reset();
    if (ctx.resampler) {
      ctx.resampler->reset();
    }
    if (ctx.opus_decoder) {
      ctx.opus_decoder->reset();
    }
    ctx.speech_active = false;
    ctx.realtime.clear_current_item();
    conn->send(ctx.realtime.event_buffer_cleared(), drogon::WebSocketMessageType::Text);
    spdlog::debug("RealtimeWS[{}]: input_audio_buffer.clear applied", ctx.connection_id);
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
  g_server_state.recognizer = &recognizer_;
  g_server_state.vad_config = vad_config_;
  g_server_state.config     = &config_;

  // Initialize concurrent request limiter
  g_request_sem.max_count = config.max_concurrent_requests;
  g_asr_executor          = std::make_unique<BoundedExecutor>(asr_executor_worker_count(config),
                                                              asr_executor_queue_capacity(config));
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

  auto make_json_response = [](drogon::HttpStatusCode status, const nlohmann::json& body) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(status);
    resp->setBody(body.dump());
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    return resp;
  };

  // GET /healthz — lightweight liveness check
  app.registerHandler(
      "/healthz",
      [this, make_json_response](const drogon::HttpRequestPtr& /*req*/,
                                 std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        nlohmann::json j;
        j["status"]   = "ok";
        j["provider"] = config_.provider;
        j["threads"]  = config_.num_threads;
        callback(make_json_response(drogon::k200OK, j));
      },
      {drogon::Get});

  auto readiness_handler = [this, make_json_response](
                               const drogon::HttpRequestPtr& /*req*/,
                               std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    bool        vad_ready = false;
    std::string vad_error;
    try {
      VoiceActivityDetector vad(vad_config_);
      vad_ready = true;
    } catch (const std::exception& e) {
      vad_error = e.what();
    }

    const bool recognizer_ready = recognizer_.ready();
    const bool ready            = recognizer_ready && vad_ready;

    nlohmann::json j;
    j["status"]           = ready ? "ok" : "not_ready";
    j["provider"]         = config_.provider;
    j["recognizer_ready"] = recognizer_ready;
    j["vad_ready"]        = vad_ready;
    if (!vad_ready) {
      j["vad_model_path"]        = vad_config_.model_path;
      j["vad_model_file_exists"] = std::filesystem::exists(vad_config_.model_path);
      if (!vad_error.empty()) {
        j["vad_error"] = vad_error;
      }
    }

    callback(make_json_response(ready ? drogon::k200OK : drogon::k503ServiceUnavailable, j));
  };

  // Drogon's registerHandler template keeps lvalue callables as references;
  // pass an owning copy to avoid dangling after setup_http_handlers() returns.
  using ReadinessHandler = std::decay_t<decltype(readiness_handler)>;

  // GET /readyz — deep readiness check (recognizer + VAD runtime)
  app.registerHandler("/readyz", ReadinessHandler(readiness_handler), {drogon::Get});

  // Backward-compatible readiness alias.
  app.registerHandler("/health", ReadinessHandler(readiness_handler), {drogon::Get});

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

        auto request_loop = drogon::app().getLoop();
        auto callback_ptr =
            std::make_shared<std::function<void(const drogon::HttpResponsePtr&)>>(std::move(callback));
        auto upload_body = std::make_shared<std::string>(file.fileContent());
        auto file_name   = file.getFileName();

        bool submitted = false;
        try {
          submitted = g_asr_executor != nullptr && g_asr_executor->try_submit([this, start_ts, request_loop,
                                                                               callback_ptr, upload_body,
                                                                               file_name]() {
            auto make_async_error = [start_ts](drogon::HttpStatusCode status, const std::string& detail,
                                               const std::string& error_type) {
              nlohmann::json err;
              err["detail"] = detail;
              auto resp     = drogon::HttpResponse::newHttpResponse();
              resp->setStatusCode(status);
              resp->setBody(err.dump());
              resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
              auto& metrics = ASRMetrics::instance();
              metrics.observe_error(error_type);
              const auto end = std::chrono::steady_clock::now();
              metrics.observe_request(std::chrono::duration<double>(end - start_ts).count(), 0.0, 0.0, 0, 0,
                                      0.0, 0.0, "http", "failed");
              metrics.session_ended(0.0);
              g_request_sem.release();
              return resp;
            };

            try {
              auto file_bytes = asr::span<const uint8_t>(
                  reinterpret_cast<const uint8_t*>(upload_body->data()), upload_body->size());

              std::string           text;
              double                decode_sec = 0.0;
              std::optional<double> ttfr_sec;

              auto       pipeline_start = std::chrono::steady_clock::now();
              const auto audio          = decode_audio_streamed(
                  file_bytes, file_name, config_.sample_rate, http_chunk_samples(config_.sample_rate),
                  [this, &text, &decode_sec, &ttfr_sec, &start_ts](span<const float> chunk) {
                    auto t0       = std::chrono::steady_clock::now();
                    auto chunk_tx = recognizer_.recognize(chunk, config_.sample_rate);
                    auto t1       = std::chrono::steady_clock::now();
                    decode_sec += std::chrono::duration<double>(t1 - t0).count();
                    if (!ttfr_sec.has_value()) {
                      ttfr_sec = std::chrono::duration<double>(t1 - start_ts).count();
                    }
                    append_transcription_chunk(text, chunk_tx);
                  });
              auto         pipeline_end   = std::chrono::steady_clock::now();
              const double preprocess_sec = std::max(
                  0.0, std::chrono::duration<double>(pipeline_end - pipeline_start).count() - decode_sec);

              auto         end_ts    = std::chrono::steady_clock::now();
              const double total_sec = std::chrono::duration<double>(end_ts - start_ts).count();

              auto& metrics = ASRMetrics::instance();
              if (ttfr_sec.has_value()) {
                metrics.observe_ttfr(*ttfr_sec, "http");
              }
              metrics.observe_segment(static_cast<double>(audio.duration_sec), decode_sec);
              metrics.observe_request(total_sec, static_cast<double>(audio.duration_sec), decode_sec, 1,
                                      upload_body->size(), preprocess_sec, 0.0, "http", "success");
              metrics.record_result(text);
              metrics.session_ended(total_sec);

              nlohmann::json j;
              j["text"]     = text;
              j["duration"] = audio.duration_sec;
              auto resp     = drogon::HttpResponse::newHttpResponse();
              resp->setStatusCode(drogon::k200OK);
              resp->setBody(j.dump());
              resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);

              request_loop->queueInLoop([callback_ptr, resp]() { (*callback_ptr)(resp); });
            } catch (const RecognizerBusyError& e) {
              auto resp = make_async_error(drogon::k503ServiceUnavailable, e.what(), "capacity_exceeded");
              request_loop->queueInLoop([callback_ptr, resp]() { (*callback_ptr)(resp); });
              return;
            } catch (const AudioError& e) {
              auto resp = make_async_error(drogon::k400BadRequest, e.what(), "invalid_audio");
              request_loop->queueInLoop([callback_ptr, resp]() { (*callback_ptr)(resp); });
              return;
            } catch (const std::exception& e) {
              auto resp = make_async_error(drogon::k500InternalServerError, e.what(), "internal_error");
              request_loop->queueInLoop([callback_ptr, resp]() { (*callback_ptr)(resp); });
              return;
            } catch (...) {
              auto resp = make_async_error(drogon::k500InternalServerError, "Unknown internal error",
                                           "internal_error");
              request_loop->queueInLoop([callback_ptr, resp]() { (*callback_ptr)(resp); });
              return;
            }

            g_request_sem.release();
          });
        } catch (const std::exception& e) {
          submitted = false;
          spdlog::error("HTTP /recognize: failed to enqueue executor task: {}", e.what());
        } catch (...) {
          submitted = false;
          spdlog::error("HTTP /recognize: failed to enqueue executor task: unknown exception");
        }

        if (!submitted) {
          auto resp = make_error_and_release(drogon::k503ServiceUnavailable, "ASR executor queue is full",
                                             "capacity_exceeded");
          (*callback_ptr)(resp);
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
    const auto  it = std::find_if(files.begin(), files.end(),
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

    const std::string upload_file_name    = upload->getFileName();
    const auto&       upload_file_content = upload->fileContent();

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

    auto request_loop = drogon::app().getLoop();
    auto callback_ptr =
        std::make_shared<std::function<void(const drogon::HttpResponsePtr&)>>(std::move(callback));
    auto upload_body          = std::make_shared<std::string>(upload_file_content);
    auto upload_file_name_ptr = std::make_shared<const std::string>(upload_file_name);
    auto whisper_request_ptr  = std::make_shared<const WhisperTranscriptionRequest>(whisper_request);

    bool submitted = false;
    try {
      submitted =
          g_asr_executor != nullptr &&
          g_asr_executor->try_submit([this, start_ts, request_loop, callback_ptr, upload_body,
                                      upload_file_name_ptr, whisper_request_ptr]() {
            auto make_async_error = [start_ts](drogon::HttpStatusCode status, const std::string& detail,
                                               const std::string& metrics_error_type,
                                               const std::string& api_error_type,
                                               const std::string& api_param, const std::string& api_code) {
              auto resp = drogon::HttpResponse::newHttpResponse();
              resp->setStatusCode(status);
              resp->setBody(build_whisper_api_error_json(detail, api_error_type, api_param, api_code));
              resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
              auto& metrics = ASRMetrics::instance();
              metrics.observe_error(metrics_error_type);
              const auto end = std::chrono::steady_clock::now();
              metrics.observe_request(std::chrono::duration<double>(end - start_ts).count(), 0.0, 0.0, 0, 0,
                                      0.0, 0.0, "whisper_api", "failed");
              metrics.session_ended(0.0);
              g_request_sem.release();
              return resp;
            };

            try {
              auto file_bytes = asr::span<const uint8_t>(
                  reinterpret_cast<const uint8_t*>(upload_body->data()), upload_body->size());

              std::string           text;
              double                decode_sec = 0.0;
              std::optional<double> ttfr_sec;

              auto       pipeline_start = std::chrono::steady_clock::now();
              const auto audio          = decode_audio_streamed(
                  file_bytes, *upload_file_name_ptr, config_.sample_rate,
                  http_chunk_samples(config_.sample_rate),
                  [this, &text, &decode_sec, &ttfr_sec, &start_ts](span<const float> chunk) {
                    auto t0       = std::chrono::steady_clock::now();
                    auto chunk_tx = recognizer_.recognize(chunk, config_.sample_rate);
                    auto t1       = std::chrono::steady_clock::now();
                    decode_sec += std::chrono::duration<double>(t1 - t0).count();
                    if (!ttfr_sec.has_value()) {
                      ttfr_sec = std::chrono::duration<double>(t1 - start_ts).count();
                    }
                    append_transcription_chunk(text, chunk_tx);
                  });
              auto         pipeline_end   = std::chrono::steady_clock::now();
              const double preprocess_sec = std::max(
                  0.0, std::chrono::duration<double>(pipeline_end - pipeline_start).count() - decode_sec);

              auto         end_ts    = std::chrono::steady_clock::now();
              const double total_sec = std::chrono::duration<double>(end_ts - start_ts).count();

              auto& metrics = ASRMetrics::instance();
              if (ttfr_sec.has_value()) {
                metrics.observe_ttfr(*ttfr_sec, "whisper_api");
              }
              metrics.observe_segment(static_cast<double>(audio.duration_sec), decode_sec);
              metrics.observe_request(total_sec, static_cast<double>(audio.duration_sec), decode_sec, 1,
                                      upload_body->size(), preprocess_sec, 0.0, "whisper_api", "success");
              metrics.record_result(text);
              metrics.session_ended(total_sec);

              WhisperTranscriptionResponsePayload payload;
              payload.text         = text;
              payload.duration_sec = audio.duration_sec;
              payload.language     = canonical_whisper_response_language(whisper_request_ptr->language);

              auto rendered = render_whisper_transcription_response(*whisper_request_ptr, payload);
              auto resp     = drogon::HttpResponse::newHttpResponse();
              resp->setStatusCode(drogon::k200OK);
              resp->setBody(std::move(rendered.body));
              if (rendered.content_type == "application/json") {
                resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
              } else {
                resp->setContentTypeString(rendered.content_type);
              }

              request_loop->queueInLoop([callback_ptr, resp]() { (*callback_ptr)(resp); });
            } catch (const RecognizerBusyError& e) {
              auto resp = make_async_error(drogon::k503ServiceUnavailable, e.what(), "capacity_exceeded",
                                           "server_error", "", "capacity_exceeded");
              request_loop->queueInLoop([callback_ptr, resp]() { (*callback_ptr)(resp); });
              return;
            } catch (const AudioError& e) {
              auto resp = make_async_error(drogon::k400BadRequest, e.what(), "invalid_audio",
                                           "invalid_request_error", "file", "invalid_audio");
              request_loop->queueInLoop([callback_ptr, resp]() { (*callback_ptr)(resp); });
              return;
            } catch (const std::exception& e) {
              auto resp = make_async_error(drogon::k500InternalServerError, e.what(), "internal_error",
                                           "server_error", "", "internal_error");
              request_loop->queueInLoop([callback_ptr, resp]() { (*callback_ptr)(resp); });
              return;
            } catch (...) {
              auto resp = make_async_error(drogon::k500InternalServerError, "Unknown internal error",
                                           "internal_error", "server_error", "", "internal_error");
              request_loop->queueInLoop([callback_ptr, resp]() { (*callback_ptr)(resp); });
              return;
            }

            g_request_sem.release();
          });
    } catch (const std::exception& e) {
      submitted = false;
      spdlog::error("HTTP whisper_api: failed to enqueue executor task: {}", e.what());
    } catch (...) {
      submitted = false;
      spdlog::error("HTTP whisper_api: failed to enqueue executor task: unknown exception");
    }

    if (!submitted) {
      auto resp = make_error_and_release(drogon::k503ServiceUnavailable, "ASR executor queue is full",
                                         "capacity_exceeded", "server_error", "", "capacity_exceeded");
      (*callback_ptr)(resp);
    }
  };

  // Drogon's registerHandler template keeps lvalue callables as references;
  // pass an owning copy to avoid dangling after setup_http_handlers() returns.
  using WhisperHandler = std::decay_t<decltype(whisper_handler)>;
  app.registerHandler("/v1/audio/transcriptions", WhisperHandler(whisper_handler), {drogon::Post});
  app.registerHandler("/audio/transcriptions", WhisperHandler(whisper_handler), {drogon::Post});
}

void Server::setup_realtime_ws_handler() {
  // Realtime WebSocket controller is auto-registered via WS_PATH_ADD macro.
}

void Server::run() {
  install_signal_handlers();
  setup_http_handlers();
  setup_realtime_ws_handler();

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

  if (g_asr_executor) {
    g_asr_executor->shutdown();
    g_asr_executor.reset();
  }
  spdlog::info("Server stopped");
}

}  // namespace asr
