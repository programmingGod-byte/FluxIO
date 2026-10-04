#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <iomanip>
#include <filesystem>
#include <cstdlib>
#include <cmath>
#include <fcntl.h>
#include <unistd.h>
#include <sys/resource.h>
#include "fluxio/util/rdtsc.h"
#include "fluxio/util/cpu_affinity.h"
#include "fluxio/flux.h"

static uint64_t g_rng = 0x853c49e6748fea9bULL;

static inline uint64_t fast_rand() noexcept {
    uint64_t x = g_rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return g_rng = x;
}

// Helper to render readable byte sizes (1 B, 2 B ... 1 KB ... 16 MB)
static std::string format_bytes(size_t bytes) {
    if (bytes < 1024) {
        return std::to_string(bytes) + " B";
    } else if (bytes < 1024 * 1024) {
        return std::to_string(bytes / 1024) + " KB";
    } else {
        return std::to_string(bytes / (1024 * 1024)) + " MB";
    }
}

template <typename EngineType>
static void run_exponential_case(
    EngineType& engine,
    const std::string& name,
    flux::Type op_type,
    bool is_random,
    uint32_t qd,
    uint32_t total_ops,
    size_t file_size,
    size_t block_size,
    const char* buf
) {
    uint32_t warmup_ops = std::min(total_ops / 10, 2000U);
    uint32_t warmup_sub = 0;
    uint32_t warmup_comp = 0;
    flux::IoCompletion comp{};

    uint64_t max_blocks = (file_size > block_size) ? ((file_size - block_size) / block_size) : 1;
    if (max_blocks == 0) max_blocks = 1;

    // Warmup phase
    while (warmup_comp < warmup_ops) {
        while (warmup_sub < warmup_ops && (warmup_sub - warmup_comp) < qd) {
            uint64_t offset = is_random
                ? ((fast_rand() % max_blocks) * block_size)
                : ((static_cast<uint64_t>(warmup_sub) * block_size) % (file_size - block_size));
            
            flux::IORequest req{};
            req.op_type = op_type;
            req.file_slot = 0;
            req.length = block_size;
            req.file_offset = offset;
            req.user_data = warmup_sub;
            req.data_src = buf;
            req.timestamp_ns = flux::rdtsc();

            if (!engine.try_push_request(std::move(req))) {
                break;
            }
            ++warmup_sub;
        }
        engine.process_submissions(qd);
        engine.poll_completion();
        bool got = false;
        while (engine.try_pop_completion(comp)) {
            ++warmup_comp;
            got = true;
        }
        if (!got) {
            engine.submit();
            sched_yield();
        }
    }

    // Benchmark phase
    flux::LatencyTracker tracker(total_ops);
    struct rusage r_start, r_end;
    getrusage(RUSAGE_SELF, &r_start);
    auto t_start = std::chrono::high_resolution_clock::now();

    uint32_t submitted = 0;
    uint32_t completed = 0;

    while (completed < total_ops) {
        while (submitted < total_ops && (submitted - completed) < qd) {
            uint64_t offset = is_random
                ? ((fast_rand() % max_blocks) * block_size)
                : ((static_cast<uint64_t>(submitted) * block_size) % (file_size - block_size));

            flux::IORequest req{};
            req.op_type = op_type;
            req.file_slot = 0;
            req.length = block_size;
            req.file_offset = offset;
            req.user_data = submitted;
            req.data_src = buf;
            req.timestamp_ns = flux::rdtsc();

            if (!engine.try_push_request(std::move(req))) {
                break;
            }
            ++submitted;
        }
        engine.process_submissions(qd);
        engine.poll_completion();
        bool got = false;
        while (engine.try_pop_completion(comp)) {
            tracker.record_start_end(comp.submit_ts_ns, comp.complete_ts_ns);
            ++completed;
            got = true;
        }
        if (!got) {
            engine.submit();
            sched_yield();
        }
    }

    auto t_end = std::chrono::high_resolution_clock::now();
    getrusage(RUSAGE_SELF, &r_end);

    double wall_sec = std::chrono::duration<double>(t_end - t_start).count();
    double user_sec = (r_end.ru_utime.tv_sec - r_start.ru_utime.tv_sec) +
                      (r_end.ru_utime.tv_usec - r_start.ru_utime.tv_usec) * 1e-6;
    double sys_sec  = (r_end.ru_stime.tv_sec - r_start.ru_stime.tv_sec) +
                      (r_end.ru_stime.tv_usec - r_start.ru_stime.tv_usec) * 1e-6;
    double cpu_pct = (wall_sec > 0.0) ? (((user_sec + sys_sec) / wall_sec) * 100.0) : 0.0;

    auto stats = tracker.compute();
    double iops = static_cast<double>(total_ops) / wall_sec;
    double mb_s = (iops * block_size) / (1024.0 * 1024.0);

    std::cout << std::left << std::setw(16) << name
              << std::right << std::setw(12) << format_bytes(block_size)
              << std::setw(5)  << qd
              << std::setw(12) << std::fixed << std::setprecision(1) << iops
              << std::setw(11) << std::fixed << std::setprecision(2) << mb_s
              << std::setw(10) << std::fixed << std::setprecision(1) << (stats.min_ns / 1000.0)
              << std::setw(10) << std::fixed << std::setprecision(1) << (stats.p50_ns / 1000.0)
              << std::setw(10) << std::fixed << std::setprecision(1) << (stats.p90_ns / 1000.0)
              << std::setw(10) << std::fixed << std::setprecision(1) << (stats.p99_ns / 1000.0)
              << std::setw(10) << std::fixed << std::setprecision(1) << (stats.max_ns / 1000.0)
              << std::setw(8)  << std::fixed << std::setprecision(1) << cpu_pct
              << "\n";
}

