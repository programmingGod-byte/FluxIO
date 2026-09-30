#pragma once

#include "file_state.h"
#include "fileBlockManager.h"
#include "../ring/ring_buffer.h"
#include "../ring/sq_ring.h"
#include "../ring/cq_ring.h"
#include "../util/fluxio_macros.h"
#include <fcntl.h>
#include <unistd.h>
#include <array>
#include <vector>
#include <string>
#include <cstdint>
#include <cstddef>

namespace flux {

template <std::size_t num_files = FILE_QUEUE_SIZE_ENTRY,
          std::size_t pages_per_file = PAGES_PER_FILE,
          std::size_t page_size_bytes = PAGE_SIZE_BYTES,
          bool allow_block_allocation = true,
          std::uint64_t chunk_size = 10 * 1024 * 1024>
class posix_storage_engine {
public:
  posix_storage_engine() = default;

  posix_storage_engine(const std::vector<std::string> &file_paths) {
    for (size_t i = 0; i < file_paths.size() && i < num_files; ++i) {
      register_file(i, file_paths[i].c_str());
    }
  }

  ~posix_storage_engine() {
    for (uint32_t i = 0; i < num_registered_files; ++i) {
      if (registered_fds[i] >= 0) {
        ::close(registered_fds[i]);
      }
    }
  }

  FLUXIO_ALWAYS_INLINE bool setup_io_uring() noexcept { return true; }
  FLUXIO_ALWAYS_INLINE bool setup() noexcept { return true; }

  FLUXIO_ALWAYS_INLINE bool register_file(size_t slot, const char *path) noexcept {
    FLUXIO_SAFE_CHECK(slot < num_files, "slot out of bounds in register_file");
    FLUXIO_SAFE_CHECK(path != nullptr, "path cannot be null in register_file");

    if (slot >= num_files) return false;
    int fd = ::open(path, O_RDWR | O_CREAT | O_DIRECT, 0644);
    if (fd < 0 && errno == EINVAL) {
      fd = ::open(path, O_RDWR | O_CREAT, 0644);
    }
    if (fd < 0) return false;

    registered_fds[slot] = fd;

    if constexpr (FLUXIO_LIKELY(allow_block_allocation)) {
      fileBlockManager.register_file(slot, fd);
    }

    if (slot >= num_registered_files) {
      num_registered_files = static_cast<uint32_t>(slot + 1);
    }
    return true;
  }

  FLUXIO_ALWAYS_INLINE int get_fd(size_t file_slot) const noexcept {
    return registered_fds[file_slot];
  }

  FLUXIO_ALWAYS_INLINE uint32_t file_count() const noexcept {
    return num_registered_files;
  }

  FLUXIO_ALWAYS_INLINE bool try_push_request(IORequest &&request) noexcept {
    return sq_ring.push(std::move(request));
  }

  FLUXIO_ALWAYS_INLINE bool try_push_request(const IORequest &request) noexcept {
    return sq_ring.push(request);
  }

  FLUXIO_ALWAYS_INLINE void push_request(IORequest &&request) noexcept {
    uint32_t backoff = 1;
    while (!sq_ring.push(std::move(request))) {
      for (uint32_t i = 0; i < backoff; ++i) {
        FLUXIO_PAUSE_CPU_INSTRUCTION;
      }
      if (backoff < 16) backoff <<= 1;
    }
  }

  FLUXIO_ALWAYS_INLINE void push_request(const IORequest &request) noexcept {
    IORequest req = request;
    push_request(std::move(req));
  }

  FLUXIO_ALWAYS_INLINE void submit() noexcept {
    process_submissions();
  }

  FLUXIO_ALWAYS_INLINE void poll_completion() noexcept {
    // POSIX backend handles submissions synchronously into cq_ring
  }

