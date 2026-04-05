#include <_ctype.h>

#include <cctype>
#include <cstdint>
#include <string_view>
#include <vector>

#include "asr/audio.h"

namespace asr {
namespace {

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

}  // namespace

std::vector<uint8_t> base64_decode(std::string_view input) {
  std::vector<uint8_t> out;
  base64_decode_into(input, out);
  return out;
}

void base64_decode_into(std::string_view input, std::vector<uint8_t>& out) {
  out.clear();
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

    accum = (accum << 6U) | static_cast<uint32_t>(v);
    accum_bits += 6;
    if (accum_bits >= 8) {
      accum_bits -= 8;
      out.push_back(static_cast<uint8_t>((accum >> static_cast<unsigned>(accum_bits)) & 0xFFU));
    }
  }

  if (accum_bits >= 6) {
    throw AudioError("Invalid base64 payload: dangling bits");
  }
}

}  // namespace asr
