#define DR_WAV_IMPLEMENTATION
#include "asr/audio.h"

#include <samplerate.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <cstring>
#include <utility>

#include "asr/span.h"
#include "dr_wav.h"

namespace asr {

AudioData decode_wav(span<const uint8_t> data, int target_rate) {
  if (data.empty()) {
    throw AudioError("Empty audio data");
  }

  drwav wav;
  if (drwav_init_memory(&wav, data.data(), data.size(), nullptr) == 0u) {
    throw AudioError("Failed to decode WAV file: invalid format");
  }

  if (wav.channels != 1) {
    drwav_uninit(&wav);
    throw AudioError("Only mono audio is supported, got " + std::to_string(wav.channels) + " channels");
  }

  auto total_frames = static_cast<size_t>(wav.totalPCMFrameCount);
  if (total_frames == 0) {
    drwav_uninit(&wav);
    throw AudioError("WAV file contains no audio frames");
  }

  // Guard against absurdly long audio (> 1 hour at any sample rate up to 48kHz)
  if (total_frames > 48000ULL * 3600ULL) {
    drwav_uninit(&wav);
    throw AudioError("WAV file too long: " + std::to_string(total_frames) + " frames exceeds 1-hour limit");
  }

  // Read all frames as float32 (dr_wav handles int16/int32/float conversion)
  std::vector<float> samples(total_frames);
  auto               frames_read = drwav_read_pcm_frames_f32(&wav, total_frames, samples.data());
  auto               input_rate  = static_cast<int>(wav.sampleRate);
  drwav_uninit(&wav);

  if (frames_read == 0) {
    throw AudioError("Failed to read PCM frames from WAV");
  }
  samples.resize(static_cast<size_t>(frames_read));

  // Resample if needed
  if (input_rate != target_rate) {
    const double ratio = static_cast<double>(target_rate) / static_cast<double>(input_rate);
    auto output_frames = static_cast<size_t>(std::lround(static_cast<double>(samples.size()) * ratio));
    std::vector<float> resampled(output_frames);

    SRC_DATA src_data{};
    src_data.data_in       = samples.data();
    src_data.input_frames  = static_cast<long>(samples.size());
    src_data.data_out      = resampled.data();
    src_data.output_frames = static_cast<long>(output_frames);
    src_data.src_ratio     = ratio;

    const int error = src_simple(&src_data, SRC_SINC_BEST_QUALITY, 1);
    if (error != 0) {
      throw AudioError(std::string("Resampling failed: ") + src_strerror(error));
    }

    resampled.resize(static_cast<size_t>(src_data.output_frames_gen));
    samples = std::move(resampled);
  }

  const float duration = static_cast<float>(samples.size()) / static_cast<float>(target_rate);
  return AudioData{std::move(samples), duration};
}

// --- StreamResampler ---

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
  // Grow buffer to high-water mark only — never shrink, so zero-alloc after first call.
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

  SRC_DATA src_data{};
  src_data.data_in       = output_buf_.data();  // valid pointer; input_frames=0 means no reads
  src_data.input_frames  = 0;
  src_data.data_out      = output_buf_.data();
  src_data.output_frames = static_cast<long>(output_buf_.size());
  src_data.src_ratio     = ratio_;
  src_data.end_of_input  = 1;

  const int error = src_process(static_cast<SRC_STATE*>(state_), &src_data);
  if (error != 0) {
    throw AudioError(std::string("Resampler flush failed: ") + src_strerror(error));
  }

  // Reset internal state so the resampler can be reused
  src_reset(static_cast<SRC_STATE*>(state_));

  return {output_buf_.data(), static_cast<size_t>(src_data.output_frames_gen)};
}

float compute_rms(span<const float> samples) {
  if (samples.empty()) {
    return 0.0f;
  }

  double sum = 0.0;
  for (const float s : samples) {
    // cppcheck-suppress useStlAlgorithm
    sum += static_cast<double>(s) * static_cast<double>(s);
  }
  return static_cast<float>(std::sqrt(sum / static_cast<double>(samples.size())));
}

}  // namespace asr