void run_exponential_suite(const std::string& filename, size_t max_block_size, uint32_t qd) {
    const size_t file_size = 512 * 1024 * 1024; // 512MB test file

    int fd = ::open(filename.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        std::cerr << "Failed to create file: " << filename << "\n";
        return;
    }

    if (::posix_fallocate(fd, 0, file_size) != 0) {
        ::ftruncate(fd, file_size);
    }
    ::close(fd);

    flux::unpin_current_thread();

    // Engine instantiated with buffer size supporting max_block_size
    flux::io_uring_storage_engine<4, 128, 16 * 1024 * 1024> engine;
    if (!engine.register_file(0, filename.c_str())) {
        std::cerr << "Failed to register file: " << filename << "\n";
        std::filesystem::remove(filename);
        return;
    }
    if (!engine.setup_io_uring()) {
        std::cerr << "Failed to setup io_uring for: " << filename << "\n";
        std::filesystem::remove(filename);
        return;
    }

    flux::pin_current_thread_to_core(2);

    // Aligned allocation for direct I/O compatibility
    char* io_buf = nullptr;
    if (posix_memalign(reinterpret_cast<void**>(&io_buf), 4096, max_block_size) != 0) {
        std::cerr << "Failed to allocate aligned buffer\n";
        return;
    }
    std::fill_n(io_buf, max_block_size, 'A');

    std::cout << "\n" << std::string(120, '=') << "\n";
    std::cout << "  EXPONENTIAL SIZE BENCHMARK (1 Byte to " << format_bytes(max_block_size) << ", Queue Depth: " << qd << ")\n";
    std::cout << std::string(120, '=') << "\n";
    std::cout << std::left << std::setw(16) << "Pattern"
              << std::right << std::setw(12) << "Block Size"
              << std::setw(5)  << "QD"
              << std::setw(12) << "IOPS"
              << std::setw(11) << "MB/s"
              << std::setw(10) << "Min(us)"
              << std::setw(10) << "p50(us)"
              << std::setw(10) << "p90(us)"
              << std::setw(10) << "p99(us)"
              << std::setw(10) << "Max(us)"
              << std::setw(8)  << "CPU%"
              << "\n";
    std::cout << std::string(120, '-') << "\n";

    // Exponential scaling loop: 1B, 2B, 4B, 8B, ..., Max Block Size
    for (size_t block_size = 1; block_size <= max_block_size; block_size *= 2) {
        // Adjust total ops down for massive block sizes to keep run times practical
        uint32_t ops = (block_size >= 4 * 1024 * 1024) ? 2000 : 20000;

        run_exponential_case(
            engine,
            "Seq Write",
            flux::Type::Write,
            false,
            qd,
            ops,
            file_size,
            block_size,
            io_buf
        );
    }

    std::cout << std::string(120, '=') << "\n\n";

    free(io_buf);
    std::filesystem::remove(filename);
    flux::unpin_current_thread();
}

int main() {
    // Runs from 1 Byte (2^0) up to 16 MB (2^24) with Queue Depth = 1
    run_exponential_suite("bench_exponential.dat", 16 * 1024 * 1024, 1);
    return 0;
}
