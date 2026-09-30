#pragma once

#include "../ring/sq_ring.h"
#include "../ring/cq_ring.h"
#include <concepts>
#include <cstdint>
#include <cstddef>

namespace flux {

template <typename T>
concept StorageBackend = requires(T backend, size_t slot, const char *path,
                                  IORequest req, uint32_t max_batch) {
  { backend.register_file(slot, path) } -> std::same_as<bool>;
  { backend.push_request(std::move(req)) } -> std::same_as<void>;
  { backend.try_push_request(std::move(req)) } -> std::same_as<bool>;
  { backend.process_submissions(max_batch) } -> std::same_as<int>;
};

} // namespace flux
