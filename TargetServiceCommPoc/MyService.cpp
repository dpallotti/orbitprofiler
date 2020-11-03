#include <grpcpp/server_builder.h>

#include <atomic>
#include <csignal>
#include <random>
#include <thread>

#include "OrbitBase/Logging.h"
#include "SocketPath.h"
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

class MyServiceImpl final : public my_service::MyService::Service {
 public:
  grpc::Status ReceiveCommands(::grpc::ServerContext* /*context*/,
                               const ::google::protobuf::Empty* /*request*/,
                               ::grpc::ServerWriter< ::my_service::Command>* writer) override {
    LOG("ReceiveCommands");
    std::this_thread::sleep_for(std::chrono::seconds{15});
    while (!exit_requested) {
      my_service::Command command;
      command.mutable_start_command();
      if (!writer->Write(command)) {
        ERROR("writer->Write(command): StartCommand");
        return grpc::Status::OK;
      }
      LOG("writer->Write(command): StartCommand");
      std::this_thread::sleep_for(std::chrono::seconds{15});

      command.mutable_stop_command();
      if (!writer->Write(command)) {
        ERROR("writer->Write(command): StopCommand");
        return grpc::Status::OK;
      }
      LOG("writer->Write(command): StopCommand");
      std::this_thread::sleep_for(std::chrono::seconds{15});
    }
    return grpc::Status::OK;
  }

  grpc::Status SendMessages(::grpc::ServerContext* /*context*/,
                            const ::my_service::BufferedMessages* request,
                            ::google::protobuf::Empty* /*response*/) override {
    LOG("Received %lu messages. First: %s", request->messages_size(),
        request->messages(0).message());
    return grpc::Status::OK;
  }
};

int main() {
  InstallSigintHandler();

  grpc::ServerBuilder builder;

  std::string server_address = absl::StrFormat("unix://%s", kSocketPath);
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());

  MyServiceImpl my_service;
  builder.RegisterService(&my_service);

  std::unique_ptr<grpc::Server> grpc_server = builder.BuildAndStart();
  if (grpc_server == nullptr) {
    ERROR("grpc_server == nullptr");
    exit(EXIT_FAILURE);
  }
  LOG("gRPC server is running");

  while (!exit_requested) {
    std::this_thread::sleep_for(std::chrono::seconds{1});
  }
  grpc_server->Shutdown();
  grpc_server->Wait();
  return EXIT_SUCCESS;
}
