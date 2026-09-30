#include "fluxio/flux_io.h"
#include <iostream>

int main() {
    const char* filename = "io_uring_sample.dat";

    // 1. Initialize io_uring backend
    flux::io_uring_storage_engine<1, 32, 4096> engine;

    if (!engine.register_file(0, filename)) {
        std::cerr << "Failed to register file: " << filename << "\n";
        return 1;
    }
    if (!engine.setup_io_uring()) {
        std::cerr << "Failed to setup io_uring ring\n";
        return 1;
    }

    // 2. Submit async Fsync request
    flux::IORequest req{};
    req.op_type   = flux::Type::Fsync;
    req.file_slot = 0;
    req.user_data = 303;

    engine.push_request(std::move(req));
    engine.process_submissions(); // Submits IORING_OP_FSYNC into ring

    // 3. Poll completions
    bool done = false;
    while (!done) {
        engine.poll_completion();
        flux::IoCompletion comp{};
        while (engine.try_pop_completion(comp)) {
            if (comp.user_data == 303) {
                if (!comp.failed) {
                    std::cout << "[io_uring FSYNC] Successfully flushed " << filename << " to NVMe NAND storage!\n";
                } else {
                    std::cerr << "[io_uring FSYNC] Fsync operation failed!\n";
                }
                done = true;
                break;
            }
        }
    }

    return 0;
}
