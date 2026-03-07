#pragma once

#include <functional>
#include <string>

#include "asr/span.h"

namespace asr {

using RecognizeChunkFn = std::function<std::string(span<const float> audio, int sample_rate)>;

std::string recognize_audio_chunked(span<const float> audio, int sample_rate, float max_chunk_sec,
                                    const RecognizeChunkFn& recognize_chunk);

}  // namespace asr
