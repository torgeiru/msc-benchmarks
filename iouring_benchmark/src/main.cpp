#include <iostream>
#include <algorithm>
#include <random>
#include <vector>
#include <chrono>
#include <format>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <atomic>

#include <liburing.h>

#define DEBUG
#define VERIFY

const char *seq_write_bench = "VirtioFS0/seq_write_bench.csv";
const char *rng_write_bench = "VirtioFS0/rng_write_bench.csv";
const char *seq_read_bench = "VirtioFS0/seq_read_bench.csv";
const char *rng_read_bench = "VirtioFS0/rng_read_bench.csv";

const char *testfile = "VirtioFS0/syncio_testing_file.bin";
const char *seq_read_bench_copy = "VirtioFS0/seq_read_copy.bin";
const char *rng_read_bench_copy = "VirtioFS0/rng_read_copy.bin";
const char *seq_write_bench_copy = "VirtioFS0/seq_write_copy.bin";
const char *rng_write_bench_copy = "VirtioFS0/rng_write_copy.bin";

constexpr size_t FILESIZE = 214748364; // Hardcoded from size of the file
constexpr size_t DIRECT_IO_ALIGNMENT = 4096;
constexpr size_t INCREMENTAL = 8192;
constexpr size_t INCREMENTAL_START = 8192;
constexpr size_t LARGEST_CHUNK = (256 * 1024);

const char *csv_header = "chunk_size,run_number,time_ms,throughput_mibps\n";
const size_t csv_header_len = std::strlen(csv_header);

/* IO uring */
constexpr int QUEUE_DEPTH = 1;
struct io_uring ring;
constexpr unsigned int KERNEL_MAX_POLL_MS = 10000;


int seq_read_benchmark();
int rng_read_benchmark();
int seq_write_benchmark();
int rng_write_benchmark();

unsigned char *allocate_file_buffer() {
  void *buffer = nullptr;
  if (posix_memalign(&buffer, DIRECT_IO_ALIGNMENT, FILESIZE) != 0) {
    return nullptr;
  }

  std::memset(buffer, 0, FILESIZE);
  return static_cast<unsigned char*>(buffer);
}

int main() {
  struct io_uring_params params;
  std::memset(&params, 0, sizeof(struct io_uring_params));
  params.flags = IORING_SETUP_SQPOLL;
  params.sq_thread_idle = KERNEL_MAX_POLL_MS;

  int ret = io_uring_queue_init_params(QUEUE_DEPTH, &ring, &params);
  if (ret < 0) {
    std::cerr << "io_uring_queue_init_params failed!\n";
    goto end_bench_iouring_setup_failed;
  }

  if (seq_read_benchmark()) goto end_bench;
  if (rng_read_benchmark()) goto end_bench;
  if (seq_write_benchmark()) goto end_bench;
  if (rng_write_benchmark()) goto end_bench;

  std::cout << "Running the benchmark suite was a success!\n";

end_bench:
  io_uring_queue_exit(&ring);
end_bench_iouring_setup_failed:
  return 0;
}

struct BenchmarkResult {
  size_t chunk_size;
  int run_number;
  double time_ms;
  double throughput_mbps;
};

void write_result(int results_fd, const struct BenchmarkResult& result) {
  std::string results_str  = std::format("{},{},{:.3f},{:.3f}\n",
    result.chunk_size,
    result.run_number,
    result.time_ms,
    result.throughput_mbps
  );

  ssize_t written = write(results_fd, results_str.data(), results_str.size());
  if (written != results_str.size()) {
    std::cerr << "Error writing result to file\n";
    std::exit(EXIT_FAILURE);
  }
}

void dump_file_copy(const char *filename, unsigned char *filebuf) {
  int dump_fd = open(filename, O_CREAT | O_WRONLY, 777);
  if (!dump_fd) {
    std::cerr << "Failed to open/create file to be dumped!\n";
    return;
  }

  ssize_t progress = 0;
  ssize_t written_size;
  while(progress < FILESIZE) {
    ssize_t to_write = FILESIZE - progress;
    if (to_write > LARGEST_CHUNK) {
      to_write = LARGEST_CHUNK;
    }

    ssize_t written_size = write(dump_fd, &filebuf[progress], to_write);
    if (written_size < 0) {
      std::cerr << "Failed to dump any bytes in this write!\n";
      close(dump_fd);
      return;
    }

    progress += written_size;
  }

  close(dump_fd);
}

void read_file(unsigned char *filebuf) {
  int read_fd = open(testfile, O_RDONLY);
  if (!read_fd) {
    std::cerr << "Failed to open read file!\n";
    return;
  }

  if (read(read_fd, filebuf, FILESIZE) < FILESIZE) {
    std::cerr << "Failed to read entire file!\n";
  }

  close(read_fd);
}

