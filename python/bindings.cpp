#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <fluxio/flux_io.h>
#include <unordered_map>
#include <string>
#include <memory>
#include <stdexcept>
#include <chrono>
#include <thread>

namespace py = pybind11;

namespace {

template <typename EngineType>
class PyStorageEngineT {
public:
    PyStorageEngineT() : engine_(std::make_unique<EngineType>()) {}

    bool register_file(size_t slot, const std::string& path) {
        return engine_->register_file(slot, path.c_str());
    }

    bool setup() {
        return engine_->setup_io_uring();
    }

    int get_fd(size_t slot) const {
        return engine_->get_fd(slot);
    }

    uint32_t file_count() const {
        return engine_->file_count();
    }

    bool write(size_t slot, py::buffer buf, uint64_t offset, uint64_t user_data) {
        py::buffer_info info = buf.request();
        if (info.size * info.itemsize == 0) {
            throw std::invalid_argument("Buffer cannot be empty");
        }
        if (static_cast<size_t>(info.size * info.itemsize) > 4096) {
            throw std::invalid_argument("Buffer size exceeds page size (4096 bytes)");
        }

        in_flight_refs_[user_data] = buf;

        flux::IORequest req{};
        req.op_type = flux::Type::Write;
        req.file_slot = static_cast<uint16_t>(slot);
        req.length = static_cast<uint32_t>(info.size * info.itemsize);
        req.file_offset = offset;
        req.user_data = user_data;
        req.data_src = info.ptr;
        req.timestamp_ns = flux::rdtsc();

        return engine_->try_push_request(std::move(req));
    }

    bool read(size_t slot, py::buffer buf, uint64_t offset, uint64_t user_data) {
        py::buffer_info info = buf.request(true /* writable */);
        if (info.size * info.itemsize == 0) {
            throw std::invalid_argument("Buffer cannot be empty");
        }
        if (static_cast<size_t>(info.size * info.itemsize) > 4096) {
            throw std::invalid_argument("Buffer size exceeds page size (4096 bytes)");
        }

        in_flight_refs_[user_data] = buf;

        flux::IORequest req{};
        req.op_type = flux::Type::Read;
        req.file_slot = static_cast<uint16_t>(slot);
        req.length = static_cast<uint32_t>(info.size * info.itemsize);
        req.file_offset = offset;
        req.user_data = user_data;
        req.data_src = info.ptr;
        req.timestamp_ns = flux::rdtsc();

        return engine_->try_push_request(std::move(req));
    }

    bool fsync(size_t slot, uint64_t user_data) {
        flux::IORequest req{};
        req.op_type = flux::Type::Fsync;
        req.file_slot = static_cast<uint16_t>(slot);
        req.user_data = user_data;
        req.timestamp_ns = flux::rdtsc();

        return engine_->try_push_request(std::move(req));
    }

    int process_submissions(uint32_t max_batch = 64) {
        return engine_->process_submissions(max_batch);
    }

    void submit() {
        engine_->submit();
    }

    void poll_completion() {
        engine_->poll_completion();
    }

    py::object try_pop_completion() {
        flux::IoCompletion comp;
        if (engine_->try_pop_completion(comp)) {
            in_flight_refs_.erase(comp.user_data);
            return py::cast(comp);
        }
        return py::none();
    }

    py::object wait_completion(int timeout_ms = -1) {
        flux::IoCompletion comp;
        auto start = std::chrono::steady_clock::now();

        while (true) {
            engine_->poll_completion();
            if (engine_->try_pop_completion(comp)) {
                in_flight_refs_.erase(comp.user_data);
                return py::cast(comp);
            }

            if (timeout_ms >= 0) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
                if (elapsed >= timeout_ms) {
                    return py::none();
                }
            }

            std::this_thread::yield();
        }
    }

