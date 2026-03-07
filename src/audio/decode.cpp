#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "asr/audio.h"
#include "asr/span.h"

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#ifdef ASR_HAS_OPUSFILE
#include <opus/opusfile.h>
#endif

namespace asr {
namespace {

constexpr uint64_t kMaxAudioFrames = 48000ULL * 3600ULL;  // 1 hour at 48kHz
constexpr uint64_t kWavReadFrames  = 8192ULL;

std::string to_lower_ascii(std::string_view value) {
  std::string out(value.begin(), value.end());
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return out;
}

std::string extension_from_filename(std::string_view file_name) {
  const auto pos = file_name.find_last_of('.');
  if (pos == std::string_view::npos || pos + 1 >= file_name.size()) {
    return {};
  }
  return to_lower_ascii(file_name.substr(pos + 1));
}

std::string supported_audio_extensions_list() {
#ifdef ASR_HAS_OPUSFILE
  return "opus, wav";
#else
  return "wav";
#endif
}

class ChunkEmitter {
 public:
  ChunkEmitter(size_t chunk_samples, const AudioChunkCallback& on_chunk)
      : chunk_samples_(std::max<size_t>(1, chunk_samples)), on_chunk_(on_chunk) {
    if (!on_chunk_) {
      throw AudioError("Streaming decode callback is empty");
    }
  }

  void push(span<const float> samples) {
    if (samples.empty()) {
      return;
    }

    buffer_.insert(buffer_.end(), samples.begin(), samples.end());
    while (buffer_.size() - offset_ >= chunk_samples_) {
      on_chunk_({buffer_.data() + static_cast<ptrdiff_t>(offset_), chunk_samples_});
      offset_ += chunk_samples_;
    }

    if (offset_ > 0 && offset_ >= buffer_.size() / 2U) {
      buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<ptrdiff_t>(offset_));
      offset_ = 0;
    }
  }

  void flush() {
    const size_t tail = buffer_.size() - offset_;
    if (tail > 0) {
      on_chunk_({buffer_.data() + static_cast<ptrdiff_t>(offset_), tail});
    }
    buffer_.clear();
    offset_ = 0;
  }

