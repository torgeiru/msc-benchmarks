#ifdef UNIKERNEL
#include <os>
#endif

#define DEBUG
#define VERIFY

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <format>
#include <iostream>
#include <sys/uio.h>
#include <unistd.h>

const char *seq_write_bench = "VirtioFS0/vec_seq_write_bench.csv";
const char *seq_read_bench = "VirtioFS0/vec_seq_read_bench.csv";

const char *testfile = "VirtioFS0/syncio_testing_file.bin";
const char *seq_read_bench_copy = "VirtioFS0/vec_seq_read_copy.bin";
const char *seq_write_bench_copy = "VirtioFS0/vec_seq_write_copy.bin";

constexpr size_t FILESIZE = 214748364; // Hardcoded from size of the file
constexpr size_t DIRECT_IO_ALIGNMENT = 4096;
constexpr int RUNS = 30;
constexpr int MAX_IOV_COUNT = 16;

constexpr std::array<size_t, 5> CHUNK_SIZES = {
  1024,
  2 * 1024,
  4 * 1024,
  8 * 1024,
  16 * 1024,
};
constexpr std::array<int, 5> IOV_COUNTS = {1, 2, 4, 8, 16};
constexpr size_t MAX_REQUEST_SIZE = CHUNK_SIZES.back() * IOV_COUNTS.back();

const char *csv_header = "chunk_size,iov_count,run_number,time_ms,throughput_mibps\n";
const size_t csv_header_len = std::strlen(csv_header);

int seq_read_benchmark();
int seq_write_benchmark();

int main() {
  if (seq_read_benchmark()) goto end_bench;
  if (seq_write_benchmark()) goto end_bench;

  std::cout << "Running the vectorized benchmark suite was a success!\n";

end_bench:
#ifdef UNIKERNEL
  os::shutdown();
#else
  return 0;
#endif
}

struct BenchmarkResult {
  size_t chunk_size;
  int iov_count;
  int run_number;
  double time_ms;
  double throughput_mibps;
};

void close_if_open(int fd) {
  if (fd >= 0) {
    close(fd);
  }
}

void *allocate_file_buffer() {
  void *buffer = nullptr;

  if (posix_memalign(&buffer, DIRECT_IO_ALIGNMENT, FILESIZE) != 0) {
    return nullptr;
  }

  std::memset(buffer, 0, FILESIZE);
  return buffer;
}

bool write_all(int fd, const void *buffer, size_t count) {
  const unsigned char *bytes = static_cast<const unsigned char*>(buffer);
  size_t progress = 0;

  while (progress < count) {
    ssize_t written = write(fd, bytes + progress, count - progress);
    if (written <= 0) {
      perror("write");
      return false;
    }
    progress += static_cast<size_t>(written);
  }

  return true;
}

bool read_all(int fd, void *buffer, size_t count) {
  unsigned char *bytes = static_cast<unsigned char*>(buffer);
  size_t progress = 0;

  while (progress < count) {
    ssize_t read_bytes = read(fd, bytes + progress, count - progress);
    if (read_bytes <= 0) {
      perror("read");
      return false;
    }
    progress += static_cast<size_t>(read_bytes);
  }

  return true;
}

void write_result(int results_fd, const BenchmarkResult& result) {
  std::string results_str = std::format("{},{},{},{:.3f},{:.3f}\n",
    result.chunk_size,
    result.iov_count,
    result.run_number,
    result.time_ms,
    result.throughput_mibps
  );

  if (!write_all(results_fd, results_str.data(), results_str.size())) {
    std::cerr << "Error writing result to file\n";
    std::exit(EXIT_FAILURE);
  }
}

bool dump_file_copy(const char *filename, unsigned char *filebuf) {
  int dump_fd = open(filename, O_CREAT | O_TRUNC | O_WRONLY, 0666);
  if (dump_fd < 0) {
    perror("open");
    std::cerr << "Failed to open/create file to be dumped!\n";
    return false;
  }

  size_t progress = 0;
  while (progress < FILESIZE) {
    size_t to_write = std::min(FILESIZE - progress, MAX_REQUEST_SIZE);
    if (!write_all(dump_fd, &filebuf[progress], to_write)) {
      close_if_open(dump_fd);
      return false;
    }
    progress += to_write;
  }

  close_if_open(dump_fd);
  return true;
}

bool read_file(unsigned char *filebuf) {
  int read_fd = open(testfile, O_RDONLY);
  if (read_fd < 0) {
    perror("open");
    std::cerr << "Failed to open read file!\n";
    return false;
  }

  bool ok = read_all(read_fd, filebuf, FILESIZE);
  close_if_open(read_fd);
  return ok;
}

void make_iovecs(std::array<iovec, MAX_IOV_COUNT>& iovs,
  unsigned char *base, size_t chunk_size, int iov_count, size_t bytes_left) {
  for (int i = 0; i < iov_count && bytes_left > 0; ++i) {
    size_t iov_len = std::min(chunk_size, bytes_left);
    iovec io{};
    io.iov_base = base + (static_cast<size_t>(i) * chunk_size);
    io.iov_len = iov_len;
    iovs[static_cast<size_t>(i)] = io;
    bytes_left -= iov_len;
  }
}