    int64_t write_block(size_t slot, py::buffer buf, uint64_t offset) {
        py::buffer_info info = buf.request();
        size_t len = info.size * info.itemsize;
        if (len == 0 || len > 4096) {
            throw std::invalid_argument("Buffer size must be between 1 and 4096 bytes");
        }

        flux::IoCompletion comp{};
        {
            py::gil_scoped_release release;

            flux::IORequest req{};
            req.op_type = flux::Type::Write;
            req.file_slot = static_cast<uint16_t>(slot);
            req.length = static_cast<uint32_t>(len);
            req.file_offset = offset;
            req.user_data = 0xAA;
            req.data_src = info.ptr;
            req.timestamp_ns = flux::rdtsc();

            if (!engine_->try_push_request(std::move(req))) {
                throw std::runtime_error("FluxIO SQ ring full: failed to push request");
            }
            engine_->process_submissions(1);

            while (!engine_->try_pop_completion(comp)) {
                engine_->poll_completion();
            }
        }

        if (comp.failed) {
            throw std::runtime_error("FluxIO write failed with error code: " + std::to_string(comp.result));
        }
        return comp.result;
    }

    py::bytes read_block(size_t slot, size_t size, uint64_t offset) {
        if (size == 0 || size > 4096) {
            throw std::invalid_argument("Read size must be between 1 and 4096 bytes");
        }

        std::string buffer;
        buffer.resize(size);
        flux::IoCompletion comp{};

        {
            py::gil_scoped_release release;

            flux::IORequest req{};
            req.op_type = flux::Type::Read;
            req.file_slot = static_cast<uint16_t>(slot);
            req.length = static_cast<uint32_t>(size);
            req.file_offset = offset;
            req.user_data = 0xBB;
            req.data_src = buffer.data();
            req.timestamp_ns = flux::rdtsc();

            if (!engine_->try_push_request(std::move(req))) {
                throw std::runtime_error("FluxIO SQ ring full: failed to push read request");
            }
            engine_->process_submissions(1);

            while (!engine_->try_pop_completion(comp)) {
                engine_->poll_completion();
            }
        }

        if (comp.failed) {
            throw std::runtime_error("FluxIO read failed with error code: " + std::to_string(comp.result));
        }

        if (comp.result < static_cast<int64_t>(size)) {
            buffer.resize(comp.result > 0 ? comp.result : 0);
        }
        return py::bytes(buffer);
    }

    bool fsync_block(size_t slot) {
        flux::IoCompletion comp{};
        {
            py::gil_scoped_release release;

            flux::IORequest req{};
            req.op_type = flux::Type::Fsync;
            req.file_slot = static_cast<uint16_t>(slot);
            req.user_data = 0xCC;
            req.timestamp_ns = flux::rdtsc();

            if (!engine_->try_push_request(std::move(req))) {
                throw std::runtime_error("FluxIO SQ ring full: failed to push fsync request");
            }
            engine_->process_submissions(1);

            while (!engine_->try_pop_completion(comp)) {
                engine_->poll_completion();
            }
        }

        if (comp.failed) {
            throw std::runtime_error("FluxIO fsync failed with error code: " + std::to_string(comp.result));
        }
        return true;
    }

    PyStorageEngineT& enter() { return *this; }
    void exit(py::object, py::object, py::object) {
        engine_->process_submissions(128);
    }

private:
    std::unique_ptr<EngineType> engine_;
    std::unordered_map<uint64_t, py::object> in_flight_refs_;
};

#if FLUX_HAS_IO_URING
using PyIoUringStorageEngine = PyStorageEngineT<flux::io_uring_storage_engine<4, 128, 4096>>;
#endif
using PyPosixStorageEngine = PyStorageEngineT<flux::posix_storage_engine<4, 128, 4096>>;

