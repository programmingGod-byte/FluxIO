#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <iomanip>
#include <filesystem>
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

static void run_case(
    flux::io_uring_storage_engine<4, 128, 4096>& engine,
    const std::string& name,
    flux::Type op_type,
    bool is_random,
    uint32_t qd,
    uint32_t total_ops,
    size_t file_size,
    uint32_t block_size,
    const char* buf
) {
    uint32_t warmup_ops = std::min(total_ops / 10, 3000U);
    uint32_t warmup_sub = 0;
    uint32_t warmup_comp = 0;
    flux::IoCompletion comp{};

    uint64_t max_blocks = (file_size - block_size) / block_size;

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

    std::cout << std::left << std::setw(18) << name
              << std::right << std::setw(5) << qd
              << std::setw(12) << std::fixed << std::setprecision(1) << iops
              << std::setw(11) << std::fixed << std::setprecision(2) << mb_s
              << std::setw(10) << std::fixed << std::setprecision(1) << (stats.min_ns / 1000.0)
              << std::setw(10) << std::fixed << std::setprecision(1) << (stats.p50_ns / 1000.0)
              << std::setw(10) << std::fixed << std::setprecision(1) << (stats.p90_ns / 1000.0)
              << std::setw(10) << std::fixed << std::setprecision(1) << (stats.p99_ns / 1000.0)
              << std::setw(10) << std::fixed << std::setprecision(1) << (stats.p999_ns / 1000.0)
              << std::setw(10) << std::fixed << std::setprecision(1) << (stats.max_ns / 1000.0)
              << std::setw(8)  << std::fixed << std::setprecision(1) << cpu_pct
              << "\n";
}

static void run_suite(const std::string& suite_title, const std::string& filename, bool continuous_prealloc) {
    const size_t file_size = 256 * 1024 * 1024;
    const uint32_t block_size = 4096;

    int fd = ::open(filename.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        std::cerr << "Failed to create file: " << filename << "\n";
        return;
    }

    if (continuous_prealloc) {
        if (::posix_fallocate(fd, 0, file_size) != 0) {
            ::ftruncate(fd, file_size);
        }
        std::vector<char> pre_buf(1024 * 1024, 'C');
        for (size_t written = 0; written < file_size; written += pre_buf.size()) {
            ssize_t w = ::write(fd, pre_buf.data(), pre_buf.size());
            if (w <= 0) break;
        }
        ::fdatasync(fd);
        ::posix_fadvise(fd, 0, file_size, POSIX_FADV_DONTNEED);
    } else {
        if (::ftruncate(fd, file_size) != 0) {
            ::close(fd);
            std::cerr << "Failed to truncate file: " << filename << "\n";
            return;
        }
        ::posix_fadvise(fd, 0, file_size, POSIX_FADV_DONTNEED);
    }
    ::close(fd);

    flux::unpin_current_thread();

    flux::io_uring_storage_engine<4, 128, 4096> engine;
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

    std::vector<char> io_buf(block_size, 'X');

    std::cout << "\n" << std::string(123, '=') << "\n";
    std::cout << "  " << suite_title << "\n";
    std::cout << std::string(123, '=') << "\n";
    std::cout << std::left << std::setw(18) << "Pattern"
              << std::right << std::setw(5) << "QD"
              << std::setw(12) << "IOPS"
              << std::setw(11) << "MB/s"
              << std::setw(10) << "Min(us)"
              << std::setw(10) << "p50(us)"
              << std::setw(10) << "p90(us)"
              << std::setw(10) << "p99(us)"
              << std::setw(10) << "p99.9(us)"
              << std::setw(10) << "Max(us)"
              << std::setw(8)  << "CPU%"
              << "\n";
    std::cout << std::string(123, '-') << "\n";

    const std::vector<uint32_t> qds = {1, 4, 16, 32, 64, 128};

    for (uint32_t qd : qds) {
        uint32_t ops = (qd == 1) ? 20000 : 50000;
        run_case(engine, "4KB Seq Write", flux::Type::Write, false, qd, ops, file_size, block_size, io_buf.data());
    }
    std::cout << std::string(123, '-') << "\n";

    for (uint32_t qd : qds) {
        uint32_t ops = (qd == 1) ? 20000 : 50000;
        run_case(engine, "4KB Rand Write", flux::Type::Write, true, qd, ops, file_size, block_size, io_buf.data());
    }
    std::cout << std::string(123, '-') << "\n";

    for (uint32_t qd : qds) {
        uint32_t ops = (qd == 1) ? 20000 : 50000;

        run_case(engine, "4KB Seq Read", flux::Type::Read, false, qd, ops, file_size, block_size, io_buf.data());
    }
    std::cout << std::string(123, '-') << "\n";

    for (uint32_t qd : qds) {
        uint32_t ops = (qd == 1) ? 20000 : 50000;

        run_case(engine, "4KB Rand Read", flux::Type::Read, true, qd, ops, file_size, block_size, io_buf.data());
    }
    std::cout << std::string(123, '=') << "\n\n";

    std::filesystem::remove(filename);
    flux::unpin_current_thread();
}

int main() {
    run_suite("MODE 1: 256MB CONTINUOUS PREALLOCATION (posix_fallocate + Direct I/O)", "bench_fluxio_continuous.dat", true);
    run_suite("MODE 2: 256MB RANDOM / DYNAMIC ON-THE-FLY ALLOCATION (Sparse / Direct I/O)", "bench_fluxio_random_alloc.dat", false);
    return 0;
}
