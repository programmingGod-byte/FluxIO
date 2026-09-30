#pragma once
#include "ring_buffer.h"
#include <cstdint>

namespace flux {

enum class Type : uint8_t { Read, Write, Fsync, Close };
struct alignas(implementation::hardware_destructive_interference_size) IORequest {
  Type op_type;         // 1 byte
  uint16_t file_slot;   // 2 bytes (Fix: allows up to 65535 files, we need 2096)
  uint8_t local_page;   // 1 byte
  // Total: 4 bytes. Perfect alignment for the 32-bit length!
  uint32_t length;      // 4 bytes (offset 4)

  uint64_t file_offset; // 8 bytes (offset 8)
  uint64_t user_data;   // 8 bytes (offset 16)
  uint64_t timestamp_ns;// 8 bytes (offset 24)
  
  const void* data_src; // 8 bytes: Pointer to user's source data to write! (offset 32)

  // Remaining padding to reach 64 bytes
  char _fill[8];
};

static_assert(sizeof(IORequest) == 64, "IORequest must be exactly 64 bytes");
} // namespace flux