template <typename PyClass>
void register_engine_methods(PyClass& cl) {
    cl.def("register_file", &PyClass::type::register_file, py::arg("slot"), py::arg("path"),
           "Register a file path to an internal slot index with Direct I/O")
      .def("setup", &PyClass::type::setup,
           "Initialize engine queues, register fixed buffers and file descriptors")
      .def("get_fd", &PyClass::type::get_fd, py::arg("slot"), "Get raw OS file descriptor for slot")
      .def("file_count", &PyClass::type::file_count, "Get count of registered files")
      .def("write", &PyClass::type::write, py::arg("slot"), py::arg("buffer"), py::arg("offset"), py::arg("user_data") = 0,
           "Push an asynchronous write request to SQ ring (supports bytes, bytearray, memoryview, numpy)")
      .def("read", &PyClass::type::read, py::arg("slot"), py::arg("buffer"), py::arg("offset"), py::arg("user_data") = 0,
           "Push an asynchronous read request to SQ ring into a writable buffer")
      .def("fsync", &PyClass::type::fsync, py::arg("slot"), py::arg("user_data") = 0,
           "Push an asynchronous fsync request for slot")
      .def("process_submissions", &PyClass::type::process_submissions, py::arg("max_batch") = 64,
           "Process queued SQ ring submissions")
      .def("submit", &PyClass::type::submit, "Submit pending requests to kernel/backend")
      .def("poll_completion", &PyClass::type::poll_completion, "Poll completion queue for finished requests")
      .def("try_pop_completion", &PyClass::type::try_pop_completion, "Pop a completed request, or return None")
      .def("wait_completion", &PyClass::type::wait_completion, py::arg("timeout_ms") = -1,
           "Wait and pop next completion (optional timeout in ms)")
      .def("write_block", &PyClass::type::write_block, py::arg("slot"), py::arg("buffer"), py::arg("offset"),
           "Blocking synchronous write of buffer up to 4096 bytes")
      .def("read_block", &PyClass::type::read_block, py::arg("slot"), py::arg("size"), py::arg("offset"),
           "Blocking synchronous read returning bytes object")
      .def("fsync_block", &PyClass::type::fsync_block, py::arg("slot"),
           "Blocking synchronous fsync on file slot")
      .def("__enter__", &PyClass::type::enter)
      .def("__exit__", &PyClass::type::exit);
}

std::string op_type_to_string(flux::Type type) {
    switch (type) {
        case flux::Type::Read: return "Read";
        case flux::Type::Write: return "Write";
        case flux::Type::Fsync: return "Fsync";
        case flux::Type::Close: return "Close";
        default: return "Unknown";
    }
}

} // namespace

PYBIND11_MODULE(fluxio, m) {
    m.doc() = "FluxIO: Ultra-low latency asynchronous storage engine in modern C++20 for Python";

    py::enum_<flux::Type>(m, "OpType")
        .value("Read", flux::Type::Read)
        .value("Write", flux::Type::Write)
        .value("Fsync", flux::Type::Fsync)
        .value("Close", flux::Type::Close)
        .export_values();

    py::class_<flux::IoCompletion>(m, "IoCompletion")
        .def_readonly("user_data", &flux::IoCompletion::user_data, "User-assigned request tag")
        .def_readonly("result", &flux::IoCompletion::result, "Result code (bytes transferred or -errno)")
        .def_readonly("file_slot", &flux::IoCompletion::file_slot, "Target registered file slot")
        .def_readonly("failed", &flux::IoCompletion::failed, "True if operation returned negative errno")
        .def_property_readonly("op_type", [](const flux::IoCompletion& c) {
            return op_type_to_string(c.op_type);
        }, "Operation type string (Read, Write, Fsync)")
        .def_property_readonly("latency_cycles", [](const flux::IoCompletion& c) {
            return c.complete_ts_ns > c.submit_ts_ns ? (c.complete_ts_ns - c.submit_ts_ns) : 0;
        }, "Cycle latency recorded via RDTSC")
        .def("__repr__", [](const flux::IoCompletion& c) {
            return "<IoCompletion op=" + op_type_to_string(c.op_type) +
                   " slot=" + std::to_string(c.file_slot) +
                   " result=" + std::to_string(c.result) +
                   " failed=" + (c.failed ? "True" : "False") + ">";
        });

#if FLUX_HAS_IO_URING
    auto io_uring_cls = py::class_<PyIoUringStorageEngine>(m, "StorageEngine",
        "Primary FluxIO storage engine utilizing Linux io_uring with kernel SQPOLL and registered buffers");
    io_uring_cls.def(py::init<>());
    register_engine_methods(io_uring_cls);
#endif

    auto posix_cls = py::class_<PyPosixStorageEngine>(m, "PosixStorageEngine",
        "Fallback FluxIO storage engine utilizing synchronous POSIX Direct I/O and lock-free rings");
    posix_cls.def(py::init<>());
    register_engine_methods(posix_cls);

    // Hardware and backend utilities
    m.def("pin_thread_to_core", &flux::pin_current_thread_to_core, py::arg("core_id"),
          "Pin current calling thread to specific CPU core");
    m.def("unpin_thread", &flux::unpin_current_thread, "Unpin current thread affinity");
    m.def("get_current_core", &flux::get_current_core, "Get current running CPU core ID");
    m.def("has_io_uring", []() {
#if FLUX_HAS_IO_URING
        return true;
#else
        return false;
#endif
    }, "Check if FluxIO was compiled with native io_uring support");
}
