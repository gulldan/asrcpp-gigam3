#define DR_WAV_IMPLEMENTATION
#include "asr/audio.h"

#include <samplerate.h>
#include <spdlog/spdlog.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string_view>
#include <utility>

#include "asr/span.h"
#include "dr_wav.h"

namespace asr {
namespace {

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
  return "flac, mp3, mp4, mpeg, mpga, m4a, ogg, wav, webm";
}

int decode_base64_char(unsigned char c) {
  if (c >= 'A' && c <= 'Z') {
    return c - 'A';
  }
  if (c >= 'a' && c <= 'z') {
    return c - 'a' + 26;
  }
  if (c >= '0' && c <= '9') {
    return c - '0' + 52;
  }
  if (c == '+' || c == '-') {
    return 62;
  }
  if (c == '/' || c == '_') {
    return 63;
  }
  return -1;
}

class TempFile {
 public:
  TempFile() = default;
  TempFile(std::string path, int fd) : path_(std::move(path)), fd_(fd) {}

  TempFile(const TempFile&)            = delete;
  TempFile& operator=(const TempFile&) = delete;

  TempFile(TempFile&& other) noexcept : path_(std::move(other.path_)), fd_(other.fd_) {
    other.fd_ = -1;
  }

  TempFile& operator=(TempFile&& other) noexcept {
    if (this != &other) {
      cleanup();
      path_     = std::move(other.path_);
      fd_       = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  ~TempFile() {
    cleanup();
  }

  int fd() const {
    return fd_;
  }

  const std::string& path() const {
    return path_;
  }

  void close_fd() {
    if (fd_ >= 0) {
      (void)::close(fd_);
      fd_ = -1;
    }
  }

 private:
  void cleanup() {
    close_fd();
    if (!path_.empty()) {
      (void)std::remove(path_.c_str());
    }
  }

  std::string path_;
  int         fd_ = -1;
};

TempFile make_temp_file_with_suffix(std::string_view suffix) {
  std::string pattern = "/tmp/asr-audio-XXXXXX";
  pattern += std::string(suffix);

  std::vector<char> path_buf(pattern.begin(), pattern.end());
  path_buf.push_back('\0');

  const int suffix_len = static_cast<int>(suffix.size());
  const int fd         = mkstemps(path_buf.data(), suffix_len);
  if (fd < 0) {
    throw AudioError("Failed to create temporary file: " + std::string(std::strerror(errno)));
  }
  return TempFile(std::string(path_buf.data()), fd);
}

void write_all_bytes(int fd, span<const uint8_t> data) {
  size_t written = 0;
  while (written < data.size()) {
    const auto* ptr = reinterpret_cast<const char*>(data.data() + written);
    const auto  n   = ::write(fd, ptr, data.size() - written);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw AudioError("Failed to write temporary audio file: " + std::string(std::strerror(errno)));
    }
    written += static_cast<size_t>(n);
  }
}

std::vector<uint8_t> read_file_bytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    throw AudioError("Failed to read temporary decoded WAV file");
  }
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void run_ffmpeg_decode_to_wav(const std::string& input_path, const std::string& output_path,
                              int target_rate) {
  const std::string sample_rate = std::to_string(target_rate);

  const auto pid = fork();
  if (pid < 0) {
    throw AudioError("Failed to start ffmpeg: fork() failed");
  }

  if (pid == 0) {
    execlp("ffmpeg", "ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-i",
           input_path.c_str(), "-ac", "1", "-ar", sample_rate.c_str(), "-f", "wav", output_path.c_str(),
           static_cast<char*>(nullptr));
    _exit(errno == ENOENT ? 127 : 126);
  }

  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno == EINTR) {
      continue;
    }
    throw AudioError("Failed waiting for ffmpeg process");
  }

  if (WIFEXITED(status)) {
    const int exit_code = WEXITSTATUS(status);
    if (exit_code == 0) {
      return;
    }
    if (exit_code == 127) {
      throw AudioError("ffmpeg is not installed; required for non-WAV audio formats");
    }
    throw AudioError("ffmpeg failed to decode audio (exit code " + std::to_string(exit_code) + ")");
  }

  throw AudioError("ffmpeg terminated unexpectedly");
}

