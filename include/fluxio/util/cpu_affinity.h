#pragma once
#include <pthread.h>
#include <sched.h>
#include <cstdint>

namespace flux {

inline bool pin_current_thread_to_core(uint32_t core_id) noexcept {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_t current_thread = pthread_self();
    return pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset) == 0;
}

inline bool pin_thread_to_core(pthread_t thread, uint32_t core_id) noexcept {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    return pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset) == 0;
}

inline int get_current_core() noexcept {
    return sched_getcpu();
}

inline bool unpin_current_thread() noexcept {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (int i = 0; i < CPU_SETSIZE; ++i) {
        CPU_SET(i, &cpuset);
    }
    pthread_t current_thread = pthread_self();
    return pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset) == 0;
}

}
