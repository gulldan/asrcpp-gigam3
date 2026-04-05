// Microbenchmarks for decision-driven hot-path changes.
// Compares current vs candidate implementations for:
//   3. Realtime event serialization
//   6. live_chunk_ reserve strategy
//   10. ChunkEmitter compaction strategy (erase-compact vs ring buffer)

#include <_stdio.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <new>
#include <nlohmann/json.hpp>
#include <ratio>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,misc-use-anonymous-namespace)
static std::size_t         g_sink_bytes = 0;
static std::atomic<size_t> g_alloc_count{0};
static std::atomic<size_t> g_alloc_bytes{0};
static thread_local bool   g_counting_allocs = false;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,misc-use-anonymous-namespace)

void* operator new(std::size_t size) {
  if (g_counting_allocs) {
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
  if (g_counting_allocs) {
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

namespace {

using Clock = std::chrono::steady_clock;

struct BenchResult {
  double median_ns_per_iter = 0.0;
  double allocs_per_iter    = 0.0;
  double bytes_per_iter     = 0.0;
};

template <typename Fn>
BenchResult run_benchmark(int iterations, int repeats, Fn&& fn) {
  for (int i = 0; i < std::max(16, iterations / 32); ++i) {
    fn();
  }

  std::vector<double> samples_ns;
  samples_ns.reserve(static_cast<size_t>(repeats));

  std::size_t total_allocs = 0;
  std::size_t total_bytes  = 0;

  for (int repeat = 0; repeat < repeats; ++repeat) {
    g_alloc_count.store(0, std::memory_order_relaxed);
    g_alloc_bytes.store(0, std::memory_order_relaxed);
    g_counting_allocs = true;

    const auto start = Clock::now();
    for (int i = 0; i < iterations; ++i) {
      fn();
    }
    const auto end = Clock::now();

    g_counting_allocs = false;
    total_allocs += g_alloc_count.load(std::memory_order_relaxed);
    total_bytes += g_alloc_bytes.load(std::memory_order_relaxed);
    samples_ns.push_back(std::chrono::duration<double, std::nano>(end - start).count() /
                         static_cast<double>(iterations));
  }

  std::sort(samples_ns.begin(), samples_ns.end());
  BenchResult result;
  result.median_ns_per_iter = samples_ns[samples_ns.size() / 2U];
  result.allocs_per_iter    = static_cast<double>(total_allocs) / static_cast<double>(iterations * repeats);
  result.bytes_per_iter     = static_cast<double>(total_bytes) / static_cast<double>(iterations * repeats);
  return result;
}

void print_result(std::string_view label, const BenchResult& result) {
  std::printf("%-42.*s %10.1f ns/op  %7.3f alloc/op  %9.1f B/op\n", static_cast<int>(label.size()),
              label.data(), result.median_ns_per_iter, result.allocs_per_iter, result.bytes_per_iter);
}

void append_int64(std::string& out, int64_t value) {
  std::array<char, 32> buf{};
  const auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
  if (ec == std::errc()) {
    out.append(buf.data(), static_cast<size_t>(ptr - buf.data()));
  }
}

void append_json_escaped(std::string& out, std::string_view s) {
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
        if (static_cast<unsigned char>(c) < 0x20U) {
          std::array<char, 7> buf{};
          std::snprintf(buf.data(), buf.size(), "\\u%04x",
                        static_cast<unsigned>(static_cast<unsigned char>(c)));
          out.append(buf.data(), 6);
        } else {
          out.push_back(c);
        }
        break;
    }
  }
}

std::string current_event_speech_started(std::string_view event_id, std::string_view item_id,
                                         int64_t audio_start_ms) {
  nlohmann::json event;
  event["type"]           = "input_audio_buffer.speech_started";
  event["event_id"]       = event_id;
  event["audio_start_ms"] = audio_start_ms;
  event["item_id"]        = item_id;
  return event.dump();
}

std::string candidate_event_speech_started(std::string_view event_id, std::string_view item_id,
                                           int64_t audio_start_ms) {
  std::string out;
  out.reserve(128);
  out.append(R"({"type":"input_audio_buffer.speech_started","event_id":")");
  out.append(event_id);
  out.append(R"(","audio_start_ms":)");
  append_int64(out, audio_start_ms);
  out.append(R"(,"item_id":")");
  out.append(item_id);
  out.push_back('"');
  out.push_back('}');
  return out;
}

std::string current_event_speech_stopped(std::string_view event_id, std::string_view item_id,
                                         int64_t audio_end_ms) {
  nlohmann::json event;
  event["type"]         = "input_audio_buffer.speech_stopped";
  event["event_id"]     = event_id;
  event["audio_end_ms"] = audio_end_ms;
  event["item_id"]      = item_id;
  return event.dump();
}

std::string candidate_event_speech_stopped(std::string_view event_id, std::string_view item_id,
                                           int64_t audio_end_ms) {
  std::string out;
  out.reserve(128);
  out.append(R"({"type":"input_audio_buffer.speech_stopped","event_id":")");
  out.append(event_id);
  out.append(R"(","audio_end_ms":)");
  append_int64(out, audio_end_ms);
  out.append(R"(,"item_id":")");
  out.append(item_id);
  out.push_back('"');
  out.push_back('}');
  return out;
}

std::string current_event_buffer_committed(std::string_view event_id, std::string_view item_id,
                                           std::string_view previous_item_id) {
  nlohmann::json event;
  event["type"]     = "input_audio_buffer.committed";
  event["event_id"] = event_id;
  event["item_id"]  = item_id;
  if (previous_item_id.empty()) {
    event["previous_item_id"] = nullptr;
  } else {
    event["previous_item_id"] = previous_item_id;
  }
  return event.dump();
}

std::string candidate_event_buffer_committed(std::string_view event_id, std::string_view item_id,
                                             std::string_view previous_item_id) {
  std::string out;
  out.reserve(144);
  out.append(R"({"type":"input_audio_buffer.committed","event_id":")");
  out.append(event_id);
  out.append(R"(","item_id":")");
  out.append(item_id);
  out.append(R"(","previous_item_id":)");
  if (previous_item_id.empty()) {
    out.append("null");
  } else {
    out.push_back('"');
    out.append(previous_item_id);
    out.push_back('"');
  }
  out.push_back('}');
  return out;
}

std::string current_event_buffer_cleared(std::string_view event_id) {
  nlohmann::json event;
  event["type"]     = "input_audio_buffer.cleared";
  event["event_id"] = event_id;
  return event.dump();
}

std::string candidate_event_buffer_cleared(std::string_view event_id) {
  std::string out;
  out.reserve(96);
  out.append(R"({"type":"input_audio_buffer.cleared","event_id":")");
  out.append(event_id);
  out.push_back('"');
  out.push_back('}');
  return out;
}

std::string current_event_transcription_completed(std::string_view event_id, std::string_view item_id,
                                                  std::string_view transcript) {
  nlohmann::json event;
  event["type"]          = "conversation.item.input_audio_transcription.completed";
  event["event_id"]      = event_id;
  event["item_id"]       = item_id;
  event["content_index"] = 0;
  event["transcript"]    = transcript;
  return event.dump();
}

std::string candidate_event_transcription_completed(std::string_view event_id, std::string_view item_id,
                                                    std::string_view transcript) {
  std::string out;
  out.reserve(160 + transcript.size());
  out.append(R"({"type":"conversation.item.input_audio_transcription.completed","event_id":")");
  out.append(event_id);
  out.append(R"(","item_id":")");
  out.append(item_id);
  out.append(R"(","content_index":0,"transcript":")");
  append_json_escaped(out, transcript);
  out.push_back('"');
  out.push_back('}');
  return out;
}

struct LiveChunkMetrics {
  std::size_t max_capacity = 0;
  std::size_t final_size   = 0;
};

struct ChunkEmitterMetrics {
  std::size_t max_capacity         = 0;
  std::size_t emitted_samples      = 0;
  std::size_t flush_linearizations = 0;
};

LiveChunkMetrics simulate_live_chunk(bool reserve_first_append) {
  constexpr int            kChunkSamples   = 1024;
  constexpr int            kTargetSamples  = 16000 * 6;
  constexpr int            kSessionRepeats = 2000;
  const std::vector<float> input(static_cast<size_t>(kChunkSamples), 0.1F);
  LiveChunkMetrics         metrics;

  for (int session = 0; session < kSessionRepeats; ++session) {
    std::vector<float> live_chunk;
    for (int appended = 0; appended < kTargetSamples; appended += kChunkSamples) {
      if (reserve_first_append && live_chunk.empty()) {
        live_chunk.reserve(static_cast<size_t>(kTargetSamples));
      }
      live_chunk.insert(live_chunk.end(), input.begin(), input.end());
      metrics.max_capacity = std::max(metrics.max_capacity, live_chunk.capacity());
    }
    metrics.final_size += live_chunk.size();
    g_sink_bytes += live_chunk.size();
  }

  return metrics;
}

template <typename CompactFn>
ChunkEmitterMetrics simulate_chunk_emitter_vector(CompactFn compact) {
  constexpr std::size_t kReadBlockSamples = 8192;
  constexpr std::size_t kChunkSamples     = 320000;                    // 20 s at 16 kHz
  constexpr std::size_t kTotalSamples     = 16000ULL * 60ULL * 10ULL;  // 10 minutes

  std::array<float, kReadBlockSamples> input{};
  input.fill(0.05F);

  ChunkEmitterMetrics metrics;
  std::vector<float>  buffer;
  std::size_t         offset = 0;

  for (std::size_t produced = 0; produced < kTotalSamples; produced += kReadBlockSamples) {
    const auto remaining     = kTotalSamples - produced;
    const auto block_samples = std::min(kReadBlockSamples, remaining);
    buffer.insert(buffer.end(), input.begin(), input.begin() + static_cast<std::ptrdiff_t>(block_samples));
    while (buffer.size() - offset >= kChunkSamples) {
      g_sink_bytes += static_cast<std::size_t>(buffer[offset] > 0.0F);
      metrics.emitted_samples += kChunkSamples;
      offset += kChunkSamples;
    }
    compact(buffer, offset);
    metrics.max_capacity = std::max(metrics.max_capacity, buffer.capacity());
  }

  const auto tail = buffer.size() - offset;
  if (tail > 0) {
    g_sink_bytes += static_cast<std::size_t>(buffer[offset] > 0.0F);
    metrics.emitted_samples += tail;
  }

  g_sink_bytes += metrics.emitted_samples;
  return metrics;
}

ChunkEmitterMetrics simulate_chunk_emitter_ring() {
  constexpr std::size_t kReadBlockSamples = 8192;
  constexpr std::size_t kChunkSamples     = 320000;                    // 20 s at 16 kHz
  constexpr std::size_t kTotalSamples     = 16000ULL * 60ULL * 10ULL;  // 10 minutes
  constexpr std::size_t kCapacitySamples  = kChunkSamples * 2U;

  std::array<float, kReadBlockSamples> input{};
  input.fill(0.05F);

  ChunkEmitterMetrics metrics;
  std::vector<float>  storage(kCapacitySamples);
  std::vector<float>  linearized_tail;
  linearized_tail.reserve(kChunkSamples);
  std::size_t head = 0;
  std::size_t size = 0;

  metrics.max_capacity = storage.capacity();

  for (std::size_t produced = 0; produced < kTotalSamples; produced += kReadBlockSamples) {
    const auto remaining     = kTotalSamples - produced;
    const auto block_samples = std::min(kReadBlockSamples, remaining);
    if (block_samples > storage.size() - size) {
      std::abort();
    }

    const auto tail       = (head + size) % storage.size();
    const auto first_part = std::min(block_samples, storage.size() - tail);
    std::copy_n(input.data(), static_cast<std::ptrdiff_t>(first_part), storage.data() + tail);
    if (block_samples > first_part) {
      std::copy_n(input.data() + static_cast<std::ptrdiff_t>(first_part),
                  static_cast<std::ptrdiff_t>(block_samples - first_part), storage.data());
    }
    size += block_samples;

    while (size >= kChunkSamples) {
      g_sink_bytes += static_cast<std::size_t>(storage[head] > 0.0F);
      metrics.emitted_samples += kChunkSamples;
      head = (head + kChunkSamples) % storage.size();
      size -= kChunkSamples;
    }
  }

  if (size == 0) {
    g_sink_bytes += metrics.emitted_samples;
    return metrics;
  }

  if (head + size <= storage.size()) {
    g_sink_bytes += static_cast<std::size_t>(storage[head] > 0.0F);
    metrics.emitted_samples += size;
  } else {
    linearized_tail.resize(size);
    const auto first_part = storage.size() - head;
    std::copy_n(storage.data() + static_cast<std::ptrdiff_t>(head), static_cast<std::ptrdiff_t>(first_part),
                linearized_tail.data());
    std::copy_n(storage.data(), static_cast<std::ptrdiff_t>(size - first_part),
                linearized_tail.data() + static_cast<std::ptrdiff_t>(first_part));
    g_sink_bytes += static_cast<std::size_t>(linearized_tail[0] > 0.0F);
    metrics.emitted_samples += size;
    metrics.flush_linearizations += 1;
  }

  g_sink_bytes += metrics.emitted_samples;
  return metrics;
}

void current_compact(std::vector<float>& buffer, std::size_t& offset) {
  if (offset > 0 && offset >= buffer.size() / 2U) {
    buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(offset));
    offset = 0;
  }
}

}  // namespace

