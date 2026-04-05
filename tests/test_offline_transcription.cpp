#include <gtest/gtest.h>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

#include "asr/offline_transcription.h"
#include "asr/span.h"

namespace asr {

TEST(OfflineTranscription, EmptyAudioReturnsEmptyText) {
  std::vector<float> audio;
  int                calls = 0;

  auto text = recognize_audio_chunked(audio, 16000, 20.0f, [&calls](span<const float>, int) {
    ++calls;
    return std::string("unused");
  });

  EXPECT_TRUE(text.empty());
  EXPECT_EQ(calls, 0);
}

TEST(OfflineTranscription, InvalidArgsAreRejected) {
  std::vector<float> audio(16, 0.1f);

  EXPECT_THROW(
      (void)recognize_audio_chunked(audio, 0, 20.0f, [](span<const float>, int) { return std::string(); }),
      std::invalid_argument);
  EXPECT_THROW((void)recognize_audio_chunked(audio, 16000, 20.0f, RecognizeChunkFn{}), std::invalid_argument);
}

TEST(OfflineTranscription, UsesSingleChunkWhenInputFits) {
  std::vector<float> audio(15, 0.1f);
  int                calls = 0;
  size_t             seen  = 0;

  auto text = recognize_audio_chunked(audio, 10, 2.0f, [&calls, &seen](span<const float> chunk, int) {
    ++calls;
    seen = chunk.size();
    return std::string(" hello ");
  });

  EXPECT_EQ(calls, 1);
  EXPECT_EQ(seen, audio.size());
  EXPECT_EQ(text, "hello");
}

TEST(OfflineTranscription, SplitsIntoChunksAndJoinsNonEmptyResults) {
  std::vector<float>  audio(25, 0.2f);
  std::vector<size_t> chunks;
  int                 idx = 0;

  auto text =
      recognize_audio_chunked(audio, 10, 1.0f, [&chunks, &idx](span<const float> chunk, int) -> std::string {
        chunks.push_back(chunk.size());
        ++idx;
        if (idx == 1) {
          return " first ";
        }
        if (idx == 2) {
          return "";
        }
        return "third";
      });

  ASSERT_EQ(chunks.size(), 3);
  EXPECT_EQ(chunks[0], 10);
  EXPECT_EQ(chunks[1], 10);
  EXPECT_EQ(chunks[2], 5);
  EXPECT_EQ(text, "first third");
}

TEST(OfflineTranscription, NonPositiveChunkDurationFallsBackToSinglePass) {
  std::vector<float> audio(21, 0.3f);
  int                calls = 0;

  auto text = recognize_audio_chunked(audio, 10, 0.0f, [&calls](span<const float> chunk, int) {
    ++calls;
    EXPECT_EQ(chunk.size(), 21);
    return std::string("  full  ");
  });

  EXPECT_EQ(calls, 1);
  EXPECT_EQ(text, "full");
}

}  // namespace asr
