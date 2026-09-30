#include "fluxio/flux_io.h"
#include <iostream>
#include <vector>

int main() {
    const char* filename = "posix_sample.dat";
    const uint32_t block_size = 4096;

    // 1. Initialize POSIX storage engine
    flux::posix_storage_engine<1, 32, 4096> engine;

    if (!engine.register_file(0, filename)) {
        std::cerr << "Failed to register file: " << filename << "\n";
        return 1;
    }

    // 2. Prepare 4KB read destination buffer
    std::vector<char> read_buf(block_size, 0);

    // 3. Submit Read request
    flux::IORequest req{};
    req.op_type     = flux::Type::Read;
    req.file_slot   = 0;
    req.length      = block_size;
    req.file_offset = 0;
    req.user_data   = 502;
    req.data_src    = read_buf.data();

    engine.push_request(std::move(req));
    engine.process_submissions(); // Executes pread synchronously with aligned buffers

    // 4. Pop completion
    flux::IoCompletion comp{};
    while (engine.try_pop_completion(comp)) {
        if (comp.user_data == 502) {
            if (!comp.failed) {
                std::cout << "[POSIX READ] Successfully read " << comp.result << " bytes from " << filename << "\n";
                std::cout << "[POSIX READ] First 5 characters read: '"
                          << read_buf[0] << read_buf[1] << read_buf[2] << read_buf[3] << read_buf[4] << "'\n";
            } else {
                std::cerr << "[POSIX READ] Read operation failed!\n";
            }
            break;
        }
    }

    return 0;
}
