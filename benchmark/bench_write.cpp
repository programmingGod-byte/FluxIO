// Write-only benchmark for the FluxIO io_uring engine.
//
// Methodology (differs from bench_fluxio.cpp on purpose):
//   * time-based runs with a separate warm-up window, not a fixed op count,
//     so every case runs long enough to get past the drive's DRAM/HMB buffer;
//   * every case gets a fresh engine and a known file state
//     (overwrite / unwritten-prealloc / append past EOF), no state carried over;
//   * each case is repeated and reported as the median rep plus spread;
//   * every completion's result is checked, so failed or short writes are
//     counted instead of being reported as throughput;
//   * optional group commit (--sync-every N): drain N writes, then fdatasync;
//   * the environment (governor, write cache, fs, free space, O_DIRECT,
//     SQPOLL, io_uring worker threads) is reported so numbers can be trusted.

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "fluxio/flux.h"
#include "fluxio/util/cpu_affinity.h"
#include "fluxio/util/rdtsc.h"

namespace {

constexpr uint32_t kMaxQd = 128;
constexpr uint64_t kMeasureBit = 1ULL << 63;

enum class FileMode { Overwrite, Prealloc, Append };

struct Workload {
  const char *name;
  FileMode mode;
  bool random;
  bool managed;  // FileBlockManager on (allow_block_allocation = true)
  const char *desc;
};

const Workload kWorkloads[] = {
    {"overwrite-seq", FileMode::Overwrite, false, false,
     "sequential overwrite of a fully written file (best case)"},
    {"overwrite-rand", FileMode::Overwrite, true, false,
     "random overwrite of a fully written file"},
    {"prealloc-seq", FileMode::Prealloc, false, false,
     "one pass into fallocate()d, never-written (unwritten) extents"},
    {"prealloc-rand", FileMode::Prealloc, true, false,
     "random writes into fallocate()d, never-written extents"},
    {"append", FileMode::Append, false, false,
     "append to an empty file past EOF, no FileBlockManager"},
    {"append-managed", FileMode::Append, false, true,
     "append to an empty file with FileBlockManager (library default)"},
};

struct Options {
  std::string dir = ".";
  uint64_t file_size = 1024ULL << 20;
  double runtime_s = 3.0;
  double warmup_s = 1.0;
  uint32_t reps = 3;
  uint32_t sync_every = 0;
  uint32_t core = 2;
  uint32_t cooldown_ms = 500;
  std::vector<uint32_t> qds = {1, 4, 16, 32, 64, 128};
  std::vector<uint32_t> bss = {4096};
  std::vector<const Workload *> workloads;
  std::string csv;
};

struct RunResult {
  uint64_t ops = 0;
  uint64_t errors = 0;
  uint64_t fsyncs = 0;
  double wall_s = 0;
  double cpu_proc_pct = 0;
  double cpu_submit_pct = 0;
  uint32_t iou_workers = 0;
  bool o_direct = false;
  bool sqpoll = false;
  flux::LatencyStats lat{};
  flux::LatencyStats fsync_lat{};

