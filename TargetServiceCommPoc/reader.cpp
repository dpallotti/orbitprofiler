#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <thread>

#include "OrbitBase/Logging.h"
#include "OrbitBase/SafeStrerror.h"

static std::atomic<bool> exit_requested;

static void SigintHandler(int signum) {
  if (signum == SIGINT) {
    exit_requested = true;
  }
}

static void InstallSigintHandler() {
  struct sigaction act {};
  act.sa_handler = SigintHandler;
  sigemptyset(&act.sa_mask);
  act.sa_flags = 0;
  act.sa_restorer = nullptr;
  sigaction(SIGINT, &act, nullptr);
}

void ReaderMain(int fd) {
  std::array<char, 1025> buffer;
  while (!exit_requested) {
    ssize_t read_count = read(fd, buffer.data(), buffer.size() - 1);
    if (read_count == -1) {
      ERROR("read: %s", SafeStrerror(errno));
      break;
    }
    if (read_count == 0) {
      ERROR("read: read_count=0");
      break;
    }

    buffer.at(read_count) = '\0';
    LOG("from %d: %s", fd, buffer.data());
  }
  close(fd);
}

int main() {
  InstallSigintHandler();

  // https://www.man7.org/linux/man-pages/man7/unix.7.html
  int socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (socket_fd == -1) {
    ERROR("socket: %s", SafeStrerror(errno));
    exit(EXIT_FAILURE);
  }
  LOG("socket: socket_fd=%d", socket_fd);

  constexpr const char* kSocketPath = "/tmp/TargetServiceCommPocUnixSocket";
  if (remove(kSocketPath) != 0 && errno != ENOENT) {
    ERROR("remove(%s): %s", kSocketPath, SafeStrerror(errno));
    exit(EXIT_FAILURE);
  }

  sockaddr_un addr;
  memset(&addr, 0, sizeof(sockaddr_un));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, kSocketPath, sizeof(addr.sun_path) - 1);

  if (bind(socket_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(sockaddr_un)) != 0) {
    ERROR("bind: %s", SafeStrerror(errno));
    exit(EXIT_FAILURE);
  }
  LOG("bind");

  if (listen(socket_fd, 1) != 0) {
    ERROR("listen: %s", SafeStrerror(errno));
    exit(EXIT_FAILURE);
  }
  LOG("listen");

  std::vector<std::thread> threads;

  while (!exit_requested) {
    int fd = accept(socket_fd, nullptr, nullptr);
    if (fd == -1) {
      ERROR("accept: %s", SafeStrerror(errno));
      exit(EXIT_FAILURE);
    }
    LOG("accept: fd=%d", fd);

    threads.emplace_back(&ReaderMain, fd);
  }

  for (std::thread& thread : threads) {
    thread.join();
  }
}
