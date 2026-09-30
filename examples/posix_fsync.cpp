#include "fluxio/flux_io.h"
#include <iostream>

int main() {
    const char* filename = "posix_sample.dat";

    // 1. Initialize POSIX storage engine
    flux::posix_storage_engine<1, 32, 4096> engine;

    if (!engine.register_file(0, filename)) {
        std::cerr << "Failed to register file: " << filename << "\n";
        return 1;
    }

    // 2. Submit Fsync request
    flux::IORequest req{};
    req.op_type   = flux::Type::Fsync;
    req.file_slot = 0;
    req.user_data = 603;

    engine.push_request(std::move(req));
    engine.process_submissions(); // Executes fdatasync synchronously

    // 3. Pop completion
    flux::IoCompletion comp{};
    while (engine.try_pop_completion(comp)) {
        if (comp.user_data == 603) {
            if (!comp.failed) {
                std::cout << "[POSIX FSYNC] Successfully synced " << filename << " to NVMe NAND storage via fdatasync!\n";
            } else {
                std::cerr << "[POSIX FSYNC] Fsync operation failed!\n";
            }
            break;
        }
    }

    return 0;
}
