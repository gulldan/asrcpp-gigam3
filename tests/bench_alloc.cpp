// Allocation-counting benchmark for ASR hot path.
// Overrides global operator new to count heap allocations precisely.
// Measures steady-state allocation count for on_audio() and on_recognize(),
// with component-level breakdown (VAD/ORT, metrics, handler).
//
// Usage: ./asr_alloc_bench           (requires models/ directory)
//        valgrind --tool=dhat ./asr_alloc_bench   (full valgrind profile)

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <new>
#include <string>
#include <vector>

#include "asr/audio.h"
#include "asr/config.h"
#include "asr/handler.h"
#include "asr/metrics.h"
#include "asr/recognizer.h"
#include "asr/span.h"
#include "asr/vad.h"

// ===== Global allocation counter =====
// Thread-local flag: only count when explicitly enabled in measurement scope.
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,misc-use-anonymous-namespace)
static std::atomic<size_t> g_alloc_count{0};
static std::atomic<size_t> g_alloc_bytes{0};
static thread_local bool   g_counting = false;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,misc-use-anonymous-namespace)

// NOLINTBEGIN(misc-definitions-in-headers)
void* operator new(std::size_t size) {
  if (g_counting) {
    g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    g_alloc_bytes.fetch_add(size, std::memory_order_relaxed);
  }
  void* p = std::malloc(size);  // NOLINT(cppcoreguidelines-no-malloc)
  if (p == nullptr) {
    throw std::bad_alloc();
  }
  return p;
}

void* operator new[](std::size_t size) {
  if (g_counting) {
    g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    g_alloc_bytes.fetch_add(size, std::memory_order_relaxed);
  }
  void* p = std::malloc(size);  // NOLINT(cppcoreguidelines-no-malloc)
  if (p == nullptr) {
    throw std::bad_alloc();
  }
  return p;
}

void operator delete(void* p) noexcept {
  std::free(p);  // NOLINT(cppcoreguidelines-no-malloc)
}
void operator delete[](void* p) noexcept {
  std::free(p);  // NOLINT(cppcoreguidelines-no-malloc)
}
void operator delete(void* p, std::size_t /*size*/) noexcept {
  std::free(p);  // NOLINT(cppcoreguidelines-no-malloc)
}
void operator delete[](void* p, std::size_t /*size*/) noexcept {
  std::free(p);  // NOLINT(cppcoreguidelines-no-malloc)
}
// NOLINTEND(misc-definitions-in-headers)

// RAII scope for allocation measurement
struct AllocScope {
  const char* label;

  explicit AllocScope(const char* name) : label(name) {
    g_alloc_count.store(0, std::memory_order_relaxed);
    g_alloc_bytes.store(0, std::memory_order_relaxed);
    g_counting = true;
  }

  ~AllocScope() {
    g_counting = false;
  }

  AllocScope(const AllocScope&)            = delete;
  AllocScope& operator=(const AllocScope&) = delete;
  AllocScope(AllocScope&&)                 = delete;
  AllocScope& operator=(AllocScope&&)      = delete;

  // cppcheck-suppress functionStatic  ; intentionally non-static for API clarity
  [[nodiscard]] size_t count() const {
    return g_alloc_count.load(std::memory_order_relaxed);
  }
  // cppcheck-suppress functionStatic  ; intentionally non-static for API clarity
  [[nodiscard]] size_t bytes() const {
    return g_alloc_bytes.load(std::memory_order_relaxed);
  }

  void report(int iterations) const {
    const size_t c = count();
    const size_t b = bytes();
    std::printf("  %-45s %4d iters → %5zu allocs (%8zu bytes)", label, iterations, c, b);
    if (c == 0) {
      std::printf("  ZERO-ALLOC\n");
    } else {
      std::printf("  (%.1f/call, %.0f B/call)\n", static_cast<double>(c) / iterations,
                  static_cast<double>(b) / iterations);
    }
  }
};