  double iops() const { return wall_s > 0 ? ops / wall_s : 0; }
};

// ---------------------------------------------------------------- helpers

double tv_sec(const timeval &tv) { return tv.tv_sec + tv.tv_usec * 1e-6; }

double cpu_sec(const rusage &a, const rusage &b) {
  return (tv_sec(b.ru_utime) - tv_sec(a.ru_utime)) +
         (tv_sec(b.ru_stime) - tv_sec(a.ru_stime));
}

std::string read_first_line(const std::string &path) {
  std::ifstream f(path);
  std::string s;
  std::getline(f, s);
  return s;
}

// Counts this process's threads whose name starts with `prefix`
// (io_uring spawns "iou-sqp-*" for SQPOLL and "iou-wrk-*" for punted I/O).
uint32_t count_threads(const char *prefix) {
  uint32_t n = 0;
  DIR *d = opendir("/proc/self/task");
  if (!d) return 0;
  while (dirent *e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    std::string comm = read_first_line(std::string("/proc/self/task/") + e->d_name + "/comm");
    if (comm.rfind(prefix, 0) == 0) ++n;
  }
  closedir(d);
  return n;
}

std::vector<uint32_t> parse_list(const std::string &s) {
  std::vector<uint32_t> out;
  std::stringstream ss(s);
  std::string tok;
  while (std::getline(ss, tok, ',')) {
    if (tok.empty()) continue;
    uint64_t mult = 1;
    char last = static_cast<char>(std::tolower(tok.back()));
    if (last == 'k') mult = 1024;
    if (last == 'm') mult = 1024 * 1024;
    if (mult != 1) tok.pop_back();
    out.push_back(static_cast<uint32_t>(std::stoull(tok) * mult));
  }
  return out;
}

const Workload *find_workload(const std::string &name) {
  for (const auto &w : kWorkloads)
    if (name == w.name) return &w;
  return nullptr;
}

// --------------------------------------------------------- file preparation

bool write_full_file(const std::string &path, uint64_t size) {
  int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_DIRECT, 0644);
  if (fd < 0) return false;
  if (::posix_fallocate(fd, 0, static_cast<off_t>(size)) != 0) {
    ::close(fd);
    return false;
  }
  constexpr size_t kChunk = 1 << 20;
  void *buf = std::aligned_alloc(4096, kChunk);
  std::memset(buf, 0xA5, kChunk);
  bool ok = true;
  for (uint64_t off = 0; off < size; off += kChunk) {
    if (::pwrite(fd, buf, kChunk, static_cast<off_t>(off)) != static_cast<ssize_t>(kChunk)) {
      ok = false;
      break;
    }
  }
  std::free(buf);
  ok = ok && ::fdatasync(fd) == 0;
  ::close(fd);
  return ok;
}

bool prepare_file(const std::string &path, FileMode mode, uint64_t size) {
  int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return false;
  bool ok = true;
  if (mode == FileMode::Prealloc) {
    // mode 0: blocks reserved and i_size set, extents stay "unwritten"
    ok = ::fallocate(fd, 0, 0, static_cast<off_t>(size)) == 0;
  }
  ok = ok && ::fsync(fd) == 0;
  ::close(fd);
  return ok;
}

// ------------------------------------------------------------- core loop

