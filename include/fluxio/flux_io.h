#pragma once

#ifndef FLUX_HAS_IO_URING
  #if __has_include(<liburing.h>)
    #define FLUX_HAS_IO_URING 1
  #else
    #define FLUX_HAS_IO_URING 0
  #endif
#endif

#include "util/aethon.h"
#include "util/rdtsc.h"
#include "util/cpu_affinity.h"
#include "ring/ring_buffer.h"
#include "ring/sq_ring.h"
#include "ring/cq_ring.h"
#include "memory/slab_allocator.h"
#include "backend/file_state.h"
#include "backend/fileBlockManager.h"
#include "backend/posix_backend.h"
#include "concepts/backend_concept.h"

#if FLUX_HAS_IO_URING
  #include "backend/io_uring_backend.h"
#endif

namespace flux {

#if FLUX_HAS_IO_URING
template <std::size_t num_files = FILE_QUEUE_SIZE_ENTRY,
          std::size_t pages_per_file = PAGES_PER_FILE,
          std::size_t page_size_bytes = PAGE_SIZE_BYTES,
          bool allow_block_allocation = true,
          std::uint64_t chunk_size = 10 * 1024 * 1024>
using flux_storage_engine = io_uring_storage_engine<num_files, pages_per_file, page_size_bytes, allow_block_allocation, chunk_size>;
#else
template <std::size_t num_files = FILE_QUEUE_SIZE_ENTRY,
          std::size_t pages_per_file = PAGES_PER_FILE,
          std::size_t page_size_bytes = PAGE_SIZE_BYTES,
          bool allow_block_allocation = true,
          std::uint64_t chunk_size = 10 * 1024 * 1024>
using flux_storage_engine = posix_storage_engine<num_files, pages_per_file, page_size_bytes, allow_block_allocation, chunk_size>;
#endif

using DefaultStorageEngine = flux_storage_engine<>;

} // namespace flux