int seq_chunk_bench(int results_fd, size_t chunk_size,
  bool read_direction, bool dump_file) {
  unsigned char *chunk = allocate_file_buffer();

  if (chunk == nullptr) {
    std::cerr << "Failed to allocate chunk for writing!\n";
    return -1;
  }

  if (not read_direction) {
    read_file(chunk);
  }

  for (int run = 0; run < 30; ++run) {
    int test_fd;
    if (read_direction) {
      test_fd = open(testfile, O_RDONLY | O_DIRECT);
    } else {
      test_fd = open(seq_write_bench_copy, O_CREAT | O_WRONLY | O_DIRECT, 777);
    }

    if (io_uring_register_files(&ring, &test_fd, 1)) {
      std::cerr << "Failed to register filedes with io uring!\n";
      return -1;
    }
 
    if (test_fd < 0) {
      std::cerr << "Failed to open benchmark file\n";
      return -1;
    }

    auto start = std::chrono::high_resolution_clock::now();

    ssize_t cur_processed_bytes = 0;
    while (cur_processed_bytes < FILESIZE) {
      size_t left_to_process = FILESIZE - cur_processed_bytes;
      size_t to_process = left_to_process > chunk_size ? chunk_size : left_to_process;
      ssize_t newly_processed_size;

      struct io_uring_sqe *sqe;
      while((sqe = io_uring_get_sqe(&ring)) == nullptr);

      if (read_direction) {
        io_uring_prep_read(sqe, 0, &chunk[cur_processed_bytes], to_process, cur_processed_bytes);
      } else {
        io_uring_prep_write(sqe, 0, &chunk[cur_processed_bytes], to_process, cur_processed_bytes);
      }
      sqe->flags |= IOSQE_FIXED_FILE;

      io_uring_submit(&ring);

      struct io_uring_cqe *cqe;
      while(io_uring_peek_cqe(&ring, &cqe));

      newly_processed_size = cqe->res;

      io_uring_cqe_seen(&ring, cqe);

      if (newly_processed_size < to_process) {
        std::cerr << "Processed " << newly_processed_size << " bytes but needed " << to_process << "\n";
        return -1;
      }

      cur_processed_bytes += newly_processed_size;
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> start_stop_diff = end - start;

    double time_ms = start_stop_diff.count() * 1000.0;
    double throughput_mbps = (FILESIZE / (1024.0 * 1024.0)) / start_stop_diff.count();

#ifdef DEBUG
    std::cout << "Sequential processing time is " << time_ms << "ms\n";
#endif

    struct BenchmarkResult result = {chunk_size, run + 1, time_ms, throughput_mbps};
    write_result(results_fd, result);

    if (io_uring_unregister_files(&ring)) {
      std::cerr << "Failed to unregister files with io uring!\n";
      return -1;
    }

#ifdef VERIFY
    if ((run == 0) && dump_file && read_direction) {
      dump_file_copy(seq_read_bench_copy, chunk);
    }
#endif

    close(test_fd);

    if (not read_direction) {
#ifdef VERIFY
      if (not dump_file || (run != 29)) {
#endif
        unlink(seq_write_bench_copy);
#ifdef VERIFY
      }
#endif
    }
  }

  std::free(chunk);

  return 0;
}

int seq_read_benchmark() {
  int results_fd = open(seq_read_bench, O_CREAT | O_WRONLY, 777);
  if (results_fd < 0) {
    std::cerr << "Failed to open results file for sequential read benchmark results!\n";
    return -1;
  }

  if (write(results_fd, csv_header, csv_header_len) <= 0) {
    std::cerr << "Failed to write csv header for sequential read benchmark results!\n";
    return -1;
  }

  for (size_t chunk_size = INCREMENTAL_START; 
    chunk_size < LARGEST_CHUNK; 
    chunk_size += INCREMENTAL)
    {
    if (chunk_size == (LARGEST_CHUNK - INCREMENTAL)) {
      if (seq_chunk_bench(results_fd, chunk_size, true, true)) return -1;
    } else {
      if (seq_chunk_bench(results_fd, chunk_size, true, false)) return -1;
    }
  }

  close(results_fd);

  std::cout << "Sequential read benchmark was a success!\n";

  return 0;
}

int seq_write_benchmark() {
  int results_fd = open(seq_write_bench, O_CREAT | O_WRONLY, 777);
  if (results_fd < 0) {
    perror("open");
    return -1;
  }

  if (write(results_fd, csv_header, csv_header_len) <= 0) {
    perror("write");
    return -1;
  }

  for (size_t chunk_size = INCREMENTAL_START; 
    chunk_size < LARGEST_CHUNK; 
    chunk_size += INCREMENTAL)
  {
    if (chunk_size == (LARGEST_CHUNK - INCREMENTAL)) {
      if (seq_chunk_bench(results_fd, chunk_size, false, true)) return -1;
    } else {
      if (seq_chunk_bench(results_fd, chunk_size, false, false)) return -1;
    }
  }

  close(results_fd);

  std::cout << "Sequential write benchmark was a success!\n";

  return 0;
}

int rng_chunk_bench(int results_fd, size_t chunk_size,
  bool read_direction, bool dump_file) {
  unsigned char *chunk = allocate_file_buffer();

  if (chunk == nullptr) {
    std::cerr << "Failed to allocate rng chunk!\n";
    return -1;
  }

  if (not read_direction) {
    read_file(chunk);
  }

  size_t chunks = FILESIZE / chunk_size;
  if ((FILESIZE % chunk_size) != 0)
    ++chunks;

  std::vector<size_t> chunk_order_to_process;
  chunk_order_to_process.reserve(chunks);
  for (size_t i = 0; i < chunks; ++i) {
    chunk_order_to_process.push_back(i);
  }
  std::mt19937 gen(42);
  std::shuffle(chunk_order_to_process.begin(), chunk_order_to_process.end(), gen);

  for (int run = 0; run < 30; ++run) {
    int test_fd;
    if (read_direction) {
      test_fd = open(testfile, O_RDONLY | O_DIRECT);
    } else {
      test_fd = open(rng_write_bench_copy, O_CREAT | O_WRONLY | O_DIRECT, 777);
    }

    if (test_fd < 0) {
      perror("open");
      std::cerr << "Failed to open benchmark file\n";
      return -1;
    }

    if (io_uring_register_files(&ring, &test_fd, 1)) {
      std::cerr << "Failed to register filedes with io uring!\n";
      return -1;
    }

    auto start = std::chrono::high_resolution_clock::now();

    for (size_t chunk_idx: chunk_order_to_process) {
      size_t offset = chunk_idx * chunk_size;

      size_t to_process = FILESIZE - offset;
      if (to_process > chunk_size)
        to_process = chunk_size;

      struct io_uring_sqe *sqe;
      while((sqe = io_uring_get_sqe(&ring)) == nullptr);

      ssize_t newly_processed_size;
      if (read_direction) {
        io_uring_prep_read(sqe, 0, &chunk[offset], to_process, offset);
      } else {
        io_uring_prep_write(sqe, 0, &chunk[offset], to_process, offset);
      }
      sqe->flags |= IOSQE_FIXED_FILE;

      io_uring_submit(&ring);

      struct io_uring_cqe *cqe;
      while(io_uring_peek_cqe(&ring, &cqe));

      std::atomic_thread_fence(std::memory_order_seq_cst);
      newly_processed_size = cqe->res;

      io_uring_cqe_seen(&ring, cqe);

      if (newly_processed_size < to_process) {
        std::cerr << "Processed " << newly_processed_size << " bytes but needed " << to_process << "\n";
        return -1;
      }
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> start_stop_diff = end - start;

    double time_ms = start_stop_diff.count() * 1000.0;
    double throughput_mbps = (FILESIZE / (1024.0 * 1024.0)) / start_stop_diff.count();

#ifdef DEBUG
    std::cout << "Random processing time is " << time_ms << "ms\n";
#endif

    struct BenchmarkResult result = {chunk_size, run + 1, time_ms, throughput_mbps};
    write_result(results_fd, result);
    
    if (io_uring_unregister_files(&ring)) {
      std::cerr << "Failed to unregister files with io uring!\n";
      return -1;
    }

#ifdef VERIFY
    if ((run == 0) && dump_file && read_direction) {
      dump_file_copy(rng_read_bench_copy, chunk);
    }
#endif

    close(test_fd);
    
    if (not read_direction) {
#ifdef VERIFY
      if (not dump_file || (run != 29)) {
#endif
        unlink(rng_write_bench_copy);
        std::cerr << "Unlinking random write\n";
#ifdef VERIFY
      }
#endif
    }
  }

  std::free(chunk);
  return 0;
}

int rng_read_benchmark() {
  int results_fd = open(rng_read_bench, O_CREAT | O_WRONLY, 777);
  if (results_fd < 0) {
    perror("open");
    return -1;
  }

  if (write(results_fd, csv_header, csv_header_len) <= 0) {
    perror("write");
    return -1;
  }

  for (size_t chunk_size = INCREMENTAL_START; 
    chunk_size < LARGEST_CHUNK; 
    chunk_size += INCREMENTAL)
  {
    if (chunk_size == (LARGEST_CHUNK - INCREMENTAL)) {
      if (rng_chunk_bench(results_fd, chunk_size, true, true)) return -1;
    } else {
      if (rng_chunk_bench(results_fd, chunk_size, true, false)) return -1;
    }
  }

  close(results_fd);

  std::cout << "Random read benchmark was a success!\n";

  return 0;
}

int rng_write_benchmark() {
  int results_fd = open(rng_write_bench, O_CREAT | O_RDWR, 777);
  if (results_fd < 0) {
    perror("open");
    return -1;
  }

  if (write(results_fd, csv_header, csv_header_len) <= 0) {
    perror("write");
    return -1;
  }

  for (size_t chunk_size = INCREMENTAL_START; 
    chunk_size < LARGEST_CHUNK; 
    chunk_size += INCREMENTAL)
  {
    if (chunk_size == (LARGEST_CHUNK - INCREMENTAL)) {
      if (rng_chunk_bench(results_fd, chunk_size, false, true)) return -1;
    } else {
      if (rng_chunk_bench(results_fd, chunk_size, false, false)) return -1;
    }
  }

  close(results_fd);

  std::cout << "Random write benchmark was a success!\n";

  return 0;
}
