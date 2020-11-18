// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <unistd.h>

#include <csignal>
#include <thread>

#include "CaptureEventProducer/LockFreeBufferCaptureEventProducer.h"
#include "OrbitBase/Logging.h"
#include "OrbitBase/SafeStrerror.h"
#include "OrbitService/ProducerSideUnixDomainSocketPath.h"
#include <grpcpp/impl/grpc_library.h>

static std::atomic<bool> exit_requested;

static void SigintHandler(int signum) {
  if (signum == SIGINT) {
    exit_requested = true;
  }
}

struct Message {
  static constexpr size_t kMaxMessageLength = 128;

  std::array<char, kMaxMessageLength> message;
};

class Producer : public orbit_producer::LockFreeBufferCaptureEventProducer<Message> {
 public:
  orbit_grpc_protos::CaptureEvent TranslateIntermediateEvent(
      Message&& intermediate_event) override {
    orbit_grpc_protos::CaptureEvent capture_event;
    capture_event.mutable_gpu_queue_submission()->set_placeholder(
        intermediate_event.message.data());
    return capture_event;
  }
};

grpc::internal::GrpcLibraryInitializer init;

class ProducerHolder {
 public:
  ProducerHolder() {
    producer_.emplace();
    if (!producer_->BringUp(orbit_service::kProducerSideUnixDomainSocketPath)) {
      producer_.reset();
      ERROR("Bringing up VulkanLayerProducer");
    }
  }

  virtual ~ProducerHolder() {
    if (producer_.has_value()) {
      producer_->TakeDown();
    }
  }

  [[nodiscard]] bool HasProducer() { return producer_.has_value(); }

  [[nodiscard]] Producer* GetProducer() {
    if (!producer_.has_value()) {
      return nullptr;
    }
    return &producer_.value();
  }

 private:
  std::optional<Producer> producer_;
} holder;

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
    holder.GetProducer()->EnqueueIntermediateEventIfCapturing([&message] { return message; });
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

int main() {
  std::signal(SIGINT, SigintHandler);

  std::vector<std::thread> writers;
  constexpr size_t kWriterCount = 4;
  for (size_t i = 0; i < kWriterCount; ++i) {
    writers.emplace_back(&WriterMain, i);
  }

  if (holder.HasProducer()) {
    while (!exit_requested) {
      std::this_thread::sleep_for(std::chrono::seconds{1});
    }
  }

  for (std::thread& writer : writers) {
    writer.join();
  }
  return EXIT_SUCCESS;
}
