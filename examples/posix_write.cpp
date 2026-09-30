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

    // 2. Prepare 4KB buffer
    std::vector<char> buffer(block_size, 'P');
    buffer[0] = 'W';
    buffer[1] = 'O';
    buffer[2] = 'R';
    buffer[3] = 'L';
    buffer[4] = 'D';

    // 3. Submit Write request
    flux::IORequest req{};
    req.op_type     = flux::Type::Write;
    req.file_slot   = 0;
    req.length      = block_size;
    req.file_offset = 0;
    req.user_data   = 401;
    req.data_src    = buffer.data();

    engine.push_request(std::move(req));
    engine.process_submissions(); // Executes pwrite synchronously with aligned buffers

    // 4. Pop completion
    flux::IoCompletion comp{};
    while (engine.try_pop_completion(comp)) {
        if (comp.user_data == 401) {
            if (!comp.failed) {
                std::cout << "[POSIX WRITE] Successfully wrote " << comp.result << " bytes to " << filename << "\n";
            } else {
                std::cerr << "[POSIX WRITE] Write operation failed!\n";
            }
            break;
        }
    }

    return 0;
}
