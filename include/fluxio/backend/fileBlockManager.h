#pragma once

#include <linux/falloc.h>
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "../util/fluxio_macros.h"
#include "file_state.h"
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
  uint64_t allocated_block_size{0}; // how many bytes we pre allocated on disk 
  uint64_t max_written_offset{0}; // the furthese byte offeset written so far
  int fd{-1};

  FileBlock() = default;

  explicit FileBlock(int fd)
      : current_file_size_logical(get_file_size_logical(fd)),
        allocated_block_size(0),
        max_written_offset(0),
        fd(fd) {
    FLUXIO_SAFE_CHECK(fd >= 0, "file descriptor should be non-negative");
    allocate_chunk(0,chunk_size);
  }

  FLUXIO_ALWAYS_INLINE static uint64_t get_file_size_logical(int fd) noexcept {
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) != 0) {
      return 0;
    }
    return static_cast<uint64_t>(st.st_size);
  }

  FLUXIO_ALWAYS_INLINE bool allocate_chunk(uint64_t offset,uint64_t bytes_to_add ) noexcept {
    if(FLUXIO_UNLIKELY(fd < 0 || bytes_to_add ==0)) return false;

    // physical disk allocation
    int res = ::fallocate(fd, 0, offset, bytes_to_add);
    if(FLUXIO_UNLIKELY(res==-1)){
      // fallback to the KEEP_SIZE
      res = ::fallocate(fd, FALLOC_FL_KEEP_SIZE, offset, bytes_to_add);
      if(FLUXIO_UNLIKELY(res==-1)){
        return false;
      }
    }

    // write zeros in 2 mb chunks
    constexpr size_t ZERO_BUF_SIZE = 2 * 1024 * 1024;
    alignas(POSIX_MEMALIGN_BYTES_SIZE) static const char zero_buf[ZERO_BUF_SIZE]{0};

    uint64_t written = 0;
    while (written < bytes_to_add) {
      size_t to_write = static_cast<size_t>(std::min<uint64_t>(ZERO_BUF_SIZE,bytes_to_add - written));
      ssize_t w = ::pwrite(fd, zero_buf, to_write, offset + written);

      if(FLUXIO_UNLIKELY(w <=0)){
        break;
      }

      written+=static_cast<uint64_t>(w);
    }

    allocated_block_size+=bytes_to_add;
    return true;
  }

  FLUXIO_ALWAYS_INLINE bool ensure_allocation(uint64_t write_offset, uint64_t write_size) noexcept {
    uint64_t target_offset = write_offset + write_size;
    if (target_offset > max_written_offset) {
      max_written_offset = target_offset;
    }

    uint64_t watermark = (allocated_block_size * 4) / 5;
    if (FLUXIO_UNLIKELY(target_offset >= watermark)) {
      uint64_t needed_size = ((target_offset + chunk_size - 1) / chunk_size) * chunk_size + chunk_size;
      uint64_t bytes_to_add = needed_size - allocated_block_size;

      if (bytes_to_add > 0) {
        if (!allocate_chunk(allocated_block_size, bytes_to_add)) {
          return false;
        }
      }
    }
    return true;
  }
};


template <std::size_t chunk_size = 10 * 1024 * 1024,
          std::size_t num_files = DEFAULT_NUM_FILES>
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