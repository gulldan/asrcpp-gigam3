#include <samplerate.h>
#include <spdlog/spdlog.h>

#include <cmath>

#include "asr/audio.h"
#include "asr/span.h"

namespace asr {

StreamResampler::StreamResampler(int input_rate, int output_rate)
    : ratio_(static_cast<double>(output_rate) / static_cast<double>(input_rate)) {
  int error = 0;
  state_    = src_new(SRC_SINC_MEDIUM_QUALITY, 1, &error);
  if (state_ == nullptr) {
    throw AudioError(std::string("Failed to create resampler: ") + src_strerror(error));
  }
}

StreamResampler::~StreamResampler() {
  if (state_ != nullptr) {
    src_delete(static_cast<SRC_STATE*>(state_));
  }
}

span<const float> StreamResampler::process(span<const float> input) {
  // Grow buffer to high-water mark only; after first growth, calls are allocation-free.
  auto needed = static_cast<size_t>(std::lround(static_cast<double>(input.size()) * ratio_)) + 16;
  if (output_buf_.size() < needed) {
    output_buf_.resize(needed);
  }

  SRC_DATA src_data{};
  src_data.data_in       = input.data();
  src_data.input_frames  = static_cast<long>(input.size());
  src_data.data_out      = output_buf_.data();
  src_data.output_frames = static_cast<long>(output_buf_.size());
  src_data.src_ratio     = ratio_;
  src_data.end_of_input  = 0;

  const int error = src_process(static_cast<SRC_STATE*>(state_), &src_data);
  if (error != 0) {
    throw AudioError(std::string("Resampling failed: ") + src_strerror(error));
  }

  if (src_data.input_frames_used != src_data.input_frames) {
    spdlog::warn("StreamResampler: consumed {}/{} input frames (output buffer may be too small)",
                 src_data.input_frames_used, src_data.input_frames);
  }

  return {output_buf_.data(), static_cast<size_t>(src_data.output_frames_gen)};
}

span<const float> StreamResampler::flush() {
  if (output_buf_.empty()) {
    output_buf_.resize(64);
  }

  static constexpr float kDummyInput = 0.0f;

  SRC_DATA src_data{};
  // input_frames=0: libsamplerate must not read data_in, but it still checks pointer overlap.
  src_data.data_in       = &kDummyInput;
  src_data.input_frames  = 0;
  src_data.data_out      = output_buf_.data();
  src_data.output_frames = static_cast<long>(output_buf_.size());
  src_data.src_ratio     = ratio_;
  src_data.end_of_input  = 1;

  const int error = src_process(static_cast<SRC_STATE*>(state_), &src_data);
  if (error != 0) {
    throw AudioError(std::string("Resampler flush failed: ") + src_strerror(error));
  }

  src_reset(static_cast<SRC_STATE*>(state_));
  return {output_buf_.data(), static_cast<size_t>(src_data.output_frames_gen)};
}

void StreamResampler::reset() {
  src_reset(static_cast<SRC_STATE*>(state_));
}

}  // namespace asr
