#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace asr {

inline std::string trim_ascii(std::string_view input) {
  constexpr std::string_view kWhitespace = " \t\n\r\f\v";
  const auto                 begin       = input.find_first_not_of(kWhitespace);
  if (begin == std::string_view::npos) {
    return {};
  }
  const auto end = input.find_last_not_of(kWhitespace);
  return std::string(input.substr(begin, end - begin + 1));
}

inline std::string to_lower_ascii(std::string_view input) {
  std::string out(input.begin(), input.end());
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return out;
}

}  // namespace asr