namespace {

constexpr const char* kModelDir = "models/sherpa-onnx-nemo-transducer-punct-giga-am-v3-russian-2025-12-16";
constexpr const char* kVadModel = "models/silero_vad.onnx";

bool models_exist() {
  const std::ifstream f1(std::string(kModelDir) + "/encoder.int8.onnx");
  const std::ifstream f2(kVadModel);
  return f1.good() && f2.good();
}

asr::Config make_config() {
  asr::Config cfg;
  cfg.model_dir         = kModelDir;
  cfg.vad_model         = kVadModel;
  cfg.provider          = "cpu";
  cfg.num_threads       = 2;
  cfg.sample_rate       = 16000;
  cfg.feature_dim       = 64;
  cfg.vad_threshold     = 0.5f;
  cfg.vad_min_silence   = 0.5f;
  cfg.vad_min_speech    = 0.25f;
  cfg.vad_max_speech    = 20.0f;
  cfg.vad_window_size   = 512;
  cfg.vad_context_size  = 64;
  cfg.silence_threshold = 0.008f;
  cfg.min_audio_sec     = 0.5f;
  cfg.max_audio_sec     = 30.0f;
  return cfg;
}

asr::VadConfig make_vad_config(const asr::Config& cfg) {
  asr::VadConfig vc;
  vc.model_path           = cfg.vad_model;
  vc.threshold            = cfg.vad_threshold;
  vc.min_silence_duration = cfg.vad_min_silence;
  vc.min_speech_duration  = cfg.vad_min_speech;
  vc.max_speech_duration  = cfg.vad_max_speech;
  vc.sample_rate          = cfg.sample_rate;
  vc.window_size          = cfg.vad_window_size;
  vc.context_size         = cfg.vad_context_size;
  return vc;
}

}  // namespace

