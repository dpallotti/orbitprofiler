#include <unistd.h>

#include <csignal>
#include <thread>

#include "OrbitBase/Logging.h"
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
  std::array<char, 128> message;
};

static moodycamel::ConcurrentQueue<Message> queue;

static std::atomic<bool> write_data = false;

void WriterMain(size_t index) {
  size_t message_count = 0;
  while (!exit_requested) {
    Message message;
    snprintf(message.message.data(), 128, "message %lu from writer %lu", message_count, index);
    if (write_data) {
      queue.enqueue(message);
    }
    ++message_count;
    usleep(10'000);
  }
}

void ForwarderMain() {
  std::string server_address = absl::StrFormat("unix://%s", kSocketPath);
  std::shared_ptr<grpc::Channel> channel = grpc::CreateCustomChannel(
      server_address, grpc::InsecureChannelCredentials(), grpc::ChannelArguments());

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

  while (!exit_requested) {
    Message message;
    while (!exit_requested && queue.try_dequeue(message)) {
      LOG("Read \"%s\"", message.message.data());

      {
        my_service::Message proto_message;
        proto_message.set_message(message.message.data());
        grpc::ClientContext send_message_context;
        google::protobuf::Empty send_message_empty_response;
        grpc::Status status =
            stub->SendMessage(&send_message_context, proto_message, &send_message_empty_response);
        if (!status.ok()) {
          ERROR("SendMessage: %s", status.error_message());
        }
      }
    }

    LOG("Sleep");
    usleep(200'000);
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