AudioData decode_with_ffmpeg(span<const uint8_t> data, std::string_view file_name, int target_rate) {
  auto extension = extension_from_filename(file_name);
  if (extension.empty()) {
    extension = "bin";
  }

  auto input_file  = make_temp_file_with_suffix("." + extension);
  auto output_file = make_temp_file_with_suffix(".wav");

  write_all_bytes(input_file.fd(), data);
  input_file.close_fd();
  output_file.close_fd();

  run_ffmpeg_decode_to_wav(input_file.path(), output_file.path(), target_rate);

  auto wav_bytes = read_file_bytes(output_file.path());
  if (wav_bytes.empty()) {
    throw AudioError("ffmpeg produced empty output");
  }

  return decode_wav(wav_bytes, target_rate);
}

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

bool is_supported_whisper_audio_extension(std::string_view extension) {
  auto normalized = to_lower_ascii(extension);
  if (!normalized.empty() && normalized.front() == '.') {
    normalized.erase(normalized.begin());
  }

  return normalized == "flac" || normalized == "mp3" || normalized == "mp4" || normalized == "mpeg" ||
         normalized == "mpga" || normalized == "m4a" || normalized == "ogg" || normalized == "wav" ||
         normalized == "webm";
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

  if (extension == "wav") {
    try {
      return decode_wav(data, target_rate);
    } catch (const AudioError&) {
      // Some WAV variants can still be decoded by ffmpeg.
      return decode_with_ffmpeg(data, file_name, target_rate);
    }
  }

  if (extension.empty()) {
    try {
      return decode_wav(data, target_rate);
    } catch (const AudioError&) {
      return decode_with_ffmpeg(data, file_name, target_rate);
    }
  }

  return decode_with_ffmpeg(data, file_name, target_rate);
}

std::vector<uint8_t> base64_decode(std::string_view input) {
  std::vector<uint8_t> out;
  out.reserve((input.size() * 3ULL) / 4ULL + 3ULL);

  uint32_t accum      = 0;
  int      accum_bits = 0;
  bool     seen_pad   = false;

  for (const unsigned char c : input) {
    if (std::isspace(c) != 0) {
      continue;
    }
    if (c == '=') {
      seen_pad = true;
      continue;
    }
    if (seen_pad) {
      throw AudioError("Invalid base64 payload: non-padding character after '='");
    }

    const int v = decode_base64_char(c);
    if (v < 0) {
      throw AudioError("Invalid base64 payload");
    }

    accum      = (accum << 6U) | static_cast<uint32_t>(v);
    accum_bits += 6;
    if (accum_bits >= 8) {
      accum_bits -= 8;
      out.push_back(static_cast<uint8_t>((accum >> static_cast<unsigned>(accum_bits)) & 0xFFU));
    }
  }

  if (accum_bits >= 6) {
    throw AudioError("Invalid base64 payload: dangling bits");
  }

  return out;
}

std::vector<float> pcm16_to_float32(span<const uint8_t> pcm16_data) {
  if (pcm16_data.empty()) {
    return {};
  }
  if (pcm16_data.size() % 2U != 0U) {
    throw AudioError("Invalid PCM16 payload: byte count must be even");
  }

  std::vector<float> out(pcm16_data.size() / 2U);
  for (size_t i = 0; i < out.size(); ++i) {
    const uint8_t lo = pcm16_data[i * 2U];
    const uint8_t hi = pcm16_data[i * 2U + 1U];
    const int16_t sample = static_cast<int16_t>(static_cast<uint16_t>(lo) |
                                                (static_cast<uint16_t>(hi) << 8U));
    out[i] = static_cast<float>(sample) / 32768.0F;
  }
  return out;
}

std::vector<float> decode_realtime_audio(std::string_view base64_audio, std::string_view format) {
  auto normalized = to_lower_ascii(format);
  auto bytes      = base64_decode(base64_audio);

  if (normalized.empty() || normalized == "pcm16") {
    return pcm16_to_float32(bytes);
  }

  if (normalized == "g711_ulaw" || normalized == "g711_alaw") {
    throw AudioError("Realtime format '" + normalized + "' is not implemented");
  }

  throw AudioError("Unsupported realtime audio format '" + normalized + "'");
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

  static constexpr float kDummyInput = 0.0f;

  SRC_DATA src_data{};
  // input_frames=0: libsamplerate must not read data_in, but it still checks pointer overlap
  // against data_out, so use a distinct dummy address.
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

  // Reset internal state so the resampler can be reused
  src_reset(static_cast<SRC_STATE*>(state_));

  return {output_buf_.data(), static_cast<size_t>(src_data.output_frames_gen)};
}

void StreamResampler::reset() {
  src_reset(static_cast<SRC_STATE*>(state_));
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