 private:
  size_t             chunk_samples_;
  AudioChunkCallback on_chunk_;
  std::vector<float> buffer_;
  size_t             offset_ = 0;
};

void validate_wav_header(const drwav& wav) {
  if (wav.channels != 1) {
    throw AudioError("Only mono audio is supported, got " + std::to_string(wav.channels) + " channels");
  }

  if (wav.totalPCMFrameCount == 0ULL) {
    throw AudioError("WAV file contains no audio frames");
  }

  if (wav.totalPCMFrameCount > kMaxAudioFrames) {
    throw AudioError("WAV file too long: " + std::to_string(wav.totalPCMFrameCount) +
                     " frames exceeds 1-hour limit");
  }
}

AudioStreamStats decode_wav_stream_impl(drwav& wav, int target_rate, size_t chunk_samples,
                                        const AudioChunkCallback& on_chunk) {
  validate_wav_header(wav);

  ChunkEmitter chunker(chunk_samples, on_chunk);

  std::vector<float> read_buf(static_cast<size_t>(kWavReadFrames));
  const int          input_rate = static_cast<int>(wav.sampleRate);

  uint64_t total_output_samples = 0;
  if (input_rate == target_rate) {
    for (;;) {
      const auto frames_read = drwav_read_pcm_frames_f32(&wav, kWavReadFrames, read_buf.data());
      if (frames_read == 0ULL) {
        break;
      }
      const auto n = static_cast<size_t>(frames_read);
      chunker.push({read_buf.data(), n});
      total_output_samples += frames_read;
    }
  } else {
    StreamResampler resampler(input_rate, target_rate);
    for (;;) {
      const auto frames_read = drwav_read_pcm_frames_f32(&wav, kWavReadFrames, read_buf.data());
      if (frames_read == 0ULL) {
        break;
      }
      const auto out = resampler.process({read_buf.data(), static_cast<size_t>(frames_read)});
      chunker.push(out);
      total_output_samples += static_cast<uint64_t>(out.size());
    }

    const auto tail = resampler.flush();
    if (!tail.empty()) {
      chunker.push(tail);
      total_output_samples += static_cast<uint64_t>(tail.size());
    }
  }

  chunker.flush();

  AudioStreamStats stats;
  stats.samples = static_cast<size_t>(total_output_samples);
  stats.duration_sec =
      static_cast<float>(static_cast<double>(total_output_samples) / static_cast<double>(target_rate));
  return stats;
}

AudioStreamStats decode_wav_stream_memory(span<const uint8_t> data, int target_rate, size_t chunk_samples,
                                          const AudioChunkCallback& on_chunk) {
  if (data.empty()) {
    throw AudioError("Empty audio data");
  }

  drwav wav;
  if (drwav_init_memory(&wav, data.data(), data.size(), nullptr) == 0u) {
    throw AudioError("Failed to decode WAV file: invalid format");
  }

  try {
    auto stats = decode_wav_stream_impl(wav, target_rate, chunk_samples, on_chunk);
    drwav_uninit(&wav);
    return stats;
  } catch (...) {
    drwav_uninit(&wav);
    throw;
  }
}

#ifdef ASR_HAS_OPUSFILE

std::string opusfile_error_message(int code) {
  switch (code) {
    case OP_FALSE:
      return "A request did not succeed";
    case OP_EOF:
      return "Unexpected end of file";
    case OP_HOLE:
      return "There was a hole in the data";
    case OP_EREAD:
      return "Read or seek operation failed";
    case OP_EFAULT:
      return "Memory allocation failure";
    case OP_EIMPL:
      return "Unimplemented feature in Opus stream";
    case OP_EINVAL:
      return "Invalid Opus stream";
    case OP_ENOTFORMAT:
      return "Data is not recognized as an Opus stream";
    case OP_EBADHEADER:
      return "Malformed or missing Opus headers";
    case OP_EVERSION:
      return "Unsupported Opus stream version";
    case OP_ENOTAUDIO:
      return "Not an audio Opus stream";
    case OP_EBADPACKET:
      return "Malformed Opus packet";
    case OP_EBADLINK:
      return "Invalid stream link";
    case OP_ENOSEEK:
      return "Stream is not seekable";
    case OP_EBADTIMESTAMP:
      return "Invalid timestamp in stream";
    default:
      return "Unknown Opus file error";
  }
}

AudioData decode_opus_file(span<const uint8_t> data, int target_rate) {
  int          err = 0;
  OggOpusFile* of  = op_open_memory(data.data(), static_cast<int>(data.size()), &err);
  if (of == nullptr) {
    throw AudioError("Failed to decode Opus file: " + opusfile_error_message(err));
  }

  auto cleanup = [&of]() {
    if (of != nullptr) {
      op_free(of);
      of = nullptr;
    }
  };

  const int channels = op_channel_count(of, -1);
  if (channels != 1) {
    cleanup();
    throw AudioError("Only mono audio is supported, got " + std::to_string(channels) + " channels");
  }

  const opus_int64 total_frames = op_pcm_total(of, -1);
  if (total_frames > static_cast<opus_int64>(kMaxAudioFrames)) {
    cleanup();
    throw AudioError("Opus file too long: " + std::to_string(total_frames) + " frames exceeds 1-hour limit");
  }

  std::vector<float> samples;
  if (target_rate == 48000) {
    if (total_frames > 0) {
      samples.reserve(static_cast<size_t>(total_frames));
    }
  } else if (total_frames > 0) {
    const double estimate = static_cast<double>(total_frames) * static_cast<double>(target_rate) / 48000.0;
    samples.reserve(static_cast<size_t>(std::max(estimate, 0.0)) + 64U);
  }

  std::vector<float> read_buf(4096U);
  if (target_rate == 48000) {
    for (;;) {
      int section = 0;
      int n       = op_read_float(of, read_buf.data(), static_cast<int>(read_buf.size()), &section);
      if (n == 0) {
        break;
      }
      if (n < 0) {
        cleanup();
        throw AudioError("Failed to decode Opus packet: " + opusfile_error_message(n));
      }
      samples.insert(samples.end(), read_buf.begin(), read_buf.begin() + n);
    }
  } else {
    StreamResampler resampler(48000, target_rate);
    for (;;) {
      int section = 0;
      int n       = op_read_float(of, read_buf.data(), static_cast<int>(read_buf.size()), &section);
      if (n == 0) {
        break;
      }
      if (n < 0) {
        cleanup();
        throw AudioError("Failed to decode Opus packet: " + opusfile_error_message(n));
      }
      auto out = resampler.process({read_buf.data(), static_cast<size_t>(n)});
      samples.insert(samples.end(), out.begin(), out.end());
    }
    auto tail = resampler.flush();
    samples.insert(samples.end(), tail.begin(), tail.end());
  }

  cleanup();

  if (samples.empty()) {
    throw AudioError("Opus file contains no audio frames");
  }

  AudioData audio;
  audio.samples = std::move(samples);
  audio.duration_sec =
      static_cast<float>(static_cast<double>(audio.samples.size()) / static_cast<double>(target_rate));
  return audio;
}

AudioStreamStats decode_opus_file_streamed(span<const uint8_t> data, int target_rate, size_t chunk_samples,
                                           const AudioChunkCallback& on_chunk) {
  int          err = 0;
  OggOpusFile* of  = op_open_memory(data.data(), static_cast<int>(data.size()), &err);
  if (of == nullptr) {
    throw AudioError("Failed to decode Opus file: " + opusfile_error_message(err));
  }

  auto cleanup = [&of]() {
    if (of != nullptr) {
      op_free(of);
      of = nullptr;
    }
  };

  const int channels = op_channel_count(of, -1);
  if (channels != 1) {
    cleanup();
    throw AudioError("Only mono audio is supported, got " + std::to_string(channels) + " channels");
  }

  const opus_int64 total_frames = op_pcm_total(of, -1);
  if (total_frames > static_cast<opus_int64>(kMaxAudioFrames)) {
    cleanup();
    throw AudioError("Opus file too long: " + std::to_string(total_frames) + " frames exceeds 1-hour limit");
  }

  ChunkEmitter       chunker(chunk_samples, on_chunk);
  std::vector<float> read_buf(4096U);
  uint64_t           total_output_samples = 0;

  if (target_rate == 48000) {
    for (;;) {
      int section = 0;
      int n       = op_read_float(of, read_buf.data(), static_cast<int>(read_buf.size()), &section);
      if (n == 0) {
        break;
      }
      if (n < 0) {
        cleanup();
        throw AudioError("Failed to decode Opus packet: " + opusfile_error_message(n));
      }
      chunker.push({read_buf.data(), static_cast<size_t>(n)});
      total_output_samples += static_cast<uint64_t>(n);
    }
  } else {
    StreamResampler resampler(48000, target_rate);
    for (;;) {
      int section = 0;
      int n       = op_read_float(of, read_buf.data(), static_cast<int>(read_buf.size()), &section);
      if (n == 0) {
        break;
      }
      if (n < 0) {
        cleanup();
        throw AudioError("Failed to decode Opus packet: " + opusfile_error_message(n));
      }

      auto out = resampler.process({read_buf.data(), static_cast<size_t>(n)});
      chunker.push(out);
      total_output_samples += static_cast<uint64_t>(out.size());
    }

    auto tail = resampler.flush();
    if (!tail.empty()) {
      chunker.push(tail);
      total_output_samples += static_cast<uint64_t>(tail.size());
    }
  }

  cleanup();
  chunker.flush();

  if (total_output_samples == 0ULL) {
    throw AudioError("Opus file contains no audio frames");
  }

  AudioStreamStats stats;
  stats.samples = static_cast<size_t>(total_output_samples);
  stats.duration_sec =
      static_cast<float>(static_cast<double>(total_output_samples) / static_cast<double>(target_rate));
  return stats;
}

#endif

}  // namespace

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

  if (total_frames > kMaxAudioFrames) {
    drwav_uninit(&wav);
    throw AudioError("WAV file too long: " + std::to_string(total_frames) + " frames exceeds 1-hour limit");
  }

  std::vector<float> samples(total_frames);
  auto               frames_read = drwav_read_pcm_frames_f32(&wav, total_frames, samples.data());
  auto               input_rate  = static_cast<int>(wav.sampleRate);
  drwav_uninit(&wav);

  if (frames_read == 0) {
    throw AudioError("Failed to read PCM frames from WAV");
  }
  samples.resize(static_cast<size_t>(frames_read));

  if (input_rate == target_rate) {
    return {std::move(samples), static_cast<float>(static_cast<double>(frames_read) / target_rate)};
  }

  StreamResampler    resampler(input_rate, target_rate);
  std::vector<float> resampled;
  resampled.reserve(
      static_cast<size_t>(std::lround(static_cast<double>(samples.size()) * static_cast<double>(target_rate) /
                                      static_cast<double>(input_rate))));

  auto out = resampler.process(samples);
  resampled.insert(resampled.end(), out.begin(), out.end());

  auto tail = resampler.flush();
  resampled.insert(resampled.end(), tail.begin(), tail.end());

  if (resampled.empty()) {
    throw AudioError("Resampling produced empty audio");
  }

  const auto duration_sec =
      static_cast<float>(static_cast<double>(resampled.size()) / static_cast<double>(target_rate));
  return {std::move(resampled), duration_sec};
}

