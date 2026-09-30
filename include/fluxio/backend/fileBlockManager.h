#pragma once

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "../util/fluxio_macros.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cstddef>
#include <cstdint>
#include <array>
#include <algorithm>

namespace flux {

template <std::size_t chunk_size = 10 * 1024 * 1024>
struct FileBlock {
  uint64_t current_file_size_logical{0};
  uint64_t allocated_block_size{0};
  uint64_t max_written_offset{0};
  int fd{-1};

  FileBlock() = default;

  explicit FileBlock(int fd)
      : current_file_size_logical(get_file_size_logical(fd)),
        allocated_block_size(0),
        max_written_offset(0),
        fd(fd) {
    FLUXIO_SAFE_CHECK(fd >= 0, "file descriptor should be non-negative");
    allocate_chunk(0);
  }

  FLUXIO_ALWAYS_INLINE static uint64_t get_file_size_logical(int fd) noexcept {
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) != 0) {
      return 0;
    }
    return static_cast<uint64_t>(st.st_size);
  }

  FLUXIO_ALWAYS_INLINE bool allocate_chunk(uint64_t offset) noexcept {
    if (FLUXIO_UNLIKELY(fd < 0)) return false;
    int res = ::fallocate(fd, FALLOC_FL_KEEP_SIZE, offset, chunk_size);
    if (FLUXIO_UNLIKELY(res == -1)) {
      return false;
    }
    allocated_block_size += chunk_size;
    return true;
  }

  FLUXIO_ALWAYS_INLINE bool ensure_allocation(uint64_t write_offset, uint64_t write_size) noexcept {
    uint64_t target_offset = write_offset + write_size;
    if (target_offset > max_written_offset) {
      max_written_offset = target_offset;
    }

    uint64_t watermark = (allocated_block_size * 4) / 5;
    while (target_offset >= watermark) {
      if (FLUXIO_UNLIKELY(!allocate_chunk(allocated_block_size))) {
        return false;
      }
      watermark = (allocated_block_size * 4) / 5;
    }
    return true;
  }
};


template <std::size_t chunk_size = 10 * 1024 * 1024,
          std::size_t num_files = 2096>
class FileBlockManager {
public:
  FileBlockManager() = default;

  bool register_file(size_t slot, int fd) noexcept {
    if (FLUXIO_UNLIKELY(slot >= num_files || fd < 0)) {
      return false;
    }
    files[slot] = FileBlock<chunk_size>(fd);
    return true;
  }

  FLUXIO_ALWAYS_INLINE bool ensure_allocation(size_t slot, uint64_t offset, uint64_t size) noexcept {
    if (FLUXIO_UNLIKELY(slot >= num_files)) {
      return false;
    }
    return files[slot].ensure_allocation(offset, size);
  }

//   FLUXIO_ALWAYS_INLINE FileBlock<chunk_size>& get_block(size_t slot) noexcept {
//     return files[slot];
//   }

//   FLUXIO_ALWAYS_INLINE const FileBlock<chunk_size>& get_block(size_t slot) const noexcept {
//     return files[slot];
//   }

private:
  std::array<FileBlock<chunk_size>, num_files> files{};
};

} // namespace flux