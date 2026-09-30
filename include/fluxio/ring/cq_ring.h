// ring/cq_ring.h
#pragma once
#include "ring_buffer.h"
#include "sq_ring.h"

namespace flux {

struct alignas(64) IoCompletion {
    uint64_t user_data;      
    int64_t  result;         // Bytes read/written, or negative errno
    uint64_t submit_ts_ns;   // RDTSC when submitted
    uint64_t complete_ts_ns; // RDTSC when completed (latency = complete - submit)
    uint16_t file_slot;      // Which file this completion belongs to
    uint8_t  local_page;     // Which 4KB page to recycle via FileState::free_page()
    bool     failed;         // True if result < 0
    Type     op_type;        // Operation type (Read, Write, Fsync, Close)
    char     _pad[27];       // Pad to exactly 64 bytes

    FLUXIO_ALWAYS_INLINE Type type() const noexcept { return op_type; }
};
static_assert(sizeof(IoCompletion) == 64, "IoCompletion must be exactly 1 cache line!");

using CqRing = flux_ring_buffer<IoCompletion, 1024>;

} // namespace flux