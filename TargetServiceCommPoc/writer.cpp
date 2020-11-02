#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <csignal>
#include <thread>

#include "OrbitBase/Logging.h"
#include "OrbitBase/SafeStrerror.h"
#include "concurrentqueue.h"

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

struct Message {
  std::array<char, 128> message;
};

static moodycamel::ConcurrentQueue<Message> queue;

void WriterMain(size_t index) {
  size_t message_count = 0;
  while (!exit_requested) {
    Message message;
    snprintf(message.message.data(), 128, "message %lu from writer %lu", message_count, index);
    queue.enqueue(message);
    ++message_count;
    usleep(1000);
  }
}

void ForwarderMain() {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd == -1) {
    ERROR("socket: %s", SafeStrerror(errno));
    return;
  }
  LOG("socket: fd=%d", fd);

  constexpr const char* kSocketPath = "/tmp/TargetServiceCommPocUnixSocket";
  sockaddr_un addr;
  memset(&addr, 0, sizeof(sockaddr_un));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, kSocketPath, sizeof(addr.sun_path) - 1);

  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(sockaddr_un)) != 0) {
    ERROR("connect: %s", SafeStrerror(errno));
    return;
  }

  while (!exit_requested) {
    Message message;
    while (queue.try_dequeue(message)) {
      LOG("Read \"%s\"", message.message.data());
      write(fd, message.message.data(), strlen(message.message.data()));
    }
    LOG("Sleep");
    usleep(2000);
  }
}

int main() {
  InstallSigintHandler();

  std::vector<std::thread> writers;
  constexpr size_t kWriterCount = 4;
  for (size_t i = 0; i < kWriterCount; ++i) {
    writers.emplace_back(&WriterMain, i);
  }

  std::thread forwarder{&ForwarderMain};

  forwarder.join();
  for (std::thread& writer : writers) {
    writer.join();
  }
  return 0;
}