template <class Engine>
RunResult run_loop(Engine &eng, const Workload &w, uint32_t bs, uint32_t qd,
                   const Options &o, uint64_t seed) {
  RunResult r;
  const uint64_t nblocks = o.file_size / bs;
  // Overwrite runs for a fixed time and wraps; prealloc/append are one pass
  // over the file (a second pass would just be an overwrite).
  const bool single_pass = w.mode != FileMode::Overwrite;
  const uint64_t warm_blocks = nblocks / 10;

  void *src = std::aligned_alloc(4096, bs);
  uint64_t rng = seed | 1;
  auto next_rand = [&rng]() {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
  };
  for (uint32_t i = 0; i < bs / 8; ++i) static_cast<uint64_t *>(src)[i] = next_rand();

  const double ghz = flux::get_tsc_freq_ghz();
  const uint64_t warm_cycles = static_cast<uint64_t>(o.warmup_s * ghz * 1e9);
  const uint64_t run_cycles = static_cast<uint64_t>(o.runtime_s * ghz * 1e9);

  flux::LatencyTracker lat(static_cast<size_t>(o.runtime_s * 2'000'000));
  flux::LatencyTracker fsync_lat(1024);
  rusage ru_p0{}, ru_p1{}, ru_t0{}, ru_t1{};

  const uint64_t t_begin = flux::rdtsc();
  uint64_t t_measure = 0, t_end = 0;
  bool measuring = false;

  uint64_t issued = 0, next_blk = 0, inflight = 0, since_sync = 0;
  bool fsync_pending = false, exhausted = false;
  uint64_t last_progress = t_begin;
  flux::IoCompletion comp{};

  auto begin_measure = [&](uint64_t now) {
    measuring = true;
    t_measure = now;
    getrusage(RUSAGE_SELF, &ru_p0);
    getrusage(RUSAGE_THREAD, &ru_t0);
  };

  for (;;) {
    const uint64_t now = flux::rdtsc();
    if (!measuring && (single_pass ? issued >= warm_blocks : now - t_begin >= warm_cycles))
      begin_measure(now);
    if (measuring && now - t_measure >= run_cycles) break;
    if (exhausted && inflight == 0 && !fsync_pending) {
      if (!measuring) begin_measure(now);
      break;
    }
    if (now - last_progress > static_cast<uint64_t>(30 * ghz * 1e9)) {
      std::cerr << "  !! no progress for 30s (engine stuck, inflight=" << inflight << ")\n";
      std::abort();
    }

    if (!fsync_pending) {
      const bool sync_due = o.sync_every && since_sync >= o.sync_every;
      if (sync_due && inflight == 0) {
        // io_uring does not order fsync after in-flight writes, so the group
        // is drained first; this is a real group commit.
        flux::IORequest req{};
        req.op_type = flux::Type::Fsync;
        req.file_slot = 0;
        req.user_data = measuring ? kMeasureBit : 0;
        req.timestamp_ns = flux::rdtsc();
        if (eng.try_push_request(std::move(req))) {
          fsync_pending = true;
          since_sync = 0;
        }
      }
      while (!sync_due && inflight < qd && !exhausted &&
             !(o.sync_every && since_sync >= o.sync_every)) {
        uint64_t blk;
        if (w.random) {
          blk = next_rand() % nblocks;
        } else {
          if (next_blk >= nblocks) next_blk = 0;
          blk = next_blk++;
        }
        flux::IORequest req{};
        req.op_type = flux::Type::Write;
        req.file_slot = 0;
        req.length = bs;
        req.file_offset = blk * bs;
        req.user_data = measuring ? (issued | kMeasureBit) : issued;
        req.data_src = src;
        req.timestamp_ns = flux::rdtsc();
        if (!eng.try_push_request(std::move(req))) break;
        ++issued;
        ++inflight;
        ++since_sync;
        if (single_pass && issued >= nblocks) exhausted = true;
      }
    }

    eng.process_submissions(qd + 1);
    eng.poll_completion();
    while (eng.try_pop_completion(comp)) {
      last_progress = comp.complete_ts_ns;
      const bool counted = measuring && (comp.user_data & kMeasureBit);
      if (comp.op_type == flux::Type::Fsync) {
        fsync_pending = false;
        if (comp.result < 0) ++r.errors;
        if (counted) {
          ++r.fsyncs;
          fsync_lat.record_start_end(comp.submit_ts_ns, comp.complete_ts_ns);
        }
        continue;
      }
      --inflight;
      if (comp.result != static_cast<int64_t>(bs)) ++r.errors;
      if (measuring) {
        ++r.ops;
        if (counted) lat.record_start_end(comp.submit_ts_ns, comp.complete_ts_ns);
      }
    }
  }

  t_end = flux::rdtsc();
  getrusage(RUSAGE_SELF, &ru_p1);
  getrusage(RUSAGE_THREAD, &ru_t1);
  r.iou_workers = count_threads("iou-wrk");
  r.sqpoll = count_threads("iou-sqp") > 0;

  // drain whatever is still in flight; it is not counted
  const uint64_t drain_start = flux::rdtsc();
  while (inflight > 0 || fsync_pending) {
    eng.process_submissions(qd + 1);
    eng.poll_completion();
    while (eng.try_pop_completion(comp)) {
      if (comp.op_type == flux::Type::Fsync) fsync_pending = false;
      else --inflight;
    }
    if (flux::rdtsc() - drain_start > static_cast<uint64_t>(30 * ghz * 1e9)) {
      std::cerr << "  !! drain timed out\n";
      std::abort();
    }
  }

  r.wall_s = static_cast<double>(t_end - t_measure) / (ghz * 1e9);
  if (r.wall_s > 0) {
    r.cpu_proc_pct = cpu_sec(ru_p0, ru_p1) / r.wall_s * 100.0;
    r.cpu_submit_pct = cpu_sec(ru_t0, ru_t1) / r.wall_s * 100.0;
  }
  r.lat = lat.compute();
  r.fsync_lat = fsync_lat.compute();
  std::free(src);
  return r;
}

