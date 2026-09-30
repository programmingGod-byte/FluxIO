#pragma  once
#include "../util/aethon.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace flux {

template<typename T, std::size_t capacity = 16>
class flux_ring_buffer {
    static_assert((capacity & (capacity - 1)) == 0 && capacity > 0, "Capacity must be a power of 2!");
    static constexpr std::size_t MASK = capacity - 1;

private:
    alignas(implementation::hardware_destructive_interference_size) std::atomic<uint32_t> head_{0};
    
    alignas(implementation::hardware_destructive_interference_size) std::atomic<uint32_t> tail_{0};

    alignas(implementation::hardware_destructive_interference_size) std::array<T, capacity> data_;

public:
    flux_ring_buffer() = default;

    AETHON_ALWAYS_INLINE bool isEmpty() const noexcept {
        uint32_t head = head_.load(std::memory_order_relaxed);
        uint32_t tail = tail_.load(std::memory_order_acquire);
        return head == tail;
    }

    AETHON_ALWAYS_INLINE bool isFull() const noexcept {
        uint32_t head = head_.load(std::memory_order_acquire);
        uint32_t next_tail = (tail_.load(std::memory_order_relaxed) + 1) & MASK;
        return head == next_tail;
    }

    // Pop element (Consumer)
    AETHON_ALWAYS_INLINE bool pop(T &val) noexcept {
        uint32_t head = head_.load(std::memory_order_relaxed);
        uint32_t tail = tail_.load(std::memory_order_acquire);
        
        if (AETHON_UNLIKELY(head == tail)) {
            return false; // Empty
        }

        uint32_t next_head = (head + 1) & MASK;
        AETHON_BUILTIN_PREFETCH(&data_[next_head], 0, 3);

        val = std::move(data_[head]);
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    AETHON_ALWAYS_INLINE T* peek() noexcept {
        uint32_t head = head_.load(std::memory_order_relaxed);
        uint32_t tail = tail_.load(std::memory_order_acquire);
        if (AETHON_UNLIKELY(head == tail)) {
            return nullptr;
        }
        return &data_[head];
    }

    AETHON_ALWAYS_INLINE const T* peek() const noexcept {
        uint32_t head = head_.load(std::memory_order_relaxed);
        uint32_t tail = tail_.load(std::memory_order_acquire);
        if (AETHON_UNLIKELY(head == tail)) {
            return nullptr;
        }
        return &data_[head];
    }

    AETHON_ALWAYS_INLINE void consume() noexcept {
        uint32_t head = head_.load(std::memory_order_relaxed);
        uint32_t next_head = (head + 1) & MASK;
        head_.store(next_head, std::memory_order_release);
    }

    template<typename ...Args>
    AETHON_ALWAYS_INLINE bool push(Args&&... args) noexcept {
        uint32_t tail = tail_.load(std::memory_order_relaxed);
        uint32_t head = head_.load(std::memory_order_acquire);
        uint32_t next_tail = (tail + 1) & MASK;

        if (AETHON_UNLIKELY(next_tail == head)) {
            return false; // Full
        }

        AETHON_BUILTIN_PREFETCH(&data_[tail], 1, 3);
        data_[tail] = T(std::forward<Args>(args)...);
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    ~flux_ring_buffer() {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            uint32_t head = head_.load(std::memory_order_relaxed);
            uint32_t tail = tail_.load(std::memory_order_relaxed);

            while (head != tail) {
                AETHON_BUILTIN_PREFETCH(&data_[head], 1, 3);                    
                data_[head].~T();
                head = (head + 1) & MASK;
            }
        }
    }

    flux_ring_buffer(const flux_ring_buffer&) = delete;
    flux_ring_buffer(flux_ring_buffer&&) = delete;

    flux_ring_buffer& operator=(const flux_ring_buffer&) = delete;
    flux_ring_buffer& operator=(flux_ring_buffer&&) = delete;
};

} // namespace flux
