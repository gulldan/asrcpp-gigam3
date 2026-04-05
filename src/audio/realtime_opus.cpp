#include <opus/opus.h>
#include <opus/opus_defines.h>
#include <opus/opus_types.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "asr/audio.h"
#include "asr/span.h"

namespace asr {
namespace {

constexpr uint16_t kRtpVersionMask  = 0xC000U;
constexpr uint16_t kRtpVersion2Bits = 0x8000U;

struct ParsedOpusPacket {
  span<const uint8_t> payload;
  bool                has_sequence = false;
  uint16_t            sequence     = 0;
  bool                is_rtp       = false;
};

bool try_parse_rtp_packet(span<const uint8_t> packet, ParsedOpusPacket& out) {
  out = ParsedOpusPacket{};
  if (packet.size() < 12U) {
    return false;
  }

  const uint16_t first_two_bytes = (static_cast<uint16_t>(packet[0]) << 8U) | packet[1];
  if ((first_two_bytes & kRtpVersionMask) != kRtpVersion2Bits) {
    return false;
  }

  const uint8_t csrc_count = packet[0] & 0x0FU;
  const bool    has_ext    = (packet[0] & 0x10U) != 0U;
  const bool    has_pad    = (packet[0] & 0x20U) != 0U;

  size_t header_size = 12U + static_cast<size_t>(csrc_count) * 4U;
  if (packet.size() < header_size) {
    return false;
  }

  if (has_ext) {
    if (packet.size() < header_size + 4U) {
      return false;
    }
    const size_t ext_words = (static_cast<size_t>(packet[header_size + 2U]) << 8U) | packet[header_size + 3U];
    header_size += 4U + ext_words * 4U;
    if (packet.size() < header_size) {
      return false;
    }
  }

  size_t payload_end = packet.size();
  if (has_pad) {
    const uint8_t pad = packet.back();
    if (pad == 0U || static_cast<size_t>(pad) > payload_end - header_size) {
      return false;
    }
    payload_end -= static_cast<size_t>(pad);
  }

  if (payload_end <= header_size) {
    return false;
  }

  out.payload      = packet.subspan(header_size, payload_end - header_size);
  out.has_sequence = true;
  out.sequence     = (static_cast<uint16_t>(packet[2]) << 8U) | packet[3];
  out.is_rtp       = true;
  return true;
}

bool is_supported_opus_sample_rate(int sample_rate) {
  return sample_rate == 8000 || sample_rate == 12000 || sample_rate == 16000 || sample_rate == 24000 ||
         sample_rate == 48000;
}

std::string opus_error_to_string(int code) {
  const char* text = opus_strerror(code);
  if (text == nullptr) {
    return "unknown Opus error";
  }
  return text;
}

uint16_t forward_sequence_delta(uint16_t prev, uint16_t current) {
  return static_cast<uint16_t>(current - prev);
}

}  // namespace

RealtimeOpusDecoder::RealtimeOpusDecoder(int sample_rate, size_t max_plc_packets, bool enable_fec,
                                         OpusPacketMode packet_mode)
    : sample_rate_(sample_rate),
      max_frame_samples_(sample_rate_ * 120 / 1000),
      last_frame_samples_(sample_rate_ / 50),
      max_plc_packets_(std::max<size_t>(1, max_plc_packets)),
      enable_fec_(enable_fec),
      packet_mode_(packet_mode) {
  if (!is_supported_opus_sample_rate(sample_rate_)) {
    throw AudioError("Unsupported Opus sample rate " + std::to_string(sample_rate_) +
                     "; supported: 8000, 12000, 16000, 24000, 48000");
  }
  if (max_frame_samples_ <= 0) {
    throw AudioError("Invalid Opus max frame size for sample rate " + std::to_string(sample_rate_));
  }
  if (last_frame_samples_ <= 0) {
    last_frame_samples_ = 1;
  }

  int error = OPUS_OK;
  decoder_  = opus_decoder_create(sample_rate_, 1, &error);
  if (decoder_ == nullptr || error != OPUS_OK) {
    throw AudioError("Failed to create Opus decoder: " + opus_error_to_string(error));
  }

  const size_t reserve_samples = static_cast<size_t>(max_frame_samples_) * (max_plc_packets_ + 1U);
  output_.reserve(reserve_samples);
}

RealtimeOpusDecoder::~RealtimeOpusDecoder() {
  if (decoder_ != nullptr) {
    opus_decoder_destroy(static_cast<OpusDecoder*>(decoder_));
    decoder_ = nullptr;
  }
}

span<const float> RealtimeOpusDecoder::decode_packet(span<const uint8_t> packet) {
  if (packet.empty()) {
    throw AudioError("Empty Opus packet");
  }

  ParsedOpusPacket parsed;
  if (packet_mode_ == OpusPacketMode::Rtp) {
    if (!try_parse_rtp_packet(packet, parsed)) {
      throw AudioError("Expected RTP packet with Opus payload");
    }
    ++stats_.rtp_packets;
  } else if (packet_mode_ == OpusPacketMode::Auto && try_parse_rtp_packet(packet, parsed)) {
    ++stats_.rtp_packets;
  } else {
    parsed.payload = packet;
  }

  if (parsed.payload.empty()) {
    return {};
  }

  if (parsed.payload.size() > static_cast<size_t>(std::numeric_limits<opus_int32>::max())) {
    throw AudioError("Opus packet is too large");
  }

  auto append_decoded = [this](const uint8_t* data, opus_int32 len, int frame_size, bool decode_fec,
                               bool allow_failure = false) {
    if (frame_size <= 0 || frame_size > max_frame_samples_) {
      if (allow_failure) {
        return OPUS_BAD_ARG;
      }
      throw AudioError("Invalid Opus frame size request");
    }

    const size_t old_size = output_.size();
    output_.resize(old_size + static_cast<size_t>(frame_size));
    auto* dst = output_.data() + static_cast<ptrdiff_t>(old_size);

    const int decoded = opus_decode_float(static_cast<OpusDecoder*>(decoder_), data, len, dst, frame_size,
                                          decode_fec ? 1 : 0);
    if (decoded < 0) {
      output_.resize(old_size);
      if (allow_failure) {
        return decoded;
      }
      throw AudioError("Opus decode failed: " + opus_error_to_string(decoded));
    }

    output_.resize(old_size + static_cast<size_t>(decoded));
    if (decoded > 0) {
      last_frame_samples_ = decoded;
      stats_.decoded_samples += static_cast<uint64_t>(decoded);
    }
    return decoded;
  };

  output_.clear();

  const auto* payload_ptr = parsed.payload.data();
  const auto  payload_len = static_cast<opus_int32>(parsed.payload.size());

  if (parsed.has_sequence && have_last_seq_) {
    const uint16_t delta = forward_sequence_delta(last_seq_, parsed.sequence);
    if (delta == 0U) {
      ++stats_.duplicate_packets;
      return {};
    }
    if (delta > 0x8000U) {
      ++stats_.out_of_order_packets;
      return {};
    }

    auto lost_packets = static_cast<uint16_t>(delta - 1U);
    if (lost_packets > 0U) {
      stats_.lost_packets += lost_packets;

      const int conceal_frame_size = std::clamp(last_frame_samples_, 1, max_frame_samples_);

      if (lost_packets == 1U && enable_fec_) {
        const int fec_decoded = append_decoded(payload_ptr, payload_len, conceal_frame_size, true, true);
        if (fec_decoded >= 0) {
          ++stats_.fec_packets;
          lost_packets = 0;
        } else {
          // FEC may be absent in incoming packets.
          spdlog::debug("Opus FEC decode skipped: {}", opus_error_to_string(fec_decoded));
        }
      }

      const size_t plc_packets = std::min(static_cast<size_t>(lost_packets), max_plc_packets_);
      for (size_t i = 0; i < plc_packets; ++i) {
        append_decoded(nullptr, 0, conceal_frame_size, false);
        ++stats_.plc_packets;
      }
    }
  }

  append_decoded(payload_ptr, payload_len, max_frame_samples_, false);
  ++stats_.decoded_packets;

  if (parsed.has_sequence) {
    have_last_seq_ = true;
    last_seq_      = parsed.sequence;
  }

  return {output_.data(), output_.size()};
}

void RealtimeOpusDecoder::reset() {
  if (decoder_ != nullptr) {
    const int rc = opus_decoder_ctl(static_cast<OpusDecoder*>(decoder_), OPUS_RESET_STATE);
    if (rc != OPUS_OK) {
      throw AudioError("Failed to reset Opus decoder: " + opus_error_to_string(rc));
    }
  }
  last_frame_samples_ = std::max(1, sample_rate_ / 50);
  have_last_seq_      = false;
  last_seq_           = 0;
  stats_              = {};
  output_.clear();
}

int RealtimeOpusDecoder::sample_rate() const noexcept {
  return sample_rate_;
}

const OpusDecodeStats& RealtimeOpusDecoder::stats() const noexcept {
  return stats_;
}

}  // namespace asr