template <class Engine>
bool run_rep(const Workload &w, uint32_t bs, uint32_t qd, const Options &o,
             const std::string &path, uint64_t seed, RunResult &out) {
  // unpin before setup so the SQPOLL kernel thread does not inherit our core
  flux::unpin_current_thread();
  auto eng = std::make_unique<Engine>();
  if (!eng->register_file(0, path.c_str()) || !eng->setup_io_uring()) {
    std::cerr << "  !! engine setup failed for " << path << "\n";
    return false;
  }
  const int fl = ::fcntl(eng->get_fd(0), F_GETFL);
  flux::pin_current_thread_to_core(o.core);
  out = run_loop(*eng, w, bs, qd, o, seed);
  out.o_direct = fl >= 0 && (fl & O_DIRECT);
  flux::unpin_current_thread();
  return true;
}

template <uint32_t BS>
bool dispatch_bs(const Workload &w, uint32_t qd, const Options &o,
                 const std::string &path, uint64_t seed, RunResult &out) {
  if (w.managed)
    return run_rep<flux::io_uring_storage_engine<1, kMaxQd, BS, true>>(w, BS, qd, o, path, seed, out);
  return run_rep<flux::io_uring_storage_engine<1, kMaxQd, BS, false>>(w, BS, qd, o, path, seed, out);
}

bool dispatch(const Workload &w, uint32_t bs, uint32_t qd, const Options &o,
              const std::string &path, uint64_t seed, RunResult &out) {
  switch (bs) {
  case 4096: return dispatch_bs<4096>(w, qd, o, path, seed, out);
  case 16384: return dispatch_bs<16384>(w, qd, o, path, seed, out);
  case 65536: return dispatch_bs<65536>(w, qd, o, path, seed, out);
  case 262144: return dispatch_bs<262144>(w, qd, o, path, seed, out);
  default:
    std::cerr << "unsupported block size " << bs << " (use 4k,16k,64k,256k)\n";
    return false;
  }
}

// --------------------------------------------------------------- reporting

void print_environment(const Options &o) {
  utsname u{};
  uname(&u);
  std::string cpu;
  {
    std::ifstream f("/proc/cpuinfo");
    for (std::string line; std::getline(f, line);)
      if (line.rfind("model name", 0) == 0) {
        cpu = line.substr(line.find(':') + 2);
        break;
      }
  }
  std::string governor = read_first_line(
      "/sys/devices/system/cpu/cpu" + std::to_string(o.core) + "/cpufreq/scaling_governor");

  // find the block device that backs --dir
  std::string real = std::filesystem::canonical(o.dir).string();
  std::string mnt_src, mnt_fs, mnt_point;
  {
    std::ifstream f("/proc/self/mounts");
    std::string src, point, fs, rest;
    while (f >> src >> point >> fs && std::getline(f, rest)) {
      bool prefix = real.rfind(point, 0) == 0 &&
                    (real.size() == point.size() || point == "/" || real[point.size()] == '/');
      if (prefix && point.size() >= mnt_point.size()) {
        mnt_src = src;
        mnt_fs = fs;
        mnt_point = point;
      }
    }
  }
  std::string model, write_cache;
  if (mnt_src.rfind("/dev/", 0) == 0) {
    std::filesystem::path sys = "/sys/class/block/" + mnt_src.substr(5);
    std::error_code ec;
    sys = std::filesystem::canonical(sys, ec);
    if (!ec) {
      if (std::filesystem::exists(sys / "partition")) sys = sys.parent_path();
      model = read_first_line((sys / "device/model").string());
      model.erase(model.find_last_not_of(' ') + 1);
      write_cache = read_first_line((sys / "queue/write_cache").string());
    }
  }
  struct statvfs vfs{};
  statvfs(o.dir.c_str(), &vfs);
  const double free_gib = static_cast<double>(vfs.f_bavail) * vfs.f_frsize / (1ULL << 30);
  const double used_pct =
      100.0 * (1.0 - static_cast<double>(vfs.f_bavail) / static_cast<double>(vfs.f_blocks));

  std::cout << "FluxIO write benchmark\n"
            << "  kernel      : " << u.release << "\n"
            << "  cpu         : " << cpu << " (submitter pinned to core " << o.core
            << ", governor " << (governor.empty() ? "?" : governor) << ")\n"
            << "  tsc         : " << std::fixed << std::setprecision(3)
            << flux::get_tsc_freq_ghz() << " GHz\n"
            << "  target      : " << real << " on " << mnt_src << " (" << mnt_fs << ")\n"
            << "  device      : " << (model.empty() ? "?" : model)
            << ", write cache: " << (write_cache.empty() ? "?" : write_cache) << "\n"
            << "  free space  : " << std::setprecision(1) << free_gib << " GiB ("
            << used_pct << "% used)\n"
            << "  file size   : " << (o.file_size >> 20) << " MiB, runtime " << o.runtime_s
            << "s, warm-up " << o.warmup_s << "s, reps " << o.reps
            << ", sync-every " << o.sync_every << "\n";

  if (governor != "performance")
    std::cout << "  WARNING: cpufreq governor is '" << governor
              << "', expect higher variance (set 'performance' for stable numbers)\n";
  if (write_cache == "write back" && o.sync_every == 0)
    std::cout << "  NOTE: drive has a volatile write cache and --sync-every is 0, so a "
                 "completed write is not yet durable\n";
  if (used_pct > 85.0)
    std::cout << "  WARNING: filesystem is " << std::setprecision(0) << used_pct
              << "% full; ext4 fragmentation and a shrunken SSD SLC cache will lower "
                 "sustained write numbers\n";
  std::cout << "\n";
}

