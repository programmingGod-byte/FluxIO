#include "fluxio/flux_io.h"
#include <iostream>
#include <vector>

int main() {
    const char* filename = "io_uring_sample.dat";
    const uint32_t block_size = 4096;

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

    // 2. Prepare 4KB read destination buffer
    std::vector<char> read_buf(block_size, 0);

    // 3. Submit async Read request
    flux::IORequest req{};
    req.op_type     = flux::Type::Read;
    req.file_slot   = 0;
    req.length      = block_size;
    req.file_offset = 0;
    req.user_data   = 202;
    req.data_src    = read_buf.data();

    engine.push_request(std::move(req));
    engine.process_submissions();

    // 4. Poll completions
    bool done = false;
    while (!done) {
        engine.pool_completion();
        flux::IoCompletion comp{};
        while (engine.try_pop_completion(comp)) {
            if (comp.user_data == 202) {
                if (!comp.failed) {
                    std::cout << "[io_uring READ] Successfully read " << comp.result << " bytes from " << filename << "\n";
                    std::cout << "[io_uring READ] First 5 characters read: '"
                              << read_buf[0] << read_buf[1] << read_buf[2] << read_buf[3] << read_buf[4] << "'\n";
                } else {
                    std::cerr << "[io_uring READ] Read operation failed!\n";
                }
                done = true;
                break;
            }
        }
    }

    return 0;
}
