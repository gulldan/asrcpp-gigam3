#pragma once

#include <array>
#include <charconv>
#include <cstdio>
#include <string>
#include <string_view>

namespace asr {

inline void append_json_escaped(std::string& out, std::string_view input) {
  out.reserve(out.size() + input.size());
  for (const char c : input) {
    switch (c) {
      case '"':
        out.append("\\\"", 2);
        break;
      case '\\':
        out.append("\\\\", 2);
        break;
      case '\b':
        out.append("\\b", 2);
        break;
      case '\f':
        out.append("\\f", 2);
        break;
      case '\n':
        out.append("\\n", 2);
        break;
      case '\r':
        out.append("\\r", 2);
        break;
      case '\t':
        out.append("\\t", 2);
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20U) {
          std::array<char, 7> buf{};
          std::snprintf(buf.data(), buf.size(), "\\u%04x",
                        static_cast<unsigned>(static_cast<unsigned char>(c)));
          out.append(buf.data(), 6);
        } else {
          out.push_back(c);
        }
        break;
    }
  }
}

template <typename Integer>
inline void append_decimal(std::string& out, Integer value) {
  std::array<char, 32> buf{};
  const auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
  if (ec == std::errc()) {
    out.append(buf.data(), static_cast<size_t>(ptr - buf.data()));
  }
}

}  // namespace asr
