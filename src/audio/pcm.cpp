#include <cmath>
#include <cstdint>
#include <numeric>

#include "asr/audio.h"
#include "asr/span.h"

namespace asr {

std::vector<float> pcm16_to_float32(span<const uint8_t> pcm16_data) {
  std::vector<float> out;
  pcm16_to_float32_into(pcm16_data, out);
  return out;
}

void pcm16_to_float32_into(span<const uint8_t> pcm16_data, std::vector<float>& out) {
  out.clear();
  if (pcm16_data.empty()) {
    return;
  }
  if (pcm16_data.size() % 2U != 0U) {
    throw AudioError("Invalid PCM16 payload: byte count must be even");
  }

  out.resize(pcm16_data.size() / 2U);
  for (size_t i = 0; i < out.size(); ++i) {
    const uint8_t lo  = pcm16_data[i * 2U];
    const uint8_t hi  = pcm16_data[i * 2U + 1U];
    const auto sample = static_cast<int16_t>(static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8U));
    out[i]            = static_cast<float>(sample) / 32768.0F;
  }
}

float compute_rms(span<const float> samples) {
  if (samples.empty()) {
    return 0.0F;
  }

  const double sum_sq = std::accumulate(samples.begin(), samples.end(), 0.0, [](double acc, float s) {
    return acc + static_cast<double>(s) * static_cast<double>(s);
  });
  return static_cast<float>(std::sqrt(sum_sq / static_cast<double>(samples.size())));
}

}  // namespace asr