std::string fmt_us(double ns) {
  std::ostringstream s;
  if (ns >= 1e6) s << std::fixed << std::setprecision(2) << ns / 1e6 << "ms";
  else s << std::fixed << std::setprecision(1) << ns / 1e3;
  return s.str();
}

void print_header(bool with_sync) {
  std::cout << std::left << std::setw(16) << "workload" << std::right << std::setw(6) << "bs"
            << std::setw(5) << "qd" << std::setw(11) << "IOPS" << std::setw(8) << "+/-%"
            << std::setw(10) << "MiB/s" << std::setw(9) << "p50us" << std::setw(9) << "p99us"
            << std::setw(10) << "p99.9us" << std::setw(10) << "maxus" << std::setw(8)
            << "cpu%" << std::setw(8) << "sub%" << std::setw(5) << "wrk" << std::setw(5)
            << "err";
  if (with_sync) std::cout << std::setw(10) << "fs-p50" << std::setw(10) << "fs-p99";
  std::cout << "\n" << std::string(with_sync ? 151 : 131, '-') << "\n";
}

void print_row(const Workload &w, uint32_t bs, uint32_t qd, std::vector<RunResult> reps,
               bool with_sync) {
  std::sort(reps.begin(), reps.end(),
            [](const RunResult &a, const RunResult &b) { return a.iops() < b.iops(); });
  const RunResult &m = reps[reps.size() / 2];
  const double lo = reps.front().iops(), hi = reps.back().iops();
  const double spread = m.iops() > 0 ? (hi - lo) / 2.0 / m.iops() * 100.0 : 0;
  uint64_t errors = 0;
  uint32_t wrk = 0;
  for (const auto &r : reps) {
    errors += r.errors;
    wrk = std::max(wrk, r.iou_workers);
  }
  std::ostringstream bs_s;
  bs_s << (bs >> 10) << "k";
  std::cout << std::left << std::setw(16) << w.name << std::right << std::setw(6) << bs_s.str()
            << std::setw(5) << qd << std::setw(11) << std::fixed << std::setprecision(0)
            << m.iops() << std::setw(8) << std::setprecision(1) << spread << std::setw(10)
            << std::setprecision(1) << m.iops() * bs / (1 << 20) << std::setw(9)
            << fmt_us(m.lat.p50_ns) << std::setw(9) << fmt_us(m.lat.p99_ns) << std::setw(10)
            << fmt_us(m.lat.p999_ns) << std::setw(10) << fmt_us(m.lat.max_ns) << std::setw(8)
            << std::setprecision(0) << m.cpu_proc_pct << std::setw(8) << m.cpu_submit_pct
            << std::setw(5) << wrk << std::setw(5) << errors;
  if (with_sync)
    std::cout << std::setw(10) << fmt_us(m.fsync_lat.p50_ns) << std::setw(10)
              << fmt_us(m.fsync_lat.p99_ns);
  std::cout << (m.o_direct ? "" : "  [NOT O_DIRECT]") << (m.sqpoll ? "" : "  [no SQPOLL]")
            << "\n"
            << std::flush;
}