  int process_submissions(uint32_t max_batch = 64) {
    uint32_t processed = 0;
    while (processed < max_batch) {
      IORequest *req = sq_ring.peek();
      if (!req) break;

      FLUXIO_SAFE_CHECK(req->file_slot < num_registered_files, "file_slot out of bounds");
      int fd = registered_fds[req->file_slot];

      IoCompletion comp{};
      comp.user_data = req->user_data;
      comp.submit_ts_ns = req->timestamp_ns;
      comp.file_slot = req->file_slot;
      comp.op_type = req->op_type;

      if (req->op_type == Type::Write) {
        FLUXIO_SAFE_CHECK(req->data_src != nullptr, "data_src cannot be null");
        FLUXIO_SAFE_CHECK(req->length > 0, "write length must be > 0");
        if constexpr (FLUXIO_LIKELY(allow_block_allocation)) {
          fileBlockManager.ensure_allocation(req->file_slot, req->file_offset, req->length);
        }
        ssize_t w = ::pwrite(fd, req->data_src, req->length, req->file_offset);
        comp.result = w;
        comp.failed = (w < 0);
      } else if (req->op_type == Type::Read) {
        FLUXIO_SAFE_CHECK(req->data_src != nullptr, "destination buffer cannot be null");
        FLUXIO_SAFE_CHECK(req->length > 0, "read length must be > 0");
        ssize_t r = ::pread(fd, const_cast<void*>(req->data_src), req->length, req->file_offset);
        comp.result = r;
        comp.failed = (r < 0);
      } else if (req->op_type == Type::Fsync) {
        int s = ::fdatasync(fd);
        comp.result = s;
        comp.failed = (s < 0);
      }

#if defined(__x86_64__) || defined(_M_X64)
      comp.complete_ts_ns = __builtin_ia32_rdtsc();
#else
      comp.complete_ts_ns = 0;
#endif

      sq_ring.consume();
      while (!cq_ring.push(std::move(comp))) {
        FLUXIO_PAUSE_CPU_INSTRUCTION;
      }
      ++processed;
    }
    return static_cast<int>(processed);
  }

  FLUXIO_ALWAYS_INLINE bool try_pop_completion(IoCompletion &out) noexcept {
    return cq_ring.pop(out);
  }

  FLUXIO_ALWAYS_INLINE bool read_block(uint16_t file_slot, void *data_dest,
                                       size_t size, uint64_t file_offset,
                                       uint64_t user_data) {
    FLUXIO_SAFE_CHECK(file_slot < num_registered_files, "file_slot out of bounds in read_block");
    FLUXIO_SAFE_CHECK(data_dest != nullptr, "data_dest cannot be null in read_block");
    FLUXIO_SAFE_CHECK(size > 0, "read size must be > 0");

    IORequest req{};
    req.op_type = Type::Read;
    req.file_slot = file_slot;
    req.length = static_cast<uint32_t>(size);
    req.file_offset = file_offset;
    req.user_data = user_data;
    req.data_src = data_dest;
#if defined(__x86_64__) || defined(_M_X64)
    req.timestamp_ns = __builtin_ia32_rdtsc();
#endif
    push_request(std::move(req));
    process_submissions(1);
    return true;
  }

  FLUXIO_ALWAYS_INLINE bool fsync_block(uint16_t file_slot, uint64_t user_data) {
    FLUXIO_SAFE_CHECK(file_slot < num_registered_files, "file_slot out of bounds in fsync_block");
    IORequest req{};
    req.op_type = Type::Fsync;
    req.file_slot = file_slot;
    req.user_data = user_data;
#if defined(__x86_64__) || defined(_M_X64)
    req.timestamp_ns = __builtin_ia32_rdtsc();
#endif
    push_request(std::move(req));
    process_submissions(1);
    return true;
  }

private:
  std::array<int, num_files> registered_fds = []() {
    std::array<int, num_files> arr;
    arr.fill(-1);
    return arr;
  }();
  uint32_t num_registered_files{0};
  flux_ring_buffer<IORequest, QUEUE_ENTRY_SIZE> sq_ring{};
  flux_ring_buffer<IoCompletion, QUEUE_ENTRY_SIZE> cq_ring{};
  FileBlockManager<chunk_size, num_files> fileBlockManager{};
};

} // namespace flux
