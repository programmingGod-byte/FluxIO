#pragma once

#include "file_state.h"
#include "../ring/ring_buffer.h"
#include "../ring/sq_ring.h"
#include "../ring/cq_ring.h"
#include "../util/fluxio_macros.h"
#include "../memory/slab_allocator.h"
#include <bit>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>
#include "fileBlockManager.h"

namespace flux {



using flux_sq = flux_ring_buffer<IORequest, QUEUE_ENTRY_SIZE>;
using flux_cq = flux_ring_buffer<IoCompletion, QUEUE_ENTRY_SIZE>;

using sqe_allocator = IndexMemPool<IORequest, QUEUE_ENTRY_SIZE>;
using cqe_allocator = IndexMemPool<IoCompletion, QUEUE_ENTRY_SIZE>;

template <std::size_t num_files = FILE_QUEUE_SIZE_ENTRY,
          std::size_t pages_per_file = PAGES_PER_FILE,
          std::size_t page_size_bytes = PAGE_SIZE_BYTES,
          bool allow_block_allocation=true,
          std::uint64_t chunk_size = 10*1024 * 1024>
class io_uring_storage_engine {
  static_assert(num_files <= FILE_QUEUE_SIZE_ENTRY,
                "Number of files cannot exceed FILE_QUEUE_SIZE_ENTRY");

public:
  io_uring_storage_engine() = default;

  io_uring_storage_engine(const std::vector<std::string> &file_paths) {
    for (size_t i = 0; i < file_paths.size() && i < num_files; ++i) {
      register_file(i, file_paths[i].c_str());
    }
    setup_io_uring();
  }

  ~io_uring_storage_engine() {
    if (initialized) {
      io_uring_unregister_buffers(&ring);
      io_uring_unregister_files(&ring);
      io_uring_queue_exit(&ring);
      cleanup_buffers();
      for (uint32_t i = 0; i < num_registered_files; ++i) {
        if (registered_fds[i] >= 0) {
          ::close(registered_fds[i]);
        }
      }
    }
  }

  FLUXIO_ATTR_GNU_COLD bool setup_io_uring() {
    if (initialized)
      return true;
    if (num_registered_files == 0)
      return false;

    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_SQPOLL;
    params.sq_thread_idle = 2000;

    uint32_t ring_depth =
        std::max(static_cast<uint32_t>(QUEUE_ENTRY_SIZE),
                 static_cast<uint32_t>(num_files * pages_per_file));

    if (io_uring_queue_init_params(ring_depth, &ring, &params) >= 0) {
      sqpoll_mode = true;
    } else {
      memset(&params, 0, sizeof(params));
      if (io_uring_queue_init_params(ring_depth, &ring, &params) < 0) {
        for (uint32_t i = 0; i < num_registered_files; ++i) {
          if (registered_fds[i] >= 0)
            ::close(registered_fds[i]);
        }
        return false;
      }
      sqpoll_mode = false;
    }

    if (io_uring_register_files(&ring, registered_fds.data(),
                                num_registered_files) < 0) {
      io_uring_queue_exit(&ring);
      return false;
    }

    for (uint32_t i = 0; i < num_registered_files; ++i) {
      for (uint32_t j = 0; j < pages_per_file; ++j) {
        void *ptr = nullptr;
        if (posix_memalign(&ptr, 4096, page_size_bytes) != 0) {
          cleanup_buffers();
          io_uring_unregister_files(&ring);
          io_uring_queue_exit(&ring);
          return false;
        }
        file_buffer_ptrs[i][j] = ptr;
        uint32_t idx = i * pages_per_file + j;
        registered_buffers[idx].iov_base = ptr;
        registered_buffers[idx].iov_len = page_size_bytes;
      }
    }

    if (io_uring_register_buffers(&ring, registered_buffers.data(),
                                  num_registered_files * pages_per_file) < 0) {
      cleanup_buffers();
      io_uring_unregister_files(&ring);
      io_uring_queue_exit(&ring);
      return false;
    }

    initialized = true;
    return true;
  }