bool process_vec_request(int fd, unsigned char *buffer, size_t chunk_size,
  int iov_count, size_t bytes_left, bool read_direction) {
  std::array<iovec, MAX_IOV_COUNT> iovs{};
  const size_t request_size = chunk_size * static_cast<size_t>(iov_count);
  const size_t expected_size = std::min(request_size, bytes_left);
  make_iovecs(iovs, buffer, chunk_size, iov_count, expected_size);

  ssize_t processed = read_direction
    ? readv(fd, iovs.data(), iov_count)
    : writev(fd, iovs.data(), iov_count);

  if (processed <= 0) {
    perror(read_direction ? "readv" : "writev");
    return false;
  }

  if (static_cast<size_t>(processed) != expected_size) {
    std::cerr << (read_direction ? "readv" : "writev")
              << " processed a partial request\n";
    return false;
  }

  return true;
}

int seq_chunk_bench(int results_fd, size_t chunk_size, int iov_count,
  bool read_direction, bool dump_file) {
  unsigned char *filebuf = static_cast<unsigned char*>(allocate_file_buffer());

  if (filebuf == nullptr) {
    std::cerr << "Failed to allocate file buffer!\n";
    return -1;
  }

  if (!read_direction && !read_file(filebuf)) {
    std::free(filebuf);
    return -1;
  }

  for (int run = 0; run < RUNS; ++run) {
    int test_fd = read_direction
      ? open(testfile, O_RDONLY | O_DIRECT)
      : open(seq_write_bench_copy, O_CREAT | O_TRUNC | O_WRONLY | O_DIRECT, 0666);

    if (test_fd < 0) {
      perror("open");
      std::cerr << "Failed to open benchmark file\n";
      std::free(filebuf);
      return -1;
    }

    auto start = std::chrono::high_resolution_clock::now();

    const size_t request_size = chunk_size * static_cast<size_t>(iov_count);
    size_t cur_processed_bytes = 0;
    while (cur_processed_bytes < FILESIZE) {
      const size_t bytes_left = FILESIZE - cur_processed_bytes;
      if (!process_vec_request(
            test_fd,
            &filebuf[cur_processed_bytes],
            chunk_size,
            iov_count,
            bytes_left,
            read_direction)) {
        close_if_open(test_fd);
        std::free(filebuf);
        return -1;
      }

      cur_processed_bytes += std::min(request_size, bytes_left);
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> start_stop_diff = end - start;

    double time_ms = start_stop_diff.count() * 1000.0;
    double throughput_mibps = (FILESIZE / (1024.0 * 1024.0)) / start_stop_diff.count();

#ifdef DEBUG
    std::cout << "Sequential vector processing time is " << time_ms << "ms\n";
#endif

    BenchmarkResult result = {
      chunk_size,
      iov_count,
      run + 1,
      time_ms,
      throughput_mibps
    };
    write_result(results_fd, result);

#ifdef VERIFY
    if ((run == 0) && dump_file && read_direction) {
      if (!dump_file_copy(seq_read_bench_copy, filebuf)) {
        close_if_open(test_fd);
        std::free(filebuf);
        return -1;
      }
    }
#endif

    close_if_open(test_fd);

    if (!read_direction) {
#ifdef VERIFY
      if (!dump_file || (run != (RUNS - 1))) {
#endif
        unlink(seq_write_bench_copy);
#ifdef VERIFY
      }
#endif
    }
  }

  std::free(filebuf);
  return 0;
}

int seq_read_benchmark() {
  int results_fd = open(seq_read_bench, O_CREAT | O_TRUNC | O_WRONLY, 0666);
  if (results_fd < 0) {
    perror("open");
    return -1;
  }

  if (!write_all(results_fd, csv_header, csv_header_len)) {
    close_if_open(results_fd);
    return -1;
  }

  for (size_t chunk_size: CHUNK_SIZES) {
    for (int iov_count: IOV_COUNTS) {
      bool dump_file = (iov_count == IOV_COUNTS.back())
        && (chunk_size == CHUNK_SIZES.back());
      if (seq_chunk_bench(results_fd, chunk_size, iov_count, true, dump_file)) {
        close_if_open(results_fd);
        return -1;
      }
    }
  }

  close_if_open(results_fd);

  std::cout << "Sequential vector read benchmark was a success!\n";
  return 0;
}

int seq_write_benchmark() {
  int results_fd = open(seq_write_bench, O_CREAT | O_TRUNC | O_WRONLY, 0666);
  if (results_fd < 0) {
    perror("open");
    return -1;
  }

  if (!write_all(results_fd, csv_header, csv_header_len)) {
    close_if_open(results_fd);
    return -1;
  }

  for (size_t chunk_size: CHUNK_SIZES) {
    for (int iov_count: IOV_COUNTS) {
      bool dump_file = (iov_count == IOV_COUNTS.back())
        && (chunk_size == CHUNK_SIZES.back());
      if (seq_chunk_bench(results_fd, chunk_size, iov_count, false, dump_file)) {
        close_if_open(results_fd);
        return -1;
      }
    }
  }

  close_if_open(results_fd);

  std::cout << "Sequential vector write benchmark was a success!\n";
  return 0;
}
