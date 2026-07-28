#ifdef UNIKERNEL
#include <os>
#endif

#define DEBUG
#define VERIFY

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
  if (seq_read_benchmark()) goto end_bench;
  if (rng_read_benchmark()) goto end_bench;
  if (seq_write_benchmark()) goto end_bench;
  if (rng_write_benchmark()) goto end_bench;

  std::cout << "Running the benchmark suite was a success!\n";

end_bench:
#ifdef UNIKERNEL
  os::shutdown();
#else
  return 0;
#endif
}

struct BenchmarkResult {
  size_t chunk_size;
  int run_number;
  double time_ms;
  double throughput_mibps;
};

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

void write_result(int results_fd, const struct BenchmarkResult& result) {
  std::string results_str  = std::format("{},{},{:.3f},{:.3f}\n",
    result.chunk_size,
    result.run_number,
    result.time_ms,
    result.throughput_mibps
  );

  if (!write_all(results_fd, results_str.data(), results_str.size())) {
    std::cerr << "Error writing result to file\n";
    std::exit(EXIT_FAILURE);
  }
}

void dump_file_copy(const char *filename, unsigned char *filebuf) {
  int dump_fd = open(filename, O_CREAT | O_TRUNC | O_WRONLY, 0666);
  if (dump_fd < 0) {
    perror("open");
    std::cerr << "Failed to open/create file to be dumped!\n";
    return;
  }

  size_t progress = 0;
  while(progress < FILESIZE) {
    size_t to_write = FILESIZE - progress;
    if (to_write > LARGEST_CHUNK) {
      to_write = LARGEST_CHUNK;
    }

    if (!write_all(dump_fd, &filebuf[progress], to_write)) {
      std::cerr << "Failed to dump any bytes in this write!\n";
      close(dump_fd);
      return;
    }

    progress += to_write;
  }

  close(dump_fd);
}

void read_file(unsigned char *filebuf) {
  int read_fd = open(testfile, O_RDONLY);
  if (read_fd < 0) {
    perror("open");
    std::cerr << "Failed to open read file!\n";
    return;
  }

  if (!read_all(read_fd, filebuf, FILESIZE)) {
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
      test_fd = open(seq_write_bench_copy, O_CREAT | O_TRUNC | O_WRONLY | O_DIRECT, 0666);
    }
 
    if (test_fd < 0) {
      perror("open");
      std::cerr << "Failed to open benchmark file\n";
      return -1;
    }

    auto start = std::chrono::high_resolution_clock::now();

    size_t cur_processed_bytes = 0;
    while (cur_processed_bytes < FILESIZE) {
      size_t left_to_process = FILESIZE - cur_processed_bytes;
      size_t to_process = left_to_process > chunk_size ? chunk_size : left_to_process;

      bool processed = read_direction
        ? read_all(test_fd, &chunk[cur_processed_bytes], to_process)
        : write_all(test_fd, &chunk[cur_processed_bytes], to_process);

      if (!processed) {
        std::cerr << "Processed less than the requested " << to_process << " bytes\n";
        return -1;
      }
      cur_processed_bytes += to_process;
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> start_stop_diff = end - start;

    double time_ms = start_stop_diff.count() * 1000.0;
    double throughput_mibps = (FILESIZE / (1024.0 * 1024.0)) / start_stop_diff.count();

#ifdef DEBUG
    std::cout << "Sequential processing time is " << time_ms << "ms\n";
#endif

    struct BenchmarkResult result = {chunk_size, run + 1, time_ms, throughput_mibps};
    write_result(results_fd, result);

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
  int results_fd = open(seq_read_bench, O_CREAT | O_TRUNC | O_WRONLY, 0666);
  if (results_fd < 0) {
    perror("open");
    return -1;
  }

  if (!write_all(results_fd, csv_header, csv_header_len)) {
    perror("write");
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
  int results_fd = open(seq_write_bench, O_CREAT | O_TRUNC | O_WRONLY, 0666);
  if (results_fd < 0) {
    perror("open");
    return -1;
  }

  if (!write_all(results_fd, csv_header, csv_header_len)) {
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
      test_fd = open(rng_write_bench_copy, O_CREAT | O_TRUNC | O_WRONLY | O_DIRECT, 0666);
    }

    if (test_fd < 0) {
      perror("open");
      std::cerr << "Failed to open benchmark file\n";
      return -1;
    }

    auto start = std::chrono::high_resolution_clock::now();

    for (size_t chunk_idx: chunk_order_to_process) {
      size_t offset = chunk_idx * chunk_size;

      lseek(test_fd, offset, SEEK_SET);
      size_t to_process = FILESIZE - offset;
      if (to_process > chunk_size)
        to_process = chunk_size;

      bool processed = read_direction
        ? read_all(test_fd, &chunk[offset], to_process)
        : write_all(test_fd, &chunk[offset], to_process);

      if (!processed) {
        std::cerr << "Processed less bytes than expected!\n";
        return -1;
      }
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> start_stop_diff = end - start;

    double time_ms = start_stop_diff.count() * 1000.0;
    double throughput_mibps = (FILESIZE / (1024.0 * 1024.0)) / start_stop_diff.count();

#ifdef DEBUG
    std::cout << "Random processing time is " << time_ms << "ms\n";
#endif

    struct BenchmarkResult result = {chunk_size, run + 1, time_ms, throughput_mibps};
    write_result(results_fd, result);

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
#ifdef VERIFY
      }
#endif
    }
  }

  std::free(chunk);
  return 0;
}

int rng_read_benchmark() {
  int results_fd = open(rng_read_bench, O_CREAT | O_TRUNC | O_WRONLY, 0666);
  if (results_fd < 0) {
    perror("open");
    return -1;
  }

  if (!write_all(results_fd, csv_header, csv_header_len)) {
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
  int results_fd = open(rng_write_bench, O_CREAT | O_TRUNC | O_RDWR, 0666);
  if (results_fd < 0) {
    perror("open");
    return -1;
  }
  
  if (!write_all(results_fd, csv_header, csv_header_len)) {
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
