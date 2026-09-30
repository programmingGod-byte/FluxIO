#pragma once
#include <cstdint>
#include <vector>
#include <algorithm>
#include <numeric>
#include <chrono>
#include <thread>
#include <cmath>

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#endif

namespace flux {

[[gnu::always_inline]] static inline uint64_t rdtsc() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return __builtin_ia32_rdtsc();
#else
    return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

[[gnu::always_inline]] static inline uint64_t rdtscp() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    unsigned int aux;
    return __builtin_ia32_rdtscp(&aux);
#else
    return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

inline double estimate_tsc_ghz() {
    uint64_t start_tsc = rdtsc();
    auto start_time = std::chrono::high_resolution_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    uint64_t end_tsc = rdtsc();
    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::nano> elapsed = end_time - start_time;
    return static_cast<double>(end_tsc - start_tsc) / elapsed.count();
}

inline double get_tsc_freq_ghz() {
    static const double freq = estimate_tsc_ghz();
    return freq;
}

[[gnu::always_inline]] static inline double tsc_to_ns(uint64_t cycles) noexcept {
    double freq = get_tsc_freq_ghz();
    if (freq <= 0.0) freq = 1.0;
    return static_cast<double>(cycles) / freq;
}

struct LatencyStats {
    size_t count{0};
    double min_ns{0.0};
    double max_ns{0.0};
    double mean_ns{0.0};
    double p50_ns{0.0};
    double p90_ns{0.0};
    double p99_ns{0.0};
    double p999_ns{0.0};
    double p9999_ns{0.0};
};

class LatencyTracker {
private:
    std::vector<uint64_t> samples_;

public:
    explicit LatencyTracker(size_t reserve_count = 1000000) {
        samples_.reserve(reserve_count);
    }

    [[gnu::always_inline]] void record(uint64_t cycles) noexcept {
        samples_.push_back(cycles);
    }

    [[gnu::always_inline]] void record_start_end(uint64_t start, uint64_t end) noexcept {
        if (end >= start) {
            samples_.push_back(end - start);
        }
    }

    void reset() noexcept {
        samples_.clear();
    }

    size_t size() const noexcept {
        return samples_.size();
    }

    LatencyStats compute() {
        if (samples_.empty()) {
            return {};
        }

        std::sort(samples_.begin(), samples_.end());
        double ghz = get_tsc_freq_ghz();
        if (ghz <= 0.0) ghz = 1.0;

        LatencyStats stats;
        stats.count = samples_.size();
        stats.min_ns = static_cast<double>(samples_.front()) / ghz;
        stats.max_ns = static_cast<double>(samples_.back()) / ghz;

        uint64_t sum = std::accumulate(samples_.begin(), samples_.end(), 0ULL);
        stats.mean_ns = (static_cast<double>(sum) / stats.count) / ghz;

        auto get_percentile = [&](double pct) -> double {
            size_t idx = static_cast<size_t>(std::ceil(pct * stats.count)) - 1;
            if (idx >= stats.count) idx = stats.count - 1;
            return static_cast<double>(samples_[idx]) / ghz;
        };

        stats.p50_ns   = get_percentile(0.50);
        stats.p90_ns   = get_percentile(0.90);
        stats.p99_ns   = get_percentile(0.99);
        stats.p999_ns  = get_percentile(0.999);
        stats.p9999_ns = get_percentile(0.9999);

        return stats;
    }
};

}
