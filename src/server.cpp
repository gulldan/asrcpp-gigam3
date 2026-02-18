#include "asr/server.h"

#include <drogon/WebSocketController.h>
#include <drogon/drogon.h>
#include <prometheus/registry.h>
#include <prometheus/text_serializer.h>
#include <spdlog/spdlog.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

#include "asr/audio.h"
#include "asr/config.h"
#include "asr/handler.h"
#include "asr/metrics.h"
#include "asr/recognizer.h"
#include "asr/span.h"

namespace asr {

// Shared state for WsController — initialized before server starts accepting connections
struct WsSharedState {
  Recognizer*   recognizer = nullptr;
  VadConfig     vad_config;
  const Config* config = nullptr;
};

namespace {
// Global shared state, set once before drogon starts, read-only after
WsSharedState g_ws_state;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
}  // namespace

struct WsConnectionContext {
  std::shared_ptr<ASRSession>           session;
  std::chrono::steady_clock::time_point connected_at;
  std::vector<float>                    audio_buf;  // reusable buffer for binary WS messages
  std::string                           close_reason = "normal";
  std::unique_ptr<StreamResampler>      resampler;
  bool                                  sample_rate_received = false;
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

    spdlog::info("WS: connection opened from {}", req->peerAddr().toIp());
    auto ctx = std::make_shared<WsConnectionContext>();
    ctx->session =
        std::make_shared<ASRSession>(*g_ws_state.recognizer, g_ws_state.vad_config, *g_ws_state.config);
    ctx->connected_at = std::chrono::steady_clock::now();
    conn->setContext(ctx);

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
        // Guard against oversized messages (DoS/OOM protection)
        if (msg.size() > g_ws_state.config->max_ws_message_bytes) {
          spdlog::warn("WS: message too large ({} bytes, limit {})", msg.size(),
                       g_ws_state.config->max_ws_message_bytes);
          ctx->close_reason = "message_too_large";
          conn->shutdown(drogon::CloseCode::kViolation, "Message too large");
          return;
        }
        // Binary: float32 audio samples — use memcpy to avoid alignment UB
        if (msg.size() < sizeof(float) || msg.size() % sizeof(float) != 0) {
          spdlog::warn("WS: invalid binary size {} bytes", msg.size());
          return;
        }
        const size_t num_samples = msg.size() / sizeof(float);
        ctx->audio_buf.resize(num_samples);  // no-op after first call (capacity reused)
        std::memcpy(ctx->audio_buf.data(), msg.data(), msg.size());
        if (ctx->resampler) {
          auto resampled = ctx->resampler->process(ctx->audio_buf);
          responses      = session->on_audio(resampled);
        } else {
          responses = session->on_audio(ctx->audio_buf);
        }
      } else if (type == drogon::WebSocketMessageType::Text) {
        // Parse sample_rate JSON message from client
        if (!ctx->sample_rate_received && !msg.empty() && msg.front() == '{') {
          try {
            auto j = nlohmann::json::parse(msg);
            if (j.contains("sample_rate")) {
              int input_rate = j["sample_rate"].get<int>();
              if (input_rate < 8000 || input_rate > 192000) {
                spdlog::warn("WS: invalid sample_rate {} (must be 8000..192000), ignoring", input_rate);
                return;
              }
              ctx->sample_rate_received = true;
              if (input_rate != g_ws_state.config->sample_rate) {
                ctx->resampler =
                    std::make_unique<StreamResampler>(input_rate, g_ws_state.config->sample_rate);
                spdlog::info("WS: resampling {} -> {} Hz", input_rate, g_ws_state.config->sample_rate);
              } else {
                spdlog::debug("WS: client sample rate matches target ({}), no resampling needed", input_rate);
              }
              return;
            }
          } catch (const nlohmann::json::exception&) {  // NOLINT(bugprone-empty-catch)
            // Not a valid JSON message, fall through to command handling
          }
        }
        if (msg == "RECOGNIZE") {
          // Flush resampler filter tail before finalizing
          if (ctx->resampler) {
            auto tail = ctx->resampler->flush();
            if (!tail.empty()) {
              for (const auto& r : session->on_audio(tail)) {
                conn->send(r.json, drogon::WebSocketMessageType::Text);
              }
            }
          }
          responses = session->on_recognize();
        } else if (msg == "RESET") {
          session->on_reset();
          // Reset resampler state so it can be reused for next session
          if (ctx->resampler) {
            ctx->resampler->flush();  // flush + internal reset
          }
          return;
        } else {
          spdlog::warn("WS: Unknown text message: {}", msg);
          return;
        }
      } else {
        return;
      }

      for (const auto& r : responses) {
        conn->send(r.json, drogon::WebSocketMessageType::Text);
      }
    } catch (const std::exception& e) {
      spdlog::error("WS: Exception in message handler: {}", e.what());
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
    const double duration =
        ctx ? std::chrono::duration<double>(std::chrono::steady_clock::now() - ctx->connected_at).count()
            : 0.0;
    const std::string& reason = ctx ? ctx->close_reason : "normal";
    spdlog::info("WS: connection closed (duration={:.1f}s, reason={})", duration, reason);
    ASRMetrics::instance().connection_closed(reason, duration);
  }

  WS_PATH_LIST_BEGIN
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wc++20-extensions"
  WS_PATH_ADD("/ws");
#pragma GCC diagnostic pop
  WS_PATH_LIST_END
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
          // Decode WAV
          auto         preprocess_start = std::chrono::steady_clock::now();
          auto         audio            = decode_wav(file_data, config_.sample_rate);
          auto         preprocess_end   = std::chrono::steady_clock::now();
          const double preprocess_sec =
              std::chrono::duration<double>(preprocess_end - preprocess_start).count();

          // Recognize
          auto         decode_start = std::chrono::steady_clock::now();
          auto         text         = recognizer_.recognize(audio.samples, config_.sample_rate);
          auto         decode_end   = std::chrono::steady_clock::now();
          const double decode_sec   = std::chrono::duration<double>(decode_end - decode_start).count();

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
      .setIdleConnectionTimeout(300);  // 5 min idle timeout for WebSocket connections

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
