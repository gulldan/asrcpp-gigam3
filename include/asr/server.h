#pragma once

#include <csignal>

#include "asr/vad.h"

namespace asr {
class Recognizer;
struct Config;
}  // namespace asr

namespace asr {

class Server {
 public:
  Server(const Config& config, Recognizer& recognizer);
  void run();

 private:
  void        setup_http_handlers();
  static void setup_ws_handler();
  static void install_signal_handlers();

  const Config& config_;
  Recognizer&   recognizer_;
  VadConfig     vad_config_;

 public:
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
  static volatile sig_atomic_t shutdown_requested_;
};

}  // namespace asr
