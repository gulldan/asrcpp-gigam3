#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "asr/audio.h"
#include "asr/span.h"

// Include dr_wav for creating test WAV data
#include "dr_wav.h"

namespace asr {
namespace {

// Helper: create a valid mono WAV file in memory
std::vector<uint8_t> make_wav(const std::vector<float>& samples, int sample_rate, int channels = 1) {
  drwav_data_format format{};
  format.container     = drwav_container_riff;
  format.format        = DR_WAVE_FORMAT_IEEE_FLOAT;
  format.channels      = static_cast<drwav_uint32>(channels);
  format.sampleRate    = static_cast<drwav_uint32>(sample_rate);
  format.bitsPerSample = 32;

  void*  file_data = nullptr;
  size_t file_size = 0;
  drwav  wav;
  if (drwav_init_memory_write(&wav, &file_data, &file_size, &format, nullptr) == 0u) {
    return {};
  }
  drwav_write_pcm_frames(&wav, samples.size() / channels, samples.data());
  drwav_uninit(&wav);

  auto*                bytes = static_cast<uint8_t*>(file_data);
  std::vector<uint8_t> result(bytes, bytes + file_size);
  drwav_free(file_data, nullptr);
  return result;
}

// Helper: create a sine wave
std::vector<float> make_sine(float freq, float duration_sec, int sample_rate) {
  auto               num_samples = static_cast<size_t>(duration_sec * static_cast<float>(sample_rate));
  std::vector<float> samples(num_samples);
  for (size_t i = 0; i < num_samples; ++i) {
    samples[i] = std::sin(2.0f * static_cast<float>(M_PI) * freq * static_cast<float>(i) /
                          static_cast<float>(sample_rate));
  }
  return samples;
}

TEST(Audio, DecodeMono16kHz) {
  auto sine     = make_sine(440.0f, 1.0f, 16000);
  auto wav_data = make_wav(sine, 16000);
  ASSERT_FALSE(wav_data.empty());

  auto audio = decode_wav(wav_data, 16000);
  EXPECT_EQ(audio.samples.size(), sine.size());
  EXPECT_NEAR(audio.duration_sec, 1.0f, 0.01f);

  // Samples should be close to original
  for (size_t i = 0; i < sine.size(); ++i) {
    EXPECT_NEAR(audio.samples[i], sine[i], 1e-5f);
  }
}

TEST(Audio, RejectStereo) {
  const std::vector<float> stereo_samples(32000);  // 1 sec stereo at 16kHz
  auto                     wav_data = make_wav(stereo_samples, 16000, 2);
  ASSERT_FALSE(wav_data.empty());

  EXPECT_THROW(decode_wav(wav_data, 16000), AudioError);
}

TEST(Audio, RejectInvalid) {
  std::vector<uint8_t> garbage = {0x00, 0x01, 0x02, 0x03, 0xFF, 0xFE};
  EXPECT_THROW(decode_wav(garbage, 16000), AudioError);
}

TEST(Audio, RejectEmpty) {
  const span<const uint8_t> empty_span;
  EXPECT_THROW(decode_wav(empty_span, 16000), AudioError);
}

TEST(Audio, ResampleFrom44100) {
  auto sine     = make_sine(440.0f, 1.0f, 44100);
  auto wav_data = make_wav(sine, 44100);
  ASSERT_FALSE(wav_data.empty());

  auto audio = decode_wav(wav_data, 16000);
  // Output should be ~16000 samples for 1 second
  EXPECT_NEAR(static_cast<float>(audio.samples.size()), 16000.0f, 100.0f);
  EXPECT_NEAR(audio.duration_sec, 1.0f, 0.02f);
}

TEST(Audio, ResampleFrom48000) {
  auto sine     = make_sine(440.0f, 1.0f, 48000);
  auto wav_data = make_wav(sine, 48000);
  ASSERT_FALSE(wav_data.empty());

  auto audio = decode_wav(wav_data, 16000);
  EXPECT_NEAR(static_cast<float>(audio.samples.size()), 16000.0f, 100.0f);
  EXPECT_NEAR(audio.duration_sec, 1.0f, 0.02f);
}

TEST(Audio, NoResampleAt16000) {
  auto sine     = make_sine(440.0f, 0.5f, 16000);
  auto wav_data = make_wav(sine, 16000);
  ASSERT_FALSE(wav_data.empty());

  auto audio = decode_wav(wav_data, 16000);
  EXPECT_EQ(audio.samples.size(), sine.size());
}

TEST(Audio, DurationCalculation) {
  auto sine     = make_sine(440.0f, 2.5f, 16000);
  auto wav_data = make_wav(sine, 16000);

  auto audio = decode_wav(wav_data, 16000);
  EXPECT_NEAR(audio.duration_sec, 2.5f, 0.01f);
}

TEST(Audio, ComputeRms) {
  // RMS of a sine wave with amplitude A is A/sqrt(2)
  const float        amplitude   = 0.5f;
  auto               num_samples = 16000;
  std::vector<float> sine(num_samples);
  for (int i = 0; i < num_samples; ++i) {
    sine[i] =
        amplitude * std::sin(2.0f * static_cast<float>(M_PI) * 440.0f * static_cast<float>(i) / 16000.0f);
  }

  const float rms          = compute_rms(sine);
  const float expected_rms = amplitude / std::sqrt(2.0f);
  EXPECT_NEAR(rms, expected_rms, 0.01f);
}

TEST(Audio, ComputeRmsSilence) {
  std::vector<float> silence(1000, 0.0f);
  EXPECT_FLOAT_EQ(compute_rms(silence), 0.0f);
}

TEST(Audio, ComputeRmsEmpty) {
  const span<const float> empty;
  EXPECT_FLOAT_EQ(compute_rms(empty), 0.0f);
}

TEST(Audio, NormalizationRange) {
  // Create a WAV with extreme int16 values simulated through float
  const std::vector<float> samples  = {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f};
  auto                     wav_data = make_wav(samples, 16000);

  auto audio = decode_wav(wav_data, 16000);
  for (const float s : audio.samples) {
    EXPECT_GE(s, -1.0f);
    EXPECT_LE(s, 1.0f);
  }
}

TEST(Audio, DurationGuardAllowsNormal) {
  // 10 seconds at 16kHz should be fine
  auto sine     = make_sine(440.0f, 10.0f, 16000);
  auto wav_data = make_wav(sine, 16000);
  ASSERT_FALSE(wav_data.empty());
  EXPECT_NO_THROW(decode_wav(wav_data, 16000));
}

}  // namespace
}  // namespace asr