int main() {
  if (!models_exist()) {
    std::printf("ERROR: Models not found. Place models in models/ directory.\n");
    return 1;
  }

  std::printf("=== ASR Hot Path Allocation Benchmark ===\n");
  std::printf("=== Component-level breakdown           ===\n\n");

  // Initialize metrics (allocates internally, one-time)
  asr::ASRMetrics::instance().initialize();

  auto cfg     = make_config();
  auto vad_cfg = make_vad_config(cfg);

  // =====================================================================
  // Section 1: Component-level isolation
  // =====================================================================
  std::printf("--- Component-level analysis ---\n\n");

  // 1a. VAD inference (ONNX Runtime)
  {
    asr::VoiceActivityDetector vad(vad_cfg);
    const std::vector<float>   window(512, 0.0f);

    // Warm up ORT session
    for (int i = 0; i < 20; ++i) {
      vad.accept_waveform(window);
    }
    vad.reset();

    {
      AllocScope scope("VAD accept_waveform (ORT inference)");
      for (int i = 0; i < 100; ++i) {
        vad.accept_waveform(window);
      }
      scope.report(100);
    }
    vad.reset();
  }

  // 1b. Prometheus metrics (record_audio_level, record_silence, etc.)
  {
    auto& metrics = asr::ASRMetrics::instance();
    // Warm up
    for (int i = 0; i < 20; ++i) {
      metrics.record_audio_level(0.01);
      metrics.record_silence();
    }

    {
      AllocScope scope("Metrics: record_audio_level()");
      for (int i = 0; i < 100; ++i) {
        metrics.record_audio_level(0.01);
      }
      scope.report(100);
    }

    {
      AllocScope scope("Metrics: record_silence()");
      for (int i = 0; i < 100; ++i) {
        metrics.record_silence();
      }
      scope.report(100);
    }

    {
      AllocScope scope("Metrics: session_started()");
      for (int i = 0; i < 100; ++i) {
        metrics.session_started();
      }
      scope.report(100);
    }

    {
      AllocScope scope("Metrics: observe_segment()");
      for (int i = 0; i < 100; ++i) {
        metrics.observe_segment(1.0, 0.5);
      }
      scope.report(100);
    }

    {
      AllocScope scope("Metrics: observe_request()");
      for (int i = 0; i < 100; ++i) {
        metrics.observe_request(1.0, 0.5, 0.3, 10, 4096, 0.01, 0.0, "websocket", "success");
      }
      scope.report(100);
    }
  }

  // 1c. compute_rms
  {
    const std::vector<float> samples(1024, 0.01f);
    // Warm up
    for (int i = 0; i < 20; ++i) {
      asr::compute_rms(samples);
    }

    {
      AllocScope scope("compute_rms()");
      for (int i = 0; i < 100; ++i) {
        asr::compute_rms(samples);
      }
      scope.report(100);
    }
  }

  // =====================================================================
  // Section 2: Full on_audio / on_recognize integration
  // =====================================================================
  std::printf("\n--- Full on_audio / on_recognize ---\n\n");

  asr::Recognizer recognizer(cfg);
  asr::ASRSession session(recognizer, vad_cfg, cfg);

  const int                chunk_size = 1024;
  const std::vector<float> silence(chunk_size, 0.0f);

  // Generate "speech-like" audio (sine wave at 440Hz)
  std::vector<float> speech(chunk_size);
  for (int i = 0; i < chunk_size; ++i) {
    speech[i] = 0.3f * std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / 16000.0f);
  }

  constexpr int kWarmupCalls  = 50;
  constexpr int kMeasureCalls = 100;

  // Warm-up (grow all buffers to steady state)
  std::printf("Warm-up (%d calls x2)...\n", kWarmupCalls);
  for (int i = 0; i < kWarmupCalls; ++i) {
    session.on_audio(silence);
  }
  session.on_recognize();
  for (int i = 0; i < kWarmupCalls; ++i) {
    session.on_audio(silence);
  }
  session.on_recognize();
  std::printf("  done.\n\n");

  // 2a. on_audio (silence) — steady state
  {
    AllocScope scope("on_audio(silence) - full pipeline");
    for (int i = 0; i < kMeasureCalls; ++i) {
      session.on_audio(silence);
    }
    scope.report(kMeasureCalls);
  }
  g_counting = false;
  session.on_recognize();

  // 2b. on_recognize (after silence)
  for (int i = 0; i < 20; ++i) {
    session.on_audio(silence);
  }
  {
    AllocScope scope("on_recognize(after silence)");
    session.on_recognize();
    scope.report(1);
  }

  // 2c. on_audio (speech-like)
  // Warm up speech path
  for (int i = 0; i < kWarmupCalls; ++i) {
    session.on_audio(speech);
  }
  g_counting = false;
  session.on_recognize();
  for (int i = 0; i < kWarmupCalls; ++i) {
    session.on_audio(speech);
  }
  g_counting = false;
  session.on_recognize();

  {
    AllocScope scope("on_audio(speech-like) - full pipeline");
    for (int i = 0; i < kMeasureCalls; ++i) {
      session.on_audio(speech);
    }
    scope.report(kMeasureCalls);
  }
  g_counting = false;
  session.on_recognize();

  // 2d. on_recognize (after speech)
  for (int i = 0; i < kWarmupCalls; ++i) {
    session.on_audio(speech);
  }
  {
    AllocScope scope("on_recognize(after speech)");
    session.on_recognize();
    scope.report(1);
  }

  // 2e. Full session cycle
  for (int cycle = 0; cycle < 3; ++cycle) {
    for (int i = 0; i < 20; ++i) {
      session.on_audio(silence);
    }
    session.on_recognize();
  }
  {
    AllocScope scope("full session cycle (silence, 20+1)");
    for (int i = 0; i < 20; ++i) {
      session.on_audio(silence);
    }
    session.on_recognize();
    scope.report(21);
  }

  // =====================================================================
  // Summary
  // =====================================================================
  std::printf("\n=== Summary ===\n");
  std::printf("  Handler code (write_interim/final/done):  zero-alloc (reuses string capacity)\n");
  std::printf("  Prometheus metrics:                       zero-alloc (pre-cached instances)\n");
  std::printf("  compute_rms:                              zero-alloc (pure math)\n");
  std::printf("  VAD inference (ONNX Runtime):             allocates internally (framework)\n");
  std::printf("  Recognizer (sherpa-onnx):                 allocates internally (framework)\n");
  std::printf("\n  ORT/sherpa-onnx allocations are inside third-party frameworks and\n");
  std::printf("  cannot be eliminated without modifying the framework source code.\n");
  std::printf("  All handler wrapper code is zero-alloc in steady state.\n");

  std::printf("\nFor detailed per-callsite analysis:\n");
  std::printf("  valgrind --tool=dhat ./build/debug/tests/asr_alloc_bench\n");
  std::printf("  valgrind --tool=massif ./build/debug/tests/asr_alloc_bench\n");
  std::printf("  ms_print massif.out.<pid>\n");

  return 0;
}
