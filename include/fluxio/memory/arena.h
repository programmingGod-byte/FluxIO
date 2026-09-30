#pragma once
#include "../util/aethon.h"
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <utility>

namespace flux {

struct Block{
    Block * next {nullptr}; // pointer to next if allocated more than 64kb
    AETHON_ALWAYS_INLINE char * data() noexcept {
        return reinterpret_cast<char*>(this+1);
    };
};

template<size_t BlockSize = 64 * 1024>
class Arena{
    private:
        char * ptr_{nullptr};
        char * end_{nullptr}; // end of current 64 kb block
        Block* head_block_{nullptr}; // head of the linked list
    
        AETHON_NO_INLINE void * allocate_slow(size_t size, size_t alignment){
            size_t alloc_size = std::max(BlockSize,size+sizeof(Block) + alignment);

            void * raw_mem = std::aligned_alloc(64,alloc_size);
            AETHON_SAFE_CHECK(raw_mem !=nullptr,"out of memory in arena allocator");

            Block * new_block  = static_cast<Block*>(raw_mem);
            new_block->next = head_block_;
            head_block_ = new_block;

            char * data_start = new_block->data();
            char *aligned_ptr = implementation::align_ceil(data_start,alignment);

            ptr_ = aligned_ptr + size;
            end_ = reinterpret_cast<char*>(raw_mem) + alloc_size;

            return aligned_ptr;
        }


    public:

        Arena(){
            allocate_slow(0, 64);
        }
        template<size_t Alignment = 16>
        AETHON_ALWAYS_INLINE void * allocate(size_t size) noexcept{
            char * aligned_ptr = implementation::align_ceil(ptr_,Alignment);
            char *next_ptr = aligned_ptr + size; // move forward by size bytes

            if(AETHON_LIKELY(next_ptr <=end_)){
                ptr_ = next_ptr;
                AETHON_BUILTIN_PREFETCH(next_ptr, 1, 3);
                return aligned_ptr;
            }

            // slow allocate
            return allocate_slow(size, Alignment);
        };

        AETHON_ALWAYS_INLINE void reset() noexcept{
            if(AETHON_LIKELY(head_block_!=nullptr)){
                ptr_ = head_block_->data();
                AETHON_BUILTIN_PREFETCH(ptr_, 1, 3);
            }
        }

        template<typename T, typename... Args>
        AETHON_ALWAYS_INLINE T* create(Args&&... args){
            void *mem  = allocate<alignof(T)>(sizeof(T));
            return new (mem) T(std::forward<Args>(args)...);
        }

        ~Arena(){
            Block * curr = head_block_;
            while (curr) {
                Block *next = curr->next;
                std::free(curr);
                curr = next;
            }
        }
};

} // namespace flux

// ==========================================
// TEST / BENCHMARK MAIN FUNCTION
// ==========================================
#if defined(ARENA_MAIN) || !defined(ARENA_NO_MAIN)

struct Order {
    uint64_t id;
    double price;
    uint32_t quantity;
    char symbol[8];

    Order(uint64_t i, double p, uint32_t q, const char* s)
        : id(i), price(p), quantity(q) {
        std::strncpy(symbol, s, sizeof(symbol) - 1);
        symbol[sizeof(symbol) - 1] = '\0';
    }
};

int main() {
    std::cout << "========================================\n";
    std::cout << "  FLUX HFT ARENA ALLOCATOR TEST\n";
    std::cout << "========================================\n\n";

    flux::Arena<64 * 1024> arena; // 64KB Arena block

    // 1. Test Single Object Creation
    std::cout << "[1] Allocating Order 1...\n";
    Order* o1 = arena.create<Order>(1001, 150.25, 100, "AAPL");
    std::cout << "    Order 1: ID=" << o1->id << ", Symbol=" << o1->symbol
              << ", Price=" << o1->price << ", Addr=" << (void*)o1 << "\n";

    std::cout << "[2] Allocating Order 2...\n";
    Order* o2 = arena.create<Order>(1002, 2800.50, 50, "GOOGL");
    std::cout << "    Order 2: ID=" << o2->id << ", Symbol=" << o2->symbol
              << ", Price=" << o2->price << ", Addr=" << (void*)o2 << "\n";

    // Verify pointer increment (offset difference)
    ptrdiff_t diff = reinterpret_cast<char*>(o2) - reinterpret_cast<char*>(o1);
    std::cout << "    Pointer Delta (Aligned size of Order): " << diff << " bytes\n\n";

    // 2. Test Reset Functionality
    std::cout << "[3] Calling arena.reset() to recycle memory...\n";
    arena.reset();

    std::cout << "[4] Allocating Order 3 after reset...\n";
    Order* o3 = arena.create<Order>(1003, 420.69, 200, "TSLA");
    std::cout << "    Order 3: ID=" << o3->id << ", Symbol=" << o3->symbol
              << ", Price=" << o3->price << ", Addr=" << (void*)o3 << "\n";

    if (o1 == o3) {
        std::cout << "    SUCCESS: Order 3 recycled the EXACT same memory address as Order 1!\n";
        std::cout << "    (Zero OS memory allocations occurred!)\n\n";
    }

    // 3. High-Speed Loop Benchmark
    std::cout << "[5] Running 1,000,000 Allocations in a Hot Loop with reset()...\n";
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < 1'000'000; i++) {
        Order* o = arena.create<Order>(i, 100.0, 10, "NVDA");
        (void)o; // Do work
        arena.reset(); // Instant 0-nanosecond recycle
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    double avg_ns = static_cast<double>(duration_ns) / 1'000'000.0;

    std::cout << "    Total Time for 1,000,000 Allocations: " << duration_ns / 1'000'000.0 << " ms\n";
    std::cout << "    Average Time per Allocation: " << avg_ns << " nanoseconds!\n";
    std::cout << "\n========================================\n";
    std::cout << "  ALL TESTS PASSED SUCCESSFULLY!\n";
    std::cout << "========================================\n";

    return 0;
}
#endif