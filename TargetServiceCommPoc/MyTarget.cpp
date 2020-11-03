#include <unistd.h>

#include <csignal>
#include <thread>

#include "OrbitBase/Logging.h"
#include "OrbitBase/SafeStrerror.h"
#include "SocketPath.h"
#include "concurrentqueue.h"
#include "grpcpp/grpcpp.h"
#include "service.grpc.pb.h"

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
  static constexpr size_t kMaxMessageLength = 128;

  std::array<char, kMaxMessageLength> message;
};

static moodycamel::ConcurrentQueue<Message> queue;

static std::atomic<bool> write_data = false;

static size_t kN = 19;

__attribute__((noinline)) void EveryMicro(size_t thread_index, size_t& message_count) {
  double result = 0;
  for (double i = 0; i < kN; ++i) {
    // Complex enough to prevent clever optimizations with -O>=1.
    result += sin(i) * cos(i) * tan(i) * exp(i);
  }
  {
    Message message;
    snprintf(message.message.data(), Message::kMaxMessageLength, "message %lu from writer %lu: %f",
             message_count, thread_index, result);
    if (write_data) {
      queue.enqueue(message);
    }
    ++message_count;
  }
}

void EverySecond(size_t thread_index, size_t& message_count) {
  for (int i = 0; i < 1'000'000; ++i) {
    EveryMicro(thread_index, message_count);
  }
}

void WriterMain(size_t thread_index) {
  {
    cpu_set_t cpu_set{};
    CPU_SET(thread_index, &cpu_set);
    if (sched_setaffinity(0, sizeof(cpu_set_t), &cpu_set) != 0) {
      ERROR("sched_setaffinity error: %s\n", SafeStrerror(errno));
    }
  }

  std::vector<double> totals{};
  size_t message_count = 0;
  while (!exit_requested) {
    auto start = std::chrono::steady_clock::now();
    EverySecond(thread_index, message_count);
    auto end = std::chrono::steady_clock::now();
    double total_ms =
        std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();

    totals.push_back(total_ms);
    constexpr int kAvgWindowCount = 10;
    double avg = 0;
    size_t avg_window = totals.size() < kAvgWindowCount ? totals.size() : kAvgWindowCount;
    for (size_t i = totals.size() - avg_window; i < totals.size(); ++i) {
      avg += totals[i];
    }
    avg /= avg_window;

    LOG("%1lu: %8.3f ms (avg last %2lu: %8.3f ms)", thread_index, total_ms, avg_window, avg);
  }
}

void ForwarderMain() {
  std::string server_address = absl::StrFormat("unix://%s", kSocketPath);
  grpc::ChannelArguments channel_arguments;
  std::shared_ptr<grpc::Channel> channel = grpc::CreateCustomChannel(
      server_address, grpc::InsecureChannelCredentials(), grpc::ChannelArguments{});
  if (channel == nullptr) {
    ERROR("channel == nullptr");
    return;
  }
  std::unique_ptr<my_service::MyService::Stub> stub = my_service::MyService::NewStub(channel);
  if (stub == nullptr) {
    ERROR("stub == nullptr");
    return;
  }

  std::atomic<grpc::ClientContext*> cancellable_receive_commands_context = nullptr;
  std::thread command_receiver{[&stub, &cancellable_receive_commands_context] {
    while (!exit_requested) {
      grpc::ClientContext receive_commands_context;
      cancellable_receive_commands_context = &receive_commands_context;
      google::protobuf::Empty receive_commands_empty_request;
      std::unique_ptr<grpc::ClientReader<my_service::Command>> reader =
          stub->ReceiveCommands(&receive_commands_context, receive_commands_empty_request);
      if (reader == nullptr) {
        ERROR("reader == nullptr");
        std::this_thread::sleep_for(std::chrono::seconds{1});
        continue;
      }

      my_service::Command command;
      while (!exit_requested) {
        if (!reader->Read(&command)) {
          ERROR("!reader->Read(&command)");
          reader->Finish();
          std::this_thread::sleep_for(std::chrono::seconds{1});
          break;
        }
        switch (command.command_case()) {
          case my_service::Command::kStartCommand:
            LOG("reader->Read(&command): StartCommand");
            write_data = true;
            break;
          case my_service::Command::kStopCommand:
            LOG("reader->Read(&command): StopCommand");
            write_data = false;
            break;
          case my_service::Command::COMMAND_NOT_SET:
            ERROR("reader->Read(&command): COMMAND_NOT_SET");
            break;
        }
      }
    }
  }};

  constexpr uint64_t kMaxMessagesPerRequest = 75'000;
  std::vector<Message> messages;
  messages.resize(kMaxMessagesPerRequest);
  while (!exit_requested) {
    size_t dequeued_message_count;
    while ((dequeued_message_count =
                queue.try_dequeue_bulk(messages.begin(), kMaxMessagesPerRequest)) > 0) {
      my_service::BufferedMessages buffered_messages;
      for (size_t i = 0; i < dequeued_message_count; ++i) {
        my_service::Message* proto_message = buffered_messages.mutable_messages()->Add();
        proto_message->set_message(messages.at(i).message.data());
      }

      grpc::ClientContext send_message_context;
      google::protobuf::Empty send_message_empty_response;
      grpc::Status status = stub->SendMessages(&send_message_context, buffered_messages,
                                               &send_message_empty_response);
      if (!status.ok()) {
        ERROR("SendMessages: %s", status.error_message());
        break;
      }
      if (dequeued_message_count < kMaxMessagesPerRequest) {
        break;
      }
    }

    usleep(100);
  }

  if (cancellable_receive_commands_context.load() != nullptr) {
    cancellable_receive_commands_context.load()->TryCancel();
  }
  command_receiver.join();
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
  return EXIT_SUCCESS;
}
