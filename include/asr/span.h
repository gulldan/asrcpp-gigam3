#pragma once

#include <cstddef>
#include <type_traits>

namespace asr {

/// Lightweight C++17 span polyfill (read-only view over contiguous data)
template <typename T>
class span {
 public:
  constexpr span() noexcept : data_(nullptr), size_(0) {}
  constexpr span(T* data, size_t size) noexcept : data_(data), size_(size) {}

  template <typename Container, typename = std::enable_if_t<!std::is_same_v<std::decay_t<Container>, span>>>
  // NOLINTNEXTLINE(google-explicit-constructor)
  constexpr span(Container& c) noexcept : data_(c.data()), size_(c.size()) {}

  [[nodiscard]] constexpr T* data() const noexcept {
    return data_;
  }
  [[nodiscard]] constexpr size_t size() const noexcept {
    return size_;
  }
  [[nodiscard]] constexpr bool empty() const noexcept {
    return size_ == 0;
  }
  constexpr T& operator[](size_t i) const {  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return data_[i];                         // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }
  [[nodiscard]] constexpr T* begin() const noexcept {
    return data_;
  }
  [[nodiscard]] constexpr T* end() const noexcept {
    return data_ + size_;  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }
  [[nodiscard]] constexpr T& back() const noexcept {
    return data_[size_ - 1];  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }

  [[nodiscard]] constexpr span subspan(size_t offset, size_t count) const {
    return span(data_ + offset, count);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }

 private:
  T*     data_;
  size_t size_;
};

}  // namespace asr
