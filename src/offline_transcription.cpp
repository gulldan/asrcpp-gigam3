#include "asr/offline_transcription.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>

#include "asr/string_utils.h"

namespace asr {

std::string recognize_audio_chunked(span<const float> audio, int sample_rate, float max_chunk_sec,
                                    const RecognizeChunkFn& recognize_chunk) {
  if (sample_rate <= 0) {
    throw std::invalid_argument("sample_rate must be positive");
  }
  if (!recognize_chunk) {
    throw std::invalid_argument("recognize_chunk callback is empty");
  }
  if (audio.empty()) {
    return {};
  }

  if (max_chunk_sec <= 0.0f) {
    return trim_ascii(recognize_chunk(audio, sample_rate));
  }

  const auto chunk_samples =
      std::max<size_t>(1, static_cast<size_t>(std::floor(static_cast<double>(max_chunk_sec) * sample_rate)));

  std::string text;
  for (size_t offset = 0; offset < audio.size();) {
    const auto len   = std::min(chunk_samples, audio.size() - offset);
    auto       chunk = audio.subspan(offset, len);
    offset += len;

    auto chunk_text = trim_ascii(recognize_chunk(chunk, sample_rate));
    if (chunk_text.empty()) {
      continue;
    }

    if (!text.empty()) {
      text.push_back(' ');
    }
    text += chunk_text;
  }

  return text;
}

}  // namespace asr