void write_csv_row(std::ofstream &csv, const Workload &w, uint32_t bs, uint32_t qd,
                   uint32_t rep, const Options &o, const RunResult &r) {
  csv << w.name << ',' << bs << ',' << qd << ',' << rep << ',' << o.sync_every << ','
      << r.ops << ',' << r.wall_s << ',' << r.iops() << ',' << r.iops() * bs / (1 << 20) << ','
      << r.errors << ',' << r.lat.min_ns / 1e3 << ',' << r.lat.p50_ns / 1e3 << ','
      << r.lat.p90_ns / 1e3 << ',' << r.lat.p99_ns / 1e3 << ',' << r.lat.p999_ns / 1e3 << ','
      << r.lat.max_ns / 1e3 << ',' << r.cpu_proc_pct << ',' << r.cpu_submit_pct << ','
      << r.iou_workers << ',' << r.fsyncs << ',' << r.fsync_lat.p50_ns / 1e3 << ','
      << r.fsync_lat.p99_ns / 1e3 << '\n';
}

void usage(const char *argv0) {
  std::cout
      << "usage: " << argv0 << " [options]\n"
      << "  --dir PATH          directory for test files (default .)\n"
      << "  --size MiB          file size / per-pass write volume (default 1024)\n"
      << "  --runtime S         measured seconds per overwrite rep (default 3)\n"
      << "  --warmup S          warm-up seconds per overwrite rep (default 1)\n"
      << "  --reps N            repetitions per case (default 3)\n"
      << "  --qd LIST           queue depths, max 128 (default 1,4,16,32,64,128)\n"
      << "  --bs LIST           block sizes: 4k,16k,64k,256k (default 4k)\n"
      << "  --workloads LIST    subset of the workloads below (default all)\n"
      << "  --sync-every N      fdatasync after every N writes (group commit, default 0)\n"
      << "  --core N            core to pin the submitting thread to (default 2)\n"
      << "  --cooldown-ms N     idle time between reps (default 500)\n"
      << "  --csv PATH          write every rep as a CSV row\n"
      << "  --quick             runtime 1s, warm-up 0.5s, 1 rep, qd 1,32,128\n"
      << "workloads:\n";
  for (const auto &w : kWorkloads)
    std::cout << "  " << std::left << std::setw(16) << w.name << w.desc << "\n";
}

} // namespace

