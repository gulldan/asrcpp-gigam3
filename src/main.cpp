#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/common.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <utility>

#include "asr/config.h"
#include "asr/metrics.h"
#include "asr/recognizer.h"
#include "asr/server.h"

namespace {

void configure_logging() {
  constexpr size_t kLogQueueSize     = 8192;
  constexpr size_t kLogWorkerThreads = 1;

  spdlog::init_thread_pool(kLogQueueSize, kLogWorkerThreads);
  auto sink   = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  auto logger = std::make_shared<spdlog::async_logger>("asr", std::move(sink), spdlog::thread_pool(),
                                                       spdlog::async_overflow_policy::overrun_oldest);
  logger->set_pattern("%Y-%m-%d %H:%M:%S.%e [%^%l%$] %v");
  logger->set_level(spdlog::level::info);
  logger->flush_on(spdlog::level::warn);
  spdlog::set_default_logger(std::move(logger));
}

void shutdown_logging() noexcept {
  try {
    spdlog::shutdown();
  } catch (const std::exception& e) {
    std::fputs("Logging shutdown error: ", stderr);
    std::fputs(e.what(), stderr);
    std::fputc('\n', stderr);
  } catch (...) {
    std::fputs("Logging shutdown error: unknown exception\n", stderr);
  }
}

int run_server() {
  configure_logging();

  auto config = asr::Config::from_env();
  config.validate();

  spdlog::info("ASR Server v1.0.0 (C++)");
  spdlog::info("Loading GigaAM v3 model from {}...", config.model_dir);

  asr::Recognizer recognizer(config);
  spdlog::info("Model loaded. Provider: {}, threads: {}, pool_size: {}", config.provider, config.num_threads,
               config.recognizer_pool_size);
  spdlog::info("Runtime config: sample_rate={} idle_connection_timeout_sec={} max_ws_connections={}",
               config.sample_rate, config.idle_connection_timeout_sec, config.max_ws_connections);

  asr::ASRMetrics::instance().initialize();

  asr::Server server(config, recognizer);
  server.run();
  shutdown_logging();
  return 0;
}

}  // namespace

int main() {
  try {
    return run_server();
  } catch (const asr::ConfigError& e) {
    std::fprintf(stderr, "Configuration error: %s\n", e.what());
    shutdown_logging();
    return 2;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "Fatal error: %s\n", e.what());
    shutdown_logging();
    return 1;
  } catch (...) {
    std::fputs("Fatal error: unknown exception\n", stderr);
    shutdown_logging();
    return 1;
  }
}
