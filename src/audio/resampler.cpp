#include <samplerate.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

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
  if (input.empty()) {
    output_buf_.clear();
    return {};
  }

  output_buf_.clear();

  size_t input_offset = 0;
  while (input_offset < input.size()) {
    const auto remaining = input.size() - input_offset;
    const auto chunk_out =
        std::max<size_t>(64, static_cast<size_t>(std::lround(static_cast<double>(remaining) * ratio_)) + 64);
    const auto old_size = output_buf_.size();
    output_buf_.resize(old_size + chunk_out);

    SRC_DATA src_data{};
    src_data.data_in       = input.data() + static_cast<ptrdiff_t>(input_offset);
    src_data.input_frames  = static_cast<long>(remaining);
    src_data.data_out      = output_buf_.data() + static_cast<ptrdiff_t>(old_size);
    src_data.output_frames = static_cast<long>(chunk_out);
    src_data.src_ratio     = ratio_;
    src_data.end_of_input  = 0;

    const int error = src_process(static_cast<SRC_STATE*>(state_), &src_data);
    if (error != 0) {
      throw AudioError(std::string("Resampling failed: ") + src_strerror(error));
    }

    output_buf_.resize(old_size + static_cast<size_t>(src_data.output_frames_gen));
    input_offset += static_cast<size_t>(src_data.input_frames_used);

    if (src_data.input_frames_used == 0 && src_data.output_frames_gen == 0) {
      throw AudioError("Resampler made no forward progress");
    }
  }

  return {output_buf_.data(), output_buf_.size()};
}

span<const float> StreamResampler::flush() {
  output_buf_.clear();
  output_buf_.reserve(std::max<size_t>(output_buf_.capacity(), 256));

  static constexpr float kDummyInput = 0.0f;
  for (;;) {
    const auto old_size = output_buf_.size();
    output_buf_.resize(old_size + 256);

    SRC_DATA src_data{};
    src_data.data_in       = &kDummyInput;
    src_data.input_frames  = 0;
    src_data.data_out      = output_buf_.data() + static_cast<ptrdiff_t>(old_size);
    src_data.output_frames = 256;
    src_data.src_ratio     = ratio_;
    src_data.end_of_input  = 1;

    const int error = src_process(static_cast<SRC_STATE*>(state_), &src_data);
    if (error != 0) {
      throw AudioError(std::string("Resampler flush failed: ") + src_strerror(error));
    }

    output_buf_.resize(old_size + static_cast<size_t>(src_data.output_frames_gen));
    if (src_data.output_frames_gen == 0) {
      break;
    }
  }

  src_reset(static_cast<SRC_STATE*>(state_));
  return {output_buf_.data(), output_buf_.size()};
}

void StreamResampler::reset() {
  src_reset(static_cast<SRC_STATE*>(state_));
}

}  // namespace asr
