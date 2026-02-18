#include <spdlog/spdlog.h>

#include <exception>
#include <string>

#include "asr/config.h"
#include "asr/metrics.h"
#include "asr/recognizer.h"
#include "asr/server.h"

int main() {
  try {
    auto config = asr::Config::from_env();
    config.validate();

    spdlog::info("ASR Server v1.0.0 (C++)");
    spdlog::info("Loading GigaAM v3 model from {}...", config.model_dir);

    asr::Recognizer recognizer(config);
    spdlog::info("Model loaded. Provider: {}, threads: {}, pool_size: {}", config.provider,
                 config.num_threads, config.recognizer_pool_size);

    asr::ASRMetrics::instance().initialize();

    asr::Server server(config, recognizer);
    server.run();

  } catch (const asr::ConfigError& e) {
    spdlog::critical("Configuration error: {}", e.what());
    return 2;
  } catch (const std::exception& e) {
    spdlog::critical("Fatal error: {}", e.what());
    return 1;
  }

  return 0;
}
