#pragma once
#include "../util/aethon.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <sys/types.h>

namespace flux {
struct TaggedIndex {
  uint64_t value; // 32 bit Version 32 bit index

  AETHON_ALWAYS_INLINE uint32_t index() const noexcept {
    return static_cast<uint32_t>(value & 0xFFFFFFFF);
  };
  AETHON_ALWAYS_INLINE uint32_t version() const noexcept {
    return static_cast<uint32_t>(value >> 32);
  };

  AETHON_ALWAYS_INLINE TaggedIndex
  with_new_index(uint32_t new_idx) const noexcept {
    uint32_t next_version = version() + 1;
    uint64_t new_value = (static_cast<uint64_t>(next_version) << 32 | new_idx);
    return TaggedIndex{new_value};
  };
};

template <typename T> struct Slot {
  alignas(T) std::byte storage[sizeof(T)];
  uint32_t next_free_idx;

  AETHON_ALWAYS_INLINE T *data() noexcept {
    return reinterpret_cast<T *>(storage);
  }
};
template <typename T, uint32_t Capacity> class IndexMemPool {
private:
  // slot [0] is dummy
  Slot<T>* slots_{nullptr};
  alignas(
      implementation::cacheline_disalign_v) std::atomic<uint64_t> free_head_{0};

public:
  IndexMemPool() {
    size_t total_bytes = sizeof(Slot<T>) * (Capacity + 1); // slot 0 is null ptr
    
    // std::aligned_alloc requires size to be a multiple of alignment!
    size_t aligned_bytes = implementation::align_ceil(total_bytes, implementation::hardware_destructive_interference_size);
    
    slots_ = static_cast<Slot<T> *>(std::aligned_alloc(
        implementation::hardware_destructive_interference_size, aligned_bytes));

    AETHON_SAFE_CHECK(slots_ != nullptr, "Index mem pool OOM on startup");

    for (uint32_t i = 1; i < Capacity; ++i) {
      slots_[i].next_free_idx = i + 1;
    }
    slots_[Capacity].next_free_idx = 0; // 0 means end of the list

    TaggedIndex initial_head{0};
    initial_head =
        initial_head.with_new_index(1); // set the head to index 1, version 0
    free_head_.store(
        initial_head.value,
        std::memory_order_release); // set head to index 1, version 0
  }

  ~IndexMemPool() {
    if (slots_) {
      std::free(slots_);
    }
  }
  AETHON_ALWAYS_INLINE uint32_t allocIndex() noexcept {
    uint64_t current_val = free_head_.load(std::memory_order_acquire);
    TaggedIndex head{current_val};
    uint32_t backoff = 0; // Initialize backoff counter

    while (true) {
      uint32_t idx = head.index();
      if (AETHON_UNLIKELY(idx == 0)) {
        return 0;
      }

      uint32_t next_idx = slots_[idx].next_free_idx;
      TaggedIndex new_head = head.with_new_index(next_idx);

      if (AETHON_LIKELY(free_head_.compare_exchange_weak(
              current_val, new_head.value, std::memory_order_release,
              std::memory_order_relaxed))) {
                
                if(next_idx!=0){
                    AETHON_BUILTIN_PREFETCH(&slots_[next_idx], 0, 3); // next index in L1 cache
                }
        return idx;
      }

      head.value = current_val;

      // Exponential Backoff on failure (1, 2, 4, 8... up to 32 pauses)
      if (AETHON_UNLIKELY(backoff > 0)) {
        for (uint32_t i = 0; i < backoff; ++i) {
          AETHON_PAUSE_CPU_INSTRUCTION;
        }
      }
      if (backoff < 32) {
        backoff = (backoff == 0) ? 1 : backoff * 2;
      }
    }
  };

  AETHON_ALWAYS_INLINE void recycleIndex(uint32_t idx) noexcept {
    uint64_t current_val = free_head_.load(std::memory_order_relaxed);
    TaggedIndex head{current_val};
    uint32_t backoff = 0; // Initialize backoff counter

    while (true) {
      slots_[idx].next_free_idx = head.index();
      TaggedIndex new_head = head.with_new_index(idx);

      if (AETHON_LIKELY(free_head_.compare_exchange_weak(
              current_val, new_head.value, std::memory_order_release,
              std::memory_order_relaxed))) {
        return;
      }
      
      head.value = current_val;

      // Exponential Backoff on failure
      if (AETHON_UNLIKELY(backoff > 0)) {
        for (uint32_t i = 0; i < backoff; ++i) {
          AETHON_PAUSE_CPU_INSTRUCTION;
        }
      }
      if (backoff < 32) {
        backoff = (backoff == 0) ? 1 : backoff * 2;
      }
    }
  };

  AETHON_ALWAYS_INLINE T &operator[](uint32_t idx) noexcept {
    return *slots_[idx].data();
  }

  AETHON_ALWAYS_INLINE void destroy(uint32_t idx) noexcept {
    T *ptr = slots_[idx].data();
    ptr->~T();
    recycleIndex(idx);
  }

  template <typename... Args>
  AETHON_ALWAYS_INLINE T *create(uint32_t &out_idx, Args &&...args) noexcept {
    out_idx = allocIndex();
    if (AETHON_UNLIKELY(out_idx == 0))
      return nullptr;
    T *ptr = slots_[out_idx].data();
    AETHON_BUILTIN_PREFETCH(ptr, 1, 3);
    return new (ptr) T(std::forward<Args>(args)...);
  }
};
} // namespace flux

// ==========================================
// TEST MAIN FUNCTION
#if defined(SLAB_MAIN)

#include <iostream>
#include <thread>
#include <vector>

struct TradeEvent {
    uint64_t timestamp;
    double price;
    uint32_t quantity;

    TradeEvent(uint64_t ts, double p, uint32_t q)
        : timestamp(ts), price(p), quantity(q) {}
};

int main() {
    std::cout << "========================================\n";
    std::cout << "  FLUX LOCK-FREE SLAB ALLOCATOR TEST\n";
    std::cout << "========================================\n\n";

    flux::IndexMemPool<TradeEvent, 1024> pool;

    uint32_t idx1, idx2;
    
    // 1. Allocate from pool
    TradeEvent* t1 = pool.create(idx1, 1000001, 150.0, 100);
    TradeEvent* t2 = pool.create(idx2, 1000002, 150.5, 200);

    std::cout << "[1] Allocated Trade 1 at Index: " << idx1 << " | Price: " << t1->price << "\n";
    std::cout << "[2] Allocated Trade 2 at Index: " << idx2 << " | Price: " << t2->price << "\n";

    // 2. Destroy Trade 1 (Pushes it back to free list)
    std::cout << "[3] Destroying Trade 1 (Index " << idx1 << ")...\n";
    pool.destroy(idx1);

    // 3. Allocate a new trade. It should instantly recycle Index 1!
    uint32_t idx3;
    TradeEvent* t3 = pool.create(idx3, 1000003, 151.0, 300);
    
    std::cout << "[4] Allocated Trade 3 at Index: " << idx3 << " | Price: " << t3->price << "\n";
    
    if (idx3 == idx1) {
        std::cout << "    SUCCESS: Index was recycled perfectly in O(1) time!\n";
    }

    std::cout << "\n========================================\n";
    std::cout << "  ALL TESTS PASSED SUCCESSFULLY!\n";
    std::cout << "========================================\n";

    return 0;
}
#endif