#pragma once

#include "../util/aethon.h"
#include <array>
#include <string_view>
#include <cstdint>
#include <cstddef>
#include <algorithm>

namespace flux {

static constexpr uint32_t QUEUE_ENTRY_SIZE = 4096;
static constexpr uint32_t FILE_QUEUE_SIZE_ENTRY = 2096;
static constexpr uint32_t POSIX_MEMALIGN_BYTES_SIZE = 4096;
static constexpr uint32_t PAGES_PER_FILE = 32;
static constexpr uint32_t PAGE_SIZE_BYTES = 4096;
static constexpr uint32_t PAGES_PER_FILE_SHIFT = 5;

template <size_t N> struct FixedString {
  std::array<char, N + 1> data{};
  uint16_t size{0};

  constexpr FixedString() = default;

  constexpr FixedString(std::string_view sv) noexcept { assign(sv); }

  constexpr FixedString(const char *str) noexcept {
    if (str)
      assign(std::string_view(str));
  }

  AETHON_ALWAYS_INLINE void assign(std::string_view sv) noexcept {
    size_t len = std::min(sv.size(), N);
    for (size_t i = 0; i < len; ++i) {
      data[i] = sv[i];
    }
    data[len] = '\0';
    size = static_cast<uint16_t>(len);
  }

  AETHON_ALWAYS_INLINE void insert(const char *c, size_t len) noexcept {
    AETHON_SAFE_CHECK(len <= N, "Size exceeds fixed capacity");
    for (size_t i = 0; i < len; ++i) {
      data[i] = c[i];
    }
    data[len] = '\0';
    size = static_cast<uint16_t>(len);
  }

  constexpr std::string_view view() const noexcept {
    return {data.data(), size};
  }

  constexpr const char *c_str() const noexcept { return data.data(); }
};

struct FileEntry {
  FixedString<512> path{};
  FixedString<64> name{};
  int fd{-1};
};

struct alignas(implementation::hardware_destructive_interference_size) FileState {
  std::array<uint64_t, 2> free_mask{0xFFFFFFFFULL, 0ULL};

  uint8_t _pad[implementation::hardware_destructive_interference_size -
               sizeof(std::array<uint64_t, 2>)]{};

  AETHON_ALWAYS_INLINE void init(uint32_t num_pages) noexcept {
    if (num_pages >= 128) {
      free_mask[0] = ~0ULL;
      free_mask[1] = ~0ULL;
    } else if (num_pages >= 64) {
      free_mask[0] = ~0ULL;
      uint32_t rem = num_pages - 64;
      free_mask[1] = (rem == 0) ? 0ULL : ((1ULL << rem) - 1ULL);
    } else if (num_pages == 0) {
      free_mask[0] = 0ULL;
      free_mask[1] = 0ULL;
    } else {
      free_mask[0] = (1ULL << num_pages) - 1ULL;
      free_mask[1] = 0ULL;
    }
  }

  AETHON_ALWAYS_INLINE int alloc_page() noexcept {
    if (free_mask[0] != 0) {
      int page = __builtin_ctzll(free_mask[0]);
      free_mask[0] &= ~(1ULL << page);
      return page;
    }
    if (free_mask[1] != 0) {
      int bit = __builtin_ctzll(free_mask[1]);
      free_mask[1] &= ~(1ULL << bit);
      return 64 + bit;
    }
    return -1;
  }

  AETHON_ALWAYS_INLINE void free_page(int page) noexcept {
    if (page < 64) {
      free_mask[0] |= (1ULL << page);
    } else {
      free_mask[1] |= (1ULL << (page - 64));
    }
  }
};

static_assert(
    sizeof(FileState) == implementation::hardware_destructive_interference_size,
    "FileState size must match hardware_destructive_interference_size!");
static_assert(
    alignof(FileState) == implementation::hardware_destructive_interference_size,
    "FileState alignment must match hardware_destructive_interference_size!");

} // namespace flux