bool is_supported_whisper_audio_extension(std::string_view extension) {
  auto normalized = to_lower_ascii(extension);
  if (!normalized.empty() && normalized.front() == '.') {
    normalized.erase(normalized.begin());
  }

#ifdef ASR_HAS_OPUSFILE
  return normalized == "wav" || normalized == "opus";
#else
  return normalized == "wav";
#endif
}

AudioData decode_audio(span<const uint8_t> data, std::string_view file_name, int target_rate) {
  if (data.empty()) {
    throw AudioError("Empty audio data");
  }

  const auto extension = extension_from_filename(file_name);
  if (!extension.empty() && !is_supported_whisper_audio_extension(extension)) {
    throw AudioError("Unsupported audio format '." + extension +
                     "' — accepted: " + supported_audio_extensions_list());
  }

  if (extension.empty() || extension == "wav") {
    return decode_wav(data, target_rate);
  }

#ifdef ASR_HAS_OPUSFILE
  if (extension == "opus") {
    return decode_opus_file(data, target_rate);
  }
#endif

  throw AudioError("Unsupported audio format '." + extension +
                   "' — accepted: " + supported_audio_extensions_list());
}

AudioStreamStats decode_audio_streamed(span<const uint8_t> data, std::string_view file_name, int target_rate,
                                       size_t chunk_samples, const AudioChunkCallback& on_chunk) {
  if (data.empty()) {
    throw AudioError("Empty audio data");
  }

  const auto extension = extension_from_filename(file_name);
  if (!extension.empty() && !is_supported_whisper_audio_extension(extension)) {
    throw AudioError("Unsupported audio format '." + extension +
                     "' — accepted: " + supported_audio_extensions_list());
  }

  if (extension.empty() || extension == "wav") {
    return decode_wav_stream_memory(data, target_rate, chunk_samples, on_chunk);
  }

#ifdef ASR_HAS_OPUSFILE
  if (extension == "opus") {
    return decode_opus_file_streamed(data, target_rate, chunk_samples, on_chunk);
  }
#endif

  throw AudioError("Unsupported audio format '." + extension +
                   "' — accepted: " + supported_audio_extensions_list());
}

std::vector<float> decode_realtime_audio_bytes(span<const uint8_t> audio_bytes, std::string_view format,
                                               int target_rate) {
  auto normalized = to_lower_ascii(format);
  if (normalized.empty() || normalized == "pcm16") {
    return pcm16_to_float32(audio_bytes);
  }

  if (normalized == "opus") {
    RealtimeOpusDecoder decoder(target_rate);
    auto                decoded = decoder.decode_packet(audio_bytes);
    return std::vector<float>(decoded.begin(), decoded.end());
  }

  if (normalized == "g711_ulaw" || normalized == "g711_alaw") {
    throw AudioError("Realtime format '" + normalized + "' is not implemented");
  }

  throw AudioError("Unsupported realtime audio format '" + normalized + "'");
}

std::vector<float> decode_realtime_audio(std::string_view base64_audio, std::string_view format,
                                         int target_rate) {
  std::vector<uint8_t> bytes;
  base64_decode_into(base64_audio, bytes);
  return decode_realtime_audio_bytes(bytes, format, target_rate);
}

}  // namespace asr
