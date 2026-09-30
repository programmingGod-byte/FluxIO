#include "fluxio/flux_io.h"
#include <iostream>
#include <vector>

int main() {
    const char* filename = "io_uring_sample.dat";
    const uint32_t block_size = 4096;

    // 1. Initialize io_uring backend (1 file, 32 in-flight entries, 4096 bytes per page)
    flux::io_uring_storage_engine<1, 32, 4096> engine;

    if (!engine.register_file(0, filename)) {
        std::cerr << "Failed to register file: " << filename << "\n";
        return 1;
    }
    if (!engine.setup_io_uring()) {
        std::cerr << "Failed to setup io_uring ring\n";
        return 1;
    }

    // 2. Prepare 4KB buffer
    std::vector<char> buffer(block_size, 'W');
    buffer[0] = 'H';
    buffer[1] = 'E';
    buffer[2] = 'L';
    buffer[3] = 'L';
    buffer[4] = 'O';

    // 3. Submit async Write request
    flux::IORequest req{};
    req.op_type     = flux::Type::Write;
    req.file_slot   = 0;
    req.length      = block_size;
    req.file_offset = 0;
    req.user_data   = 101;
    req.data_src    = buffer.data();

    engine.push_request(std::move(req));
    engine.process_submissions(); // Flushes SQ to kernel polling thread

    // 4. Poll completions
    bool done = false;
    while (!done) {
        engine.pool_completion();
        flux::IoCompletion comp{};
        while (engine.try_pop_completion(comp)) {
            if (comp.user_data == 101) {
                if (!comp.failed) {
                    std::cout << "[io_uring WRITE] Successfully wrote " << comp.result << " bytes to " << filename << "\n";
                } else {
                    std::cerr << "[io_uring WRITE] Write operation failed!\n";
                }
                done = true;
                break;
            }
        }
    }

    return 0;
}
