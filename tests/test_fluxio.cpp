#include <fluxio.h>
#include <iostream>
#include <vector>
#include <filesystem>
#include <cassert>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

void test_data_integrity() {
    std::cout << "Running Data Integrity Test..." << std::endl;
    std::string filename = "test_data_integrity.dat";
    
    // Create empty file
    int fd = ::open(filename.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    assert(fd >= 0);
    ::close(fd);

    flux::io_uring_storage_engine<2, 32, 4096> engine;
    assert(engine.register_file(0, filename.c_str()));
    assert(engine.setup_io_uring());

    std::vector<char> write_buf(4096, 'A');
    std::vector<char> read_buf(4096, 'B'); // Different initial value

    // Write chunk
    flux::IORequest w_req{};
    w_req.op_type = flux::Type::Write;
    w_req.file_slot = 0;
    w_req.length = 4096;
    w_req.file_offset = 0;
    w_req.user_data = 1;
    w_req.data_src = write_buf.data();

    engine.try_push_request(std::move(w_req));
    engine.submit();
    engine.process_submissions(1);
    
    flux::IoCompletion comp;
    bool done = false;
    while (!done) {
        engine.poll_completion();
        if (engine.try_pop_completion(comp)) {
            assert(comp.user_data == 1);
            done = true;
        }
    }

    // Read chunk
    flux::IORequest r_req{};
    r_req.op_type = flux::Type::Read;
    r_req.file_slot = 0;
    r_req.length = 4096;
    r_req.file_offset = 0;
    r_req.user_data = 2;
    r_req.data_src = read_buf.data(); // read destination

    engine.try_push_request(std::move(r_req));
    engine.submit();
    engine.process_submissions(1);

    done = false;
    while (!done) {
        engine.poll_completion();
        if (engine.try_pop_completion(comp)) {
            assert(comp.user_data == 2);
            done = true;
        }
    }

    // Verify data
    assert(std::memcmp(write_buf.data(), read_buf.data(), 4096) == 0);
    std::filesystem::remove(filename);
    std::cout << "Data Integrity Test Passed!" << std::endl;
}

int main() {
    test_data_integrity();
    std::cout << "All tests passed successfully!" << std::endl;
    return 0;
}
