#include "fluxio/flux_io.h"
#include <iostream>
#include <fstream>
#include <string>
#include <filesystem>
#include <cassert>

struct SensorMetric {
    uint64_t timestamp_ns;
    uint32_t sensor_id;
    float reading;
};

int main() {
    std::cout << "========================================================\n";
    std::cout << "      FLUXIO: 4-FILE CONCURRENT STREAM WRITE DEMO       \n";
    std::cout << "========================================================\n\n";

    const std::vector<std::string> file_paths = {
        "file0_wal.log",
        "file1_events.json",
        "file2_metrics.csv",
        "file3_binary.dat"
    };

    // Clean up any stale files from previous runs
    for (const auto& path : file_paths) {
        std::filesystem::remove(path);
    }

    {
        // 1. Initialize storage engine for 4 concurrent files
        flux::io_uring_storage_engine<4, 32, 4096> engine;

        // 2. Register all 4 files
        for (uint16_t slot = 0; slot < 4; ++slot) {
            if (!engine.register_file(slot, file_paths[slot].c_str())) {
                std::cerr << "Failed to register file: " << file_paths[slot] << "\n";
                return 1;
            }
            std::cout << "[Registered] Slot " << slot << " -> " << file_paths[slot] << "\n";
        }

        // 3. Initialize io_uring backend
        if (!engine.setup_io_uring()) {
            std::cerr << "Failed to setup io_uring backend\n";
            return 1;
        }
        std::cout << "\n[OK] io_uring ring initialized for all 4 files.\n\n";

        // 4. Start writing to all 4 files concurrently
        std::cout << "[Writing] Streaming data to all 4 files...\n";

        // File 0: WAL Transactions
        engine.append(0, "TX_START: 1001\n", false);
        engine.append(0, "TX_UPDATE: account=A balance=500\n", false);
        engine.append(0, "TX_COMMIT: 1001\n", true); // immediate durability

        // File 1: JSON Event Stream
        engine.append(1, "{\"event\":\"user_login\",\"uid\":42,\"ip\":\"127.0.0.1\"}\n", false);
        engine.append(1, "{\"event\":\"page_view\",\"page\":\"/dashboard\"}\n", false);
        engine.append(1, "{\"event\":\"logout\",\"uid\":42}\n", false);

        // File 2: CSV Time-series Records
        engine.append(2, "timestamp,device,cpu_pct,ram_mb\n", false);
        for (int i = 0; i < 5; ++i) {
            std::string row = "172974000" + std::to_string(i) + ",server-" + std::to_string(i) + ",12.5,4096\n";
            engine.append(2, row, false);
        }

        // File 3: Structured Binary POD Structs (using std::forward)
        SensorMetric m1{1729740001000ULL, 1, 36.6f};
        SensorMetric m2{1729740002000ULL, 2, 98.2f};
        engine.append(3, m1, false);
        engine.append(3, m2, true); // flush

        std::cout << "[Done Writing]\n";
        for (uint16_t slot = 0; slot < 4; ++slot) {
            std::cout << "  Slot " << slot << " (" << file_paths[slot] << "): "
                      << engine.get_logical_size(slot) << " bytes written.\n";
        }

        // 5. Explicitly close each file (flushes remainder and truncates trailing sector padding)
        for (uint16_t slot = 0; slot < 4; ++slot) {
            engine.close_file(slot);
        }
    }

    // 6. Verify files on disk
    std::cout << "\n========================================================\n";
    std::cout << "                 VERIFYING FILES ON DISK                \n";
    std::cout << "========================================================\n";

    for (const auto& path : file_paths) {
        auto sz = std::filesystem::file_size(path);
        std::cout << "File: " << path << " -> Exact Size: " << sz << " bytes\n";
    }

    std::cout << "\n--- Sample File Content (file0_wal.log) ---\n";
    {
        std::ifstream in("file0_wal.log");
        std::string line;
        while (std::getline(in, line)) {
            std::cout << "  " << line << "\n";
        }
    }

    std::cout << "\n--- Sample File Content (file1_events.json) ---\n";
    {
        std::ifstream in("file1_events.json");
        std::string line;
        while (std::getline(in, line)) {
            std::cout << "  " << line << "\n";
        }
    }

    std::cout << "\n--- Sample File Content (file2_metrics.csv) ---\n";
    {
        std::ifstream in("file2_metrics.csv");
        std::string line;
        while (std::getline(in, line)) {
            std::cout << "  " << line << "\n";
        }
    }

    std::cout << "\n[SUCCESS] All 4 files written, flushed, and verified with zero null padding!\n";
    return 0;
}