  FLUXIO_ALWAYS_INLINE int
  return_file_descriptor(const char *path) const noexcept {
    int flags = O_RDWR | O_CREAT;
    int fd = -1;
    if (std::filesystem::exists(path)) {
      fd = ::open(path, flags | O_DIRECT, 0644);
      if (fd < 0 && errno == EINVAL) {
        fd = ::open(path, flags, 0644);
      }
    }
    return fd;
  }

  FLUXIO_ALWAYS_INLINE bool register_file(size_t slot,
                                          const char *absolute_path) noexcept {
    if (FLUXIO_UNLIKELY(slot >= num_files))
      return false;

    int fd = return_file_descriptor(absolute_path);
    if (FLUXIO_UNLIKELY(fd < 0)) {
      FLUXIO_SAFE_CHECK(Trait::always_false<bool>, "either the file ",
                        absolute_path,
                        " not exist or there is some error while opening it\n");
      return false;
    }
    registered_fds[slot] = fd;
    file_state[slot].init(pages_per_file);

    // enable block allocation
    if constexpr (FLUXIO_LIKELY(allow_block_allocation)){

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

  FLUXIO_ALWAYS_INLINE bool write_block_async(
      uint16_t file_slot, uint32_t buf_index, const void *data_src, size_t size,
      uint64_t file_offset, uint64_t user_data, uint64_t submit_ts_ns = 0) {

    FLUXIO_SAFE_CHECK(data_src != nullptr, "data_src pointer cannot be null in write");
    FLUXIO_SAFE_CHECK(size > 0, "write size must be greater than 0");
    FLUXIO_SAFE_CHECK(size <= page_size_bytes, "write size exceeds registered page_size_bytes buffer capacity (split writes into page_size_bytes chunks or increase page_size_bytes template argument)");
    FLUXIO_SAFE_CHECK(file_slot < num_registered_files, "file_slot out of bounds in write_block_async");

    if constexpr (FLUXIO_LIKELY(allow_block_allocation)){
      if (FLUXIO_UNLIKELY(!fileBlockManager.ensure_allocation(file_slot, file_offset, size))) {
        return false;
      }
    }

    FLUXIO_BUILTIN_PREFETCH(registered_buffers[buf_index].iov_base, 1, 3);
    FLUXIO_BUILTIN_PREFETCH(&inflight_data[buf_index], 1, 3);

    memcpy(registered_buffers[buf_index].iov_base, data_src, size);

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (FLUXIO_UNLIKELY(!sqe)) {
      return false;
    }
    io_uring_prep_write_fixed(sqe, file_slot,
                              registered_buffers[buf_index].iov_base, size,
                              file_offset, buf_index);
    sqe->flags |= IOSQE_FIXED_FILE;

    inflight_data[buf_index] = {user_data, submit_ts_ns, flux::Type::Write, nullptr};

    io_uring_sqe_set_data(
        sqe, reinterpret_cast<void *>(static_cast<uintptr_t>(buf_index)));

    return true;
  }

  FLUXIO_ALWAYS_INLINE bool read_block_async(
      uint16_t file_slot, uint32_t buf_index, void *data_dest, size_t size,
      uint64_t file_offset, uint64_t user_data, uint64_t submit_ts_ns = 0) {

    FLUXIO_SAFE_CHECK(data_dest != nullptr, "data_dest pointer cannot be null in read");
    FLUXIO_SAFE_CHECK(size > 0, "read size must be greater than 0");
    FLUXIO_SAFE_CHECK(size <= page_size_bytes, "read size exceeds registered page_size_bytes buffer capacity (split reads into page_size_bytes chunks or increase page_size_bytes template argument)");
    FLUXIO_SAFE_CHECK(file_slot < num_registered_files, "file_slot out of bounds in read_block_async");

    FLUXIO_BUILTIN_PREFETCH(registered_buffers[buf_index].iov_base, 1, 3);
    FLUXIO_BUILTIN_PREFETCH(&inflight_data[buf_index], 1, 3);

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (FLUXIO_UNLIKELY(!sqe)) {
      return false;
    }

    io_uring_prep_read_fixed(sqe, file_slot,
                             registered_buffers[buf_index].iov_base, size,
                             file_offset, buf_index);
    sqe->flags |= IOSQE_FIXED_FILE;

    inflight_data[buf_index] = {user_data, submit_ts_ns, flux::Type::Read, data_dest};

    io_uring_sqe_set_data(
        sqe, reinterpret_cast<void *>(static_cast<uintptr_t>(buf_index)));

    return true;
  }

  FLUXIO_ALWAYS_INLINE bool fsync_block_async(
      uint16_t file_slot, uint64_t user_data, uint64_t submit_ts_ns = 0) {

    FLUXIO_SAFE_CHECK(file_slot < num_registered_files, "file_slot out of bounds in fsync_block_async");

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (FLUXIO_UNLIKELY(!sqe)) {
      return false;
    }

    io_uring_prep_fsync(sqe, file_slot, IORING_FSYNC_DATASYNC);
    sqe->flags |= IOSQE_FIXED_FILE;

    inflight_fsync[file_slot] = {user_data, submit_ts_ns, flux::Type::Fsync, nullptr};

    uint64_t tag = FSYNC_TAG_BIT | static_cast<uint64_t>(file_slot);
    io_uring_sqe_set_data(sqe, reinterpret_cast<void *>(static_cast<uintptr_t>(tag)));

    return true;
  }

  FLUXIO_ALWAYS_INLINE bool read_block(uint16_t file_slot, void *data_dest,
                                       size_t size, uint64_t file_offset,
                                       uint64_t user_data) {
    FLUXIO_SAFE_CHECK(initialized, "io_uring storage engine not initialized");
    FLUXIO_SAFE_CHECK(file_slot < num_registered_files, "file_slot out of bounds in read_block");
    FLUXIO_SAFE_CHECK(data_dest != nullptr, "data_dest pointer cannot be null in read_block");
    FLUXIO_SAFE_CHECK(size > 0, "read size must be greater than 0");
    FLUXIO_SAFE_CHECK(size <= page_size_bytes, "read size exceeds registered page_size_bytes buffer capacity");

    int local_page = file_state[file_slot].alloc_page();
    if (FLUXIO_UNLIKELY(local_page < 0)) {
      return false;
    }
    uint32_t buf_index = (file_slot << shifter) + local_page;
    bool ok = read_block_async(file_slot, buf_index, data_dest, size, file_offset,
                               user_data, __builtin_ia32_rdtsc());
    if (FLUXIO_UNLIKELY(!ok)) {
      file_state[file_slot].free_page(local_page);
      return false;
    }
    return true;
  }

  FLUXIO_ALWAYS_INLINE bool fsync_block(uint16_t file_slot, uint64_t user_data) {
    FLUXIO_SAFE_CHECK(initialized, "io_uring storage engine not initialized");
    FLUXIO_SAFE_CHECK(file_slot < num_registered_files, "file_slot out of bounds in fsync_block");
    return fsync_block_async(file_slot, user_data, __builtin_ia32_rdtsc());
  }

  FLUXIO_ALWAYS_INLINE void poll_completion() {
    struct io_uring_cqe *cqe = nullptr;
    while (io_uring_peek_cqe(&ring, &cqe) == 0) {
      FLUXIO_BUILTIN_PREFETCH(cqe + 1, 0, 3);

      uint64_t tag = reinterpret_cast<uintptr_t>(io_uring_cqe_get_data(cqe));

      if (FLUXIO_UNLIKELY((tag & FSYNC_TAG_BIT) != 0)) {
        uint16_t file_slot = static_cast<uint16_t>(tag & ~FSYNC_TAG_BIT);
        const io_uring_data &data = inflight_fsync[file_slot];

        IoCompletion comp{};
        comp.user_data      = data.user_data;
        comp.result         = cqe->res;
        comp.submit_ts_ns   = data.program_submit_time;
#if defined(__x86_64__) || defined(_M_X64)
        comp.complete_ts_ns = __builtin_ia32_rdtsc();
#else
        comp.complete_ts_ns = 0;
#endif
        comp.file_slot  = file_slot;
        comp.local_page = 0;
        comp.op_type    = flux::Type::Fsync;
        comp.failed     = (cqe->res < 0);

        uint32_t backoff = 1;
        while (FLUXIO_UNLIKELY(!cq_ring.push(std::move(comp)))) {
          for (uint32_t i = 0; i < backoff; ++i) {
            FLUXIO_PAUSE_CPU_INSTRUCTION;
          }
          if (backoff < 16) {
            backoff <<= 1;
          }
        }
        continue;
      }

      uint32_t buf_index = static_cast<uint32_t>(tag);
      const io_uring_data &data = inflight_data[buf_index];

      uint16_t file_slot = static_cast<uint16_t>(buf_index >> shifter);
      uint8_t local_page =
          static_cast<uint8_t>(buf_index & (pages_per_file - 1));

      IoCompletion comp{};
      comp.user_data = data.user_data;
      comp.result = cqe->res;
      comp.submit_ts_ns = data.program_submit_time;
#if defined(__x86_64__) || defined(_M_X64)
      comp.complete_ts_ns = __builtin_ia32_rdtsc();
#else
      comp.complete_ts_ns = 0;
#endif
      comp.file_slot = file_slot;
      comp.local_page = local_page;
      comp.op_type = data.type;
      comp.failed = (cqe->res < 0);

      if (data.type == flux::Type::Read) {
        if (FLUXIO_LIKELY(cqe->res > 0 && data.dest_buf != nullptr)) {
          memcpy(data.dest_buf, registered_buffers[buf_index].iov_base, cqe->res);
        }
      }

      file_state[file_slot].free_page(local_page);
      io_uring_cqe_seen(&ring, cqe);

      uint32_t backoff = 1;
      while (FLUXIO_UNLIKELY(!cq_ring.push(std::move(comp)))) {
        for (uint32_t i = 0; i < backoff; ++i) {
          FLUXIO_PAUSE_CPU_INSTRUCTION;
        }
        if (backoff < 16) {
          backoff <<= 1;
        }
      }
    }
  }

  FLUXIO_ALWAYS_INLINE bool try_pop_completion(IoCompletion &out) noexcept {
    return cq_ring.pop(out);
  }

  FLUXIO_ALWAYS_INLINE bool try_push_request(IORequest &&request) noexcept {
    return sq_ring.push(std::move(request));
  }

  FLUXIO_ALWAYS_INLINE bool try_push_request(const IORequest &request) noexcept {
    return sq_ring.push(request);
  }

  FLUXIO_ALWAYS_INLINE void push_request(IORequest &&request) noexcept {
    uint32_t backoff = 1;
    while (FLUXIO_UNLIKELY(!sq_ring.push(std::move(request)))) {
      for (uint32_t i = 0; i < backoff; ++i) {
        FLUXIO_PAUSE_CPU_INSTRUCTION;
      }
      if (backoff < 16) {
        backoff <<= 1;
      }
    }
  }

  FLUXIO_ALWAYS_INLINE void push_request(const IORequest &request) noexcept {
    IORequest req = request;
    push_request(std::move(req));
  }

  FLUXIO_ALWAYS_INLINE void submit() noexcept {
    io_uring_submit(&ring);
  }

  FLUXIO_ALWAYS_INLINE int process_submissions(uint32_t max_batch = 64) {
    uint32_t processed = 0;
    while (processed < max_batch) {
      IORequest *req = sq_ring.peek();
      if (!req) {
        break;
      }
      if (FLUXIO_UNLIKELY(!request_handler(*req))) {
        break;
      }
      sq_ring.consume();
      ++processed;
    }
    if (processed > 0) {
      io_uring_submit(&ring);
    } else if (FLUXIO_UNLIKELY(sqpoll_mode && (*ring.sq.kflags & IORING_SQ_NEED_WAKEUP))) {
      io_uring_submit(&ring);
    }
    return static_cast<int>(processed);
  }

  FLUXIO_ALWAYS_INLINE int process_submissions(flux_sq &sq, uint32_t max_batch = 64) {
    uint32_t processed = 0;
    while (processed < max_batch) {
      IORequest *req = sq.peek();
      if (!req) {
        break;
      }
      if (FLUXIO_UNLIKELY(!request_handler(*req))) {
        break;
      }
      sq.consume();
      ++processed;
    }
    if (processed > 0) {
      io_uring_submit(&ring);
    } else if (FLUXIO_UNLIKELY(sqpoll_mode && (*ring.sq.kflags & IORING_SQ_NEED_WAKEUP))) {
      io_uring_submit(&ring);
    }
    return static_cast<int>(processed);
  }

  FLUXIO_ALWAYS_INLINE bool request_handler(const IORequest &request) {
    if (FLUXIO_UNLIKELY(!initialized)) {
      FLUXIO_SAFE_CHECK(Trait::always_false<bool>, "io_uring not initialized");
      return false;
    }
    if (FLUXIO_UNLIKELY(request.file_slot >= num_registered_files)) {
      FLUXIO_SAFE_CHECK(Trait::always_false<bool>, "request.file_slot exceeds registered file count");
      return false;
    }
    if (FLUXIO_UNLIKELY(request.op_type != flux::Type::Fsync && request.length > page_size_bytes)) {
      FLUXIO_SAFE_CHECK(Trait::always_false<bool>, "request.length exceeds page_size_bytes buffer capacity! Split your request into page_size_bytes chunks or increase page_size_bytes template argument.");
      return false;
    }

    uint16_t file_slot = request.file_slot;

    if (FLUXIO_LIKELY(request.op_type == flux::Type::Write)) {
      FLUXIO_BUILTIN_PREFETCH(&file_state[file_slot], 1, 3);
      int local_page = file_state[file_slot].alloc_page();
      if (FLUXIO_UNLIKELY(local_page < 0)) {
        return false;
      }

      uint32_t buff_index = (file_slot << shifter) + local_page;

      bool val = write_block_async(file_slot, buff_index, request.data_src,
                                   request.length, request.file_offset,
                                   request.user_data, request.timestamp_ns);
      if (FLUXIO_UNLIKELY(!val)) {
        file_state[file_slot].free_page(local_page);
        return false;
      }

      return true;
    } else if (request.op_type == flux::Type::Read) {
      FLUXIO_BUILTIN_PREFETCH(&file_state[file_slot], 1, 3);
      int local_page = file_state[file_slot].alloc_page();
      if (FLUXIO_UNLIKELY(local_page < 0)) {
        return false;
      }

      uint32_t buff_index = (file_slot << shifter) + local_page;
      void *dest = const_cast<void *>(request.data_src);

      bool val = read_block_async(file_slot, buff_index, dest,
                                  request.length, request.file_offset,
                                  request.user_data, request.timestamp_ns);
      if (FLUXIO_UNLIKELY(!val)) {
        file_state[file_slot].free_page(local_page);
        return false;
      }

      return true;
    } else if (request.op_type == flux::Type::Fsync) {
      return fsync_block_async(file_slot, request.user_data, request.timestamp_ns);
    }

    return false;
  }

  struct io_uring_data {
    uint64_t user_data{0};
    uint64_t program_submit_time{0};
    flux::Type type{flux::Type::Write};
    void *dest_buf{nullptr};
  };

private:
  std::array<int, num_files> registered_fds = []() {
    std::array<int, num_files> arr;
    arr.fill(-1);
    return arr;
  }();

  std::array<FileState, num_files> file_state{};
  std::array<struct iovec, num_files * pages_per_file> registered_buffers{};
  std::array<std::array<void *, pages_per_file>, num_files> file_buffer_ptrs{};
  std::array<io_uring_data, num_files * pages_per_file> inflight_data{};
  static constexpr uint64_t FSYNC_TAG_BIT = 1ULL << 63;
  std::array<io_uring_data, num_files> inflight_fsync{};
  flux_sq sq_ring{};
  flux_cq cq_ring{};
  static constexpr uint32_t shifter = std::countr_zero(pages_per_file);
  uint32_t num_registered_files{0};
  struct io_uring ring {};
  bool initialized{false};
  bool sqpoll_mode{false};
  FileBlockManager<chunk_size,num_files> fileBlockManager;

  FLUXIO_ATTR_GNU_COLD void cleanup_buffers() noexcept {
    for (uint32_t i = 0; i < num_registered_files; ++i) {
      for (uint32_t j = 0; j < pages_per_file; ++j) {
        if (file_buffer_ptrs[i][j]) {
          std::free(file_buffer_ptrs[i][j]);
          file_buffer_ptrs[i][j] = nullptr;
        }
      }
    }
  }
};

} // namespace flux
