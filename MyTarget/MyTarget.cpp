// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <unistd.h>

#include <csignal>
#include <thread>

#include "OrbitBase/Logging.h"
#include "OrbitBase/SafeStrerror.h"
#include "OrbitService/ProducerSideUnixDomainSocketPath.h"
#include "concurrentqueue.h"
#include "grpcpp/grpcpp.h"
#include "producer_side_services.grpc.pb.h"

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
  for (size_t i = 0; i < kN; ++i) {
    // Complex enough to prevent clever optimizations with -O>=1.
    double x = i;
    result += sin(x) * cos(x) * tan(x) * exp(x);
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
    CPU_SET(thread_index + 1, &cpu_set);
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

using orbit_grpc_protos::CaptureEvent;
using orbit_grpc_protos::GpuQueueSubmission;
using orbit_grpc_protos::ReceiveCommandsRequest;
using orbit_grpc_protos::ReceiveCommandsResponse;
using orbit_grpc_protos::SendCaptureEventsRequest;
using orbit_grpc_protos::SendCaptureEventsResponse;
using orbit_grpc_protos::ProducerSideService;

void ForwarderMain() {
  std::string server_address =
      absl::StrFormat("unix://%s", orbit_service::kProducerSideUnixDomainSocketPath);
  std::shared_ptr<grpc::Channel> channel = grpc::CreateCustomChannel(
      server_address, grpc::InsecureChannelCredentials(), grpc::ChannelArguments{});
  if (channel == nullptr) {
    ERROR("channel == nullptr");
    return;
  }
  std::unique_ptr<ProducerSideService::Stub> stub = ProducerSideService::NewStub(channel);
  if (stub == nullptr) {
    ERROR("stub == nullptr");
    return;
  }

  std::atomic<grpc::ClientContext*> cancellable_receive_commands_context = nullptr;
  std::thread command_receiver{[&stub, &cancellable_receive_commands_context] {
    while (!exit_requested) {
      grpc::ClientContext receive_commands_context;
      cancellable_receive_commands_context = &receive_commands_context;
      ReceiveCommandsRequest receive_commands_request;
      std::unique_ptr<grpc::ClientReader<ReceiveCommandsResponse>> reader =
          stub->ReceiveCommands(&receive_commands_context, receive_commands_request);
      if (reader == nullptr) {
        ERROR("reader == nullptr");
        std::this_thread::sleep_for(std::chrono::seconds{1});
        continue;
      }

      ReceiveCommandsResponse command;
      while (!exit_requested) {
        if (!reader->Read(&command)) {
          ERROR("!reader->Read(&command)");
          reader->Finish();
          std::this_thread::sleep_for(std::chrono::seconds{1});
          break;
        }
        switch (command.command_case()) {
          case ReceiveCommandsResponse::kCheckAliveCommand:
            LOG("reader->Read(&command): kCheckAliveCommand");
            break;
          case ReceiveCommandsResponse::kStartCaptureCommand:
            LOG("reader->Read(&command): kStartCaptureCommand");
            write_data = true;
            break;
          case ReceiveCommandsResponse::kStopCaptureCommand:
            LOG("reader->Read(&command): kStopCaptureCommand");
            write_data = false;
            break;
          case ReceiveCommandsResponse::COMMAND_NOT_SET:
            ERROR("reader->Read(&command): COMMAND_NOT_SET");
            break;
        }
      }
      write_data = false;
    }
  }};

  constexpr uint64_t kMaxMessagesPerRequest = 75'000;
  std::vector<Message> messages;
  messages.resize(kMaxMessagesPerRequest);
  while (!exit_requested) {
    size_t dequeued_message_count;
    while ((dequeued_message_count =
                queue.try_dequeue_bulk(messages.begin(), kMaxMessagesPerRequest)) > 0) {
      SendCaptureEventsRequest buffered_messages;
      buffered_messages.mutable_capture_events()->Reserve(dequeued_message_count);
      for (size_t i = 0; i < dequeued_message_count; ++i) {
        CaptureEvent* capture_event = buffered_messages.mutable_capture_events()->Add();
        GpuQueueSubmission* submission = capture_event->mutable_gpu_queue_submission();
        submission->set_placeholder(messages.at(i).message.data());
      }

      grpc::ClientContext send_capture_events_context;
      SendCaptureEventsResponse send_capture_events_response;
      grpc::Status status = stub->SendCaptureEvents(&send_capture_events_context, buffered_messages,
                                                    &send_capture_events_response);
      if (!status.ok()) {
        ERROR("SendCaptureEvents: %s", status.error_message());
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
