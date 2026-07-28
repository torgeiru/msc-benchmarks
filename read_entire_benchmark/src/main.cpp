#ifdef UNIKERNEL
#include <os>
#endif

#define DEBUG
#define VERIFY

#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

constexpr size_t FILESIZE = 214748364;
constexpr size_t DIRECT_IO_ALIGNMENT = 4096;
constexpr int RUNS = 30;

const char *testfile = "VirtioFS0/syncio_testing_file.bin";
const char *results_file = "VirtioFS0/whole_file_read_bench.csv";
const char *verification_copy = "VirtioFS0/whole_file_read_copy.bin";
const char *csv_header = "file_size,run_number,time_ms,throughput_mibps\n";

bool write_all(int fd, const void *buffer, size_t count) {
  const auto *bytes = static_cast<const unsigned char *>(buffer);
  size_t progress = 0;

  while (progress < count) {
    const ssize_t written = write(fd, bytes + progress, count - progress);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      perror("write");
      return false;
    }
    progress += static_cast<size_t>(written);
  }

  return true;
}

bool write_result(int fd, int run, double time_ms, double throughput_mibps) {
  const std::string row = std::format("{},{},{:.3f},{:.3f}\n",
    FILESIZE, run, time_ms, throughput_mibps);
  return write_all(fd, row.data(), row.size());
}

unsigned char *allocate_file_buffer() {
  void *buffer = nullptr;
  if (posix_memalign(&buffer, DIRECT_IO_ALIGNMENT, FILESIZE) != 0) {
    return nullptr;
  }

  std::memset(buffer, 0, FILESIZE);
  return static_cast<unsigned char*>(buffer);
}

#ifdef VERIFY
bool dump_verification_copy(const unsigned char *buffer) {
  const int fd = open(verification_copy, O_CREAT | O_TRUNC | O_WRONLY, 0666);
  if (fd < 0) {
    perror("open verification copy");
    return false;
  }

  const bool ok = write_all(fd, buffer, FILESIZE);
  if (close(fd) < 0) {
    perror("close verification copy");
    return false;
  }
  return ok;
}
#endif

int benchmark() {
  auto *buffer = allocate_file_buffer();
  if (buffer == nullptr) {
    std::cerr << "Failed to allocate the whole-file buffer\n";
    return -1;
  }

  const int results_fd =
    open(results_file, O_CREAT | O_TRUNC | O_WRONLY, 0666);
  if (results_fd < 0) {
    perror("open results");
    std::free(buffer);
    return -1;
  }

  if (!write_all(results_fd, csv_header, std::strlen(csv_header))) {
    close(results_fd);
    std::free(buffer);
    return -1;
  }

  for (int run = 1; run <= RUNS; ++run) {
    const int read_fd = open(testfile, O_RDONLY | O_DIRECT);
    if (read_fd < 0) {
      perror("open benchmark input");
      close(results_fd);
      std::free(buffer);
      return -1;
    }

    const auto start = std::chrono::high_resolution_clock::now();
    const ssize_t bytes_read = read(read_fd, buffer, FILESIZE);
    const auto end = std::chrono::high_resolution_clock::now();

    if (bytes_read < 0) {
      perror("read");
      close(read_fd);
      close(results_fd);
      std::free(buffer);
      return -1;
    }
    if (static_cast<size_t>(bytes_read) != FILESIZE) {
      std::cerr << "The single read returned " << bytes_read
                << " bytes; expected " << FILESIZE << '\n';
      close(read_fd);
      close(results_fd);
      std::free(buffer);
      return -1;
    }

    if (close(read_fd) < 0) {
      perror("close benchmark input");
      close(results_fd);
      std::free(buffer);
      return -1;
    }

    const std::chrono::duration<double> elapsed = end - start;
    const double time_ms = elapsed.count() * 1000.0;
    const double throughput_mibps =
      (1000*FILESIZE / (1024.0 * 1024.0)) / (elapsed.count()*1000);

#ifdef DEBUG
    std::cout << "Run " << run << ": " << time_ms << " ms, "
              << throughput_mibps << " MiB/s\n";
#endif

    if (!write_result(results_fd, run, time_ms, throughput_mibps)) {
      close(results_fd);
      std::free(buffer);
      return -1;
    }

#ifdef VERIFY
    if (run == 1 && !dump_verification_copy(buffer)) {
      close(results_fd);
      std::free(buffer);
      return -1;
    }
#endif
  }

  if (close(results_fd) < 0) {
    perror("close results");
    std::free(buffer);
    return -1;
  }

  std::free(buffer);
  std::cout << "Whole-file read benchmark was a success!\n";
  return 0;
}

} // namespace

int main() {
  const int status = benchmark();

#ifdef UNIKERNEL
  os::shutdown();
#else
  return status == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
#endif
}