int run_benchmarks() {
  std::printf("=== Hot-path Decision Benchmarks ===\n\n");

  constexpr std::string_view kEventId    = "evt_123456";
  constexpr std::string_view kItemId     = "item_987654";
  constexpr std::string_view kPrevItemId = "item_987653";
  constexpr std::string_view kTranscript = "Privet mir, eto finalnaya transkriptsiya bez dom-alloc path.";
  constexpr int              kEventIterations = 200000;
  constexpr int              kRepeats         = 7;

  std::printf("--- Point 3: Realtime event builders ---\n");
  print_result("current speech_started", run_benchmark(kEventIterations, kRepeats, [=] {
                 auto payload = current_event_speech_started(kEventId, kItemId, 1240);
                 g_sink_bytes += payload.size();
               }));
  print_result("candidate speech_started", run_benchmark(kEventIterations, kRepeats, [=] {
                 auto payload = candidate_event_speech_started(kEventId, kItemId, 1240);
                 g_sink_bytes += payload.size();
               }));
  print_result("current speech_stopped", run_benchmark(kEventIterations, kRepeats, [=] {
                 auto payload = current_event_speech_stopped(kEventId, kItemId, 1815);
                 g_sink_bytes += payload.size();
               }));
  print_result("candidate speech_stopped", run_benchmark(kEventIterations, kRepeats, [=] {
                 auto payload = candidate_event_speech_stopped(kEventId, kItemId, 1815);
                 g_sink_bytes += payload.size();
               }));
  print_result("current buffer_committed(null prev)", run_benchmark(kEventIterations, kRepeats, [=] {
                 auto payload = current_event_buffer_committed(kEventId, kItemId, "");
                 g_sink_bytes += payload.size();
               }));
  print_result("candidate buffer_committed(null prev)", run_benchmark(kEventIterations, kRepeats, [=] {
                 auto payload = candidate_event_buffer_committed(kEventId, kItemId, "");
                 g_sink_bytes += payload.size();
               }));
  print_result("current buffer_committed(with prev)", run_benchmark(kEventIterations, kRepeats, [=] {
                 auto payload = current_event_buffer_committed(kEventId, kItemId, kPrevItemId);
                 g_sink_bytes += payload.size();
               }));
  print_result("candidate buffer_committed(with prev)", run_benchmark(kEventIterations, kRepeats, [=] {
                 auto payload = candidate_event_buffer_committed(kEventId, kItemId, kPrevItemId);
                 g_sink_bytes += payload.size();
               }));
  print_result("current buffer_cleared", run_benchmark(kEventIterations, kRepeats, [=] {
                 auto payload = current_event_buffer_cleared(kEventId);
                 g_sink_bytes += payload.size();
               }));
  print_result("candidate buffer_cleared", run_benchmark(kEventIterations, kRepeats, [=] {
                 auto payload = candidate_event_buffer_cleared(kEventId);
                 g_sink_bytes += payload.size();
               }));
  print_result("current transcription_completed", run_benchmark(kEventIterations, kRepeats, [=] {
                 auto payload = current_event_transcription_completed(kEventId, kItemId, kTranscript);
                 g_sink_bytes += payload.size();
               }));
  print_result("candidate transcription_completed", run_benchmark(kEventIterations, kRepeats, [=] {
                 auto payload = candidate_event_transcription_completed(kEventId, kItemId, kTranscript);
                 g_sink_bytes += payload.size();
               }));

  std::printf("\n--- Point 6: live_chunk_ reserve strategy ---\n");
  auto       current_live          = run_benchmark(1, kRepeats, [] {
    const auto metrics = simulate_live_chunk(false);
    g_sink_bytes += metrics.max_capacity + metrics.final_size;
  });
  auto       reserved_live         = run_benchmark(1, kRepeats, [] {
    const auto metrics = simulate_live_chunk(true);
    g_sink_bytes += metrics.max_capacity + metrics.final_size;
  });
  const auto current_live_metrics  = simulate_live_chunk(false);
  const auto reserved_live_metrics = simulate_live_chunk(true);
  print_result("current live_chunk growth", current_live);
  print_result("candidate first-append reserve", reserved_live);
  std::printf("  current peak capacity:   %zu samples (%.1f KiB)\n", current_live_metrics.max_capacity,
              (static_cast<double>(current_live_metrics.max_capacity) * sizeof(float)) / 1024.0);
  std::printf("  reserved peak capacity:  %zu samples (%.1f KiB)\n", reserved_live_metrics.max_capacity,
              (static_cast<double>(reserved_live_metrics.max_capacity) * sizeof(float)) / 1024.0);
  std::printf("  idle connection memory if reserved eagerly in reset_session(): %.1f KiB\n",
              (static_cast<double>(16000 * 6) * sizeof(float)) / 1024.0);

  std::printf("\n--- Point 10: ChunkEmitter compaction ---\n");
  auto       current_chunker         = run_benchmark(1, kRepeats, [] {
    const auto metrics = simulate_chunk_emitter_vector(current_compact);
    g_sink_bytes += metrics.max_capacity + metrics.flush_linearizations;
  });
  auto       ring_chunker            = run_benchmark(1, kRepeats, [] {
    const auto metrics = simulate_chunk_emitter_ring();
    g_sink_bytes += metrics.max_capacity + metrics.flush_linearizations;
  });
  const auto current_chunker_metrics = simulate_chunk_emitter_vector(current_compact);
  const auto ring_chunker_metrics    = simulate_chunk_emitter_ring();
  print_result("current erase-based compact", current_chunker);
  print_result("candidate ring buffer (2x chunk)", ring_chunker);
  std::printf("  current peak capacity:   %zu samples (%.1f KiB)\n", current_chunker_metrics.max_capacity,
              (static_cast<double>(current_chunker_metrics.max_capacity) * sizeof(float)) / 1024.0);
  std::printf("  ring fixed capacity:     %zu samples (%.1f KiB)\n", ring_chunker_metrics.max_capacity,
              (static_cast<double>(ring_chunker_metrics.max_capacity) * sizeof(float)) / 1024.0);
  std::printf("  ring tail linearization: %zu\n", ring_chunker_metrics.flush_linearizations);

  std::printf("\nSink guard: %zu\n", g_sink_bytes);
  return 0;
}

int main() {
  try {
    return run_benchmarks();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "bench_decisions failed: %s\n", e.what());
    return 1;
  } catch (...) {
    std::fputs("bench_decisions failed: unknown exception\n", stderr);
    return 1;
  }
}