int main(int argc, char **argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << a << "\n";
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--dir") o.dir = next();
    else if (a == "--size") o.file_size = std::stoull(next()) << 20;
    else if (a == "--runtime") o.runtime_s = std::stod(next());
    else if (a == "--warmup") o.warmup_s = std::stod(next());
    else if (a == "--reps") o.reps = std::max(1u, static_cast<uint32_t>(std::stoul(next())));
    else if (a == "--qd") o.qds = parse_list(next());
    else if (a == "--bs") o.bss = parse_list(next());
    else if (a == "--sync-every") o.sync_every = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--core") o.core = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--cooldown-ms") o.cooldown_ms = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--csv") o.csv = next();
    else if (a == "--quick") {
      o.runtime_s = 1.0;
      o.warmup_s = 0.5;
      o.reps = 1;
      o.qds = {1, 32, 128};
    } else if (a == "--workloads") {
      std::stringstream ss(next());
      for (std::string t; std::getline(ss, t, ',');) {
        const Workload *w = find_workload(t);
        if (!w) {
          std::cerr << "unknown workload " << t << "\n";
          return 2;
        }
        o.workloads.push_back(w);
      }
    } else if (a == "-h" || a == "--help") {
      usage(argv[0]);
      return 0;
    } else {
      std::cerr << "unknown option " << a << "\n";
      usage(argv[0]);
      return 2;
    }
  }
  if (o.workloads.empty())
    for (const auto &w : kWorkloads) o.workloads.push_back(&w);
  for (uint32_t qd : o.qds)
    if (qd == 0 || qd > kMaxQd) {
      std::cerr << "queue depth must be 1.." << kMaxQd << "\n";
      return 2;
    }
  if (o.file_size == 0 || o.file_size % (256 * 1024) != 0) {
    std::cerr << "--size must be a positive number of MiB\n";
    return 2;
  }

  // at most two test files exist at a time (shared overwrite file + one fresh)
  struct statvfs vfs{};
  if (statvfs(o.dir.c_str(), &vfs) != 0) {
    std::cerr << "cannot stat " << o.dir << "\n";
    return 1;
  }
  const uint64_t free_bytes = static_cast<uint64_t>(vfs.f_bavail) * vfs.f_frsize;
  if (free_bytes < 2 * o.file_size + (512ULL << 20)) {
    std::cerr << "not enough free space in " << o.dir << ": need "
              << ((2 * o.file_size + (512ULL << 20)) >> 20) << " MiB, have "
              << (free_bytes >> 20) << " MiB\n";
    return 1;
  }

  print_environment(o);

  std::ofstream csv;
  if (!o.csv.empty()) {
    csv.open(o.csv);
    csv << "workload,bs,qd,rep,sync_every,ops,wall_s,iops,mib_s,errors,min_us,p50_us,p90_us,"
           "p99_us,p999_us,max_us,cpu_proc_pct,cpu_submit_pct,iou_workers,fsyncs,"
           "fsync_p50_us,fsync_p99_us\n";
  }

  const std::string overwrite_path = (std::filesystem::path(o.dir) / "bench_write_overwrite.dat").string();
  const std::string fresh_path = (std::filesystem::path(o.dir) / "bench_write_fresh.dat").string();
  bool overwrite_ready = false;
  bool ok = true;

  const bool with_sync = o.sync_every > 0;
  print_header(with_sync);
  for (const Workload *w : o.workloads) {
    if (w->mode == FileMode::Overwrite && !overwrite_ready) {
      std::cout << "  (preconditioning " << (o.file_size >> 20) << " MiB overwrite file...)\n"
                << std::flush;
      if (!write_full_file(overwrite_path, o.file_size)) {
        std::cerr << "failed to precondition " << overwrite_path << ": " << strerror(errno) << "\n";
        ok = false;
        break;
      }
      overwrite_ready = true;
    }
    for (uint32_t bs : o.bss) {
      for (uint32_t qd : o.qds) {
        std::vector<RunResult> reps;
        for (uint32_t rep = 0; rep < o.reps && ok; ++rep) {
          std::string path = overwrite_path;
          if (w->mode != FileMode::Overwrite) {
            path = fresh_path;
            if (!prepare_file(path, w->mode, o.file_size)) {
              std::cerr << "failed to prepare " << path << ": " << strerror(errno) << "\n";
              ok = false;
              break;
            }
          }
          RunResult r;
          const uint64_t seed = 0x9E3779B97F4A7C15ULL * (rep + 1) ^ (uint64_t(qd) << 32) ^ bs;
          if (!dispatch(*w, bs, qd, o, path, seed, r)) {
            ok = false;
            break;
          }
          if (csv.is_open()) write_csv_row(csv, *w, bs, qd, rep, o, r);
          reps.push_back(r);
          if (w->mode != FileMode::Overwrite) std::filesystem::remove(path);
          std::this_thread::sleep_for(std::chrono::milliseconds(o.cooldown_ms));
        }
        if (!ok) break;
        print_row(*w, bs, qd, reps, with_sync);
      }
      if (!ok) break;
    }
    if (!ok) break;
    std::cout << std::string(with_sync ? 151 : 131, '-') << "\n";
  }

  std::error_code ec;
  std::filesystem::remove(overwrite_path, ec);
  std::filesystem::remove(fresh_path, ec);

  std::cout << "\ncolumns: IOPS = median rep; +/-% = half the min..max spread across reps;\n"
               "latency = submit-to-completion of the median rep; cpu% = whole process\n"
               "(includes the SQPOLL kernel thread); sub% = submitting thread only;\n"
               "wrk = io_uring worker threads seen (>0 means writes were punted to\n"
               "blocking workers); err = failed or short writes across all reps.\n";
  return ok ? 0 : 1;
}
