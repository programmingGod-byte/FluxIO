#include "fluxio/flux_io.h"
#include <iostream>
#include <vector>
#include <cstring>
#include <unistd.h>

int main() {
    std::cout << "========================================================\n";
    std::cout << "       FLUXIO EXAMPLE: io_uring STORAGE BACKEND         \n";
    std::cout << "========================================================\n\n";

    const char* filename = "io_uring_demo.dat";
    const uint32_t block_size = 4096;

    // 1. Initialize io_uring backend (1 file, 32 pages in flight, 4096 bytes per page)
    flux::io_uring_storage_engine<1, 32, 4096> engine;

    if (!engine.register_file(0, filename)) {
        std::cerr << "Failed to register file: " << filename << "\n";
        return 1;
    }
    if (!engine.setup_io_uring()) {
        std::cerr << "Failed to setup io_uring backend (check kernel support/permissions)\n";
        return 1;
    }
    std::cout << "[1] File registered and io_uring ring initialized.\n";

    // 2. WRITE OPERATION
    std::cout << "[2] Submitting Write of 4KB at offset 0...\n";
    std::vector<char> write_buf(block_size, 'A');
    write_buf[0] = 'H';
    write_buf[1] = 'I';

    flux::IORequest write_req{};
    write_req.op_type = flux::Type::Write;
    write_req.file_slot = 0;
    write_req.length = block_size;
    write_req.file_offset = 0;
    write_req.user_data = 1001;
    write_req.data_src = write_buf.data();

    engine.push_request(std::move(write_req));
    engine.process_submissions();

    // Wait for write completion
    bool write_done = false;
    while (!write_done) {
        engine.poll_completion();
        flux::IoCompletion comp{};
        while (engine.try_pop_completion(comp)) {
            if (comp.user_data == 1001) {
                std::cout << "    Write completed: " << comp.result << " bytes written.\n";
                write_done = true;
                break;
            }
        }
    }

    // 3. FSYNC OPERATION
    std::cout << "[3] Submitting Fsync to commit blocks to physical NVMe...\n";
    flux::IORequest sync_req{};
    sync_req.op_type = flux::Type::Fsync;
    sync_req.file_slot = 0;
    sync_req.user_data = 2002;

    engine.push_request(std::move(sync_req));
    engine.process_submissions();

    bool sync_done = false;
    while (!sync_done) {
        engine.poll_completion();
        flux::IoCompletion comp{};
        while (engine.try_pop_completion(comp)) {
            if (comp.user_data == 2002) {
                std::cout << "    Fsync completed successfully.\n";
                sync_done = true;
                break;
            }
        }
    }

    // 4. READ OPERATION & VERIFICATION
    std::cout << "[4] Reading back 4KB at offset 0...\n";
    std::vector<char> read_buf(block_size, 0);

    flux::IORequest read_req{};
    read_req.op_type = flux::Type::Read;
    read_req.file_slot = 0;
    read_req.length = block_size;
    read_req.file_offset = 0;
    read_req.user_data = 3003;
    read_req.data_src = read_buf.data();

    engine.push_request(std::move(read_req));
    engine.process_submissions();

    bool read_done = false;
    while (!read_done) {
        engine.poll_completion();
        flux::IoCompletion comp{};
        while (engine.try_pop_completion(comp)) {
            if (comp.user_data == 3003) {
                std::cout << "    Read completed: " << comp.result << " bytes read.\n";
                read_done = true;
                break;
            }
        }
    }

    // Verify content
    if (read_buf[0] == 'H' && read_buf[1] == 'I' && read_buf[2] == 'A') {
        std::cout << "    [SUCCESS] Data verified! First bytes: '" << read_buf[0] << read_buf[1] << read_buf[2] << "'\n";
    } else {
        std::cerr << "    [ERROR] Data verification failed!\n";
    }

    // Clean up
    unlink(filename);
    std::cout << "\nio_uring example finished successfully.\n";
    return 0;
}
