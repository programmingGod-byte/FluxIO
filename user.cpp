#include "fluxio/flux_io.h"
#include <iostream>
#include <vector>
#include <chrono>
#include <cstring>

int main() {
    std::cout << "====================================================\n";
    std::cout << "         FLUXIO: 1GB FILE I/O DEMO (USER APP)       \n";
    std::cout << "====================================================\n";

    const char* filename = "user_demo_1gb.dat";
    const size_t total_size = 1024ULL * 1024 * 1024; // 1 GB
    const uint32_t block_size = 4096;                // 4 KB
    const uint64_t total_blocks = total_size / block_size;
    const uint32_t qd = 32;                          // Queue depth

    // 1. Initialize FluxIO storage engine (1 file, 128 in-flight pages, 4096 page size)
    flux::io_uring_storage_engine<1, 128, 4096> engine;

    // 2. Register file (creates or opens with O_DIRECT)
    if (!engine.register_file(0, filename)) {
        std::cerr << "Failed to register file: " << filename << "\n";
        return 1;
    }

    if (!engine.setup_io_uring()) {
        std::cerr << "Failed to setup io_uring backend\n";
        return 1;
    }

    std::cout << "[1] Successfully registered " << filename << " and initialized io_uring.\n";

    // 3. Prepare 4KB write buffer filled with sample pattern 'F'
    std::vector<char> write_buf(block_size, 'F');
    write_buf[0] = 'S'; // Start marker
    write_buf[block_size - 1] = 'E'; // End marker

    // 4. Write 1GB pipelined at Queue Depth 32
    std::cout << "[2] Writing 1 GB (" << total_blocks << " blocks of 4KB) with QD=" << qd << "...\n";
    auto start_write = std::chrono::high_resolution_clock::now();

    uint64_t submitted = 0;
    uint64_t completed = 0;

    while (completed < total_blocks) {
        while (submitted < total_blocks && (submitted - completed) < qd) {
            flux::IORequest req{};
            req.op_type = flux::Type::Write;
            req.file_slot = 0;
            req.length = block_size;
            req.file_offset = submitted * block_size;
            req.user_data = submitted;
            req.data_src = write_buf.data();

            engine.push_request(std::move(req));
            ++submitted;
        }

        engine.process_submissions();
        engine.poll_completion();

        flux::IoCompletion comp{};
        while (engine.try_pop_completion(comp)) {
            if (comp.failed) {
                std::cerr << "Write failed at block " << comp.user_data << "\n";
            }
            ++completed;
        }
    }

    auto end_write = std::chrono::high_resolution_clock::now();
    double write_sec = std::chrono::duration<double>(end_write - start_write).count();
    double write_mb_s = (total_size / (1024.0 * 1024.0)) / write_sec;
    std::cout << "    Done! Wrote 1 GB in " << write_sec << " s (" << write_mb_s << " MiB/s, "
              << (total_blocks / write_sec) << " IOPS)\n";

    // 5. Issue FSYNC to guarantee physical persistence on NVMe
    std::cout << "[3] Issuing FSYNC (fdatasync) to flush SSD buffers...\n";
    auto start_sync = std::chrono::high_resolution_clock::now();

    flux::IORequest sync_req{};
    sync_req.op_type = flux::Type::Fsync;
    sync_req.file_slot = 0;
    sync_req.user_data = 9999;
    engine.push_request(std::move(sync_req));
    engine.process_submissions();

    bool synced = false;
    while (!synced) {
        engine.poll_completion();
        flux::IoCompletion comp{};
        while (engine.try_pop_completion(comp)) {
            if (comp.op_type == flux::Type::Fsync) {
                synced = true;
                break;
            }
        }
    }
    auto end_sync = std::chrono::high_resolution_clock::now();
    double sync_ms = std::chrono::duration<double, std::milli>(end_sync - start_sync).count();
    std::cout << "    FSYNC completed in " << sync_ms << " ms.\n";

    // 6. Read back block #0 and verify content
    std::cout << "[4] Reading back block #0 to verify data...\n";
    std::vector<char> read_buf(block_size, 0);

    flux::IORequest read_req{};
    read_req.op_type = flux::Type::Read;
    read_req.file_slot = 0;
    read_req.length = block_size;
    read_req.file_offset = 0;
    read_req.user_data = 101;
    read_req.data_src = read_buf.data(); // destination buffer
    engine.push_request(std::move(read_req));
    engine.process_submissions();

    bool read_done = false;
    while (!read_done) {
        engine.poll_completion();
        flux::IoCompletion comp{};
        while (engine.try_pop_completion(comp)) {
            if (comp.op_type == flux::Type::Read) {
                read_done = true;
                break;
            }
        }
    }

    if (read_buf[0] == 'S' && read_buf[block_size - 1] == 'E') {
        std::cout << "    SUCCESS: Read verification matched original written bytes ('S'...'E')!\n";
    } else {
        std::cerr << "    ERROR: Data verification failed!\n";
    }

    std::cout << "====================================================\n";
    std::cout << "Demo complete. Cleaning up test file.\n";
    unlink(filename);
    return 0;
}
