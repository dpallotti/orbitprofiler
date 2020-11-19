// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <google/protobuf/arena.h>
#include <unistd.h>

#include <csignal>
#include <thread>

#include "OrbitBase/Logging.h"
#include "OrbitBase/Profiling.h"
#include "OrbitBase/SafeStrerror.h"
#include "OrbitProducer/LockFreeBufferCaptureEventProducer.h"
#include "ProducerSideChannel/ProducerSideChannel.h"

static std::atomic<bool> exit_requested;

#define FILL_FIXED_FAKE_EVENT 0
#define MOVE_FIXED_FAKE_EVENT 0
#define FILL_BYTES_FAKE_EVENT 0
#define MOVE_BYTES_FAKE_EVENT 0
#define MOVE_STRING_INTO_BYTES_FAKE_EVENT 0

#if FILL_FIXED_FAKE_EVENT
struct Message {
  uint64_t field1, field2, field3, field4, field5, field6;
};
#elif MOVE_FIXED_FAKE_EVENT
using Message = orbit_grpc_protos::FixedFakeEvent;
#elif FILL_BYTES_FAKE_EVENT
struct Message {
  std::array<uint64_t, 6> bytes;
};
#elif MOVE_BYTES_FAKE_EVENT
using Message = orbit_grpc_protos::BytesFakeEvent;
#elif MOVE_STRING_INTO_BYTES_FAKE_EVENT
struct Message {
  std::string bytes;
};
#else
struct Message {
  pid_t tid;
  uint64_t start_timestamp_ns;
  uint64_t end_timestamp_ns;
};
#endif

class Producer : public orbit_producer::LockFreeBufferCaptureEventProducer<Message> {
 public:
  orbit_grpc_protos::ProducerCaptureEvent* TranslateIntermediateEvent(
      Message&& intermediate_event, google::protobuf::Arena* arena) override {
    auto* capture_event =
        google::protobuf::Arena::CreateMessage<orbit_grpc_protos::ProducerCaptureEvent>(arena);
#if FILL_FIXED_FAKE_EVENT
    orbit_grpc_protos::FixedFakeEvent* event = capture_event->mutable_fixed_fake_event();
    event->set_field1(intermediate_event.field1);
    event->set_field2(intermediate_event.field2);
    event->set_field3(intermediate_event.field3);
    event->set_field4(intermediate_event.field4);
    event->set_field5(intermediate_event.field5);
    event->set_field6(intermediate_event.field6);
#elif MOVE_FIXED_FAKE_EVENT
    *capture_event->mutable_fixed_fake_event() = std::move(intermediate_event);
#elif FILL_BYTES_FAKE_EVENT
    orbit_grpc_protos::BytesFakeEvent* event = capture_event->mutable_bytes_fake_event();
    event->set_bytes(intermediate_event.bytes.data(), sizeof(uint64_t) * 6);
#elif MOVE_BYTES_FAKE_EVENT
    *capture_event->mutable_bytes_fake_event() = std::move(intermediate_event);
#elif MOVE_STRING_INTO_BYTES_FAKE_EVENT
    orbit_grpc_protos::BytesFakeEvent* event = capture_event->mutable_bytes_fake_event();
    event->set_bytes(std::move(intermediate_event.bytes));
#else
    orbit_grpc_protos::SchedulingSlice* slice = capture_event->mutable_scheduling_slice();
    slice->set_pid(1'000'000'000 + intermediate_event.tid);
    slice->set_tid(1'000'000'000 + intermediate_event.tid);
    slice->set_core(15 + intermediate_event.tid);
    slice->set_duration_ns(intermediate_event.end_timestamp_ns -
                           intermediate_event.start_timestamp_ns);
    slice->set_out_timestamp_ns(intermediate_event.end_timestamp_ns);
#endif
    return capture_event;
  }

 protected:
  void OnCaptureStart(orbit_grpc_protos::CaptureOptions capture_options) override {
    LOG("Producer::OnCaptureStart");
    LockFreeBufferCaptureEventProducer::OnCaptureStart(capture_options);
  }

  void OnCaptureStop() override {
    LockFreeBufferCaptureEventProducer::OnCaptureStop();
    LOG("Producer::OnCaptureStop");
  }
} producer;

#define BUSY_SLEEP 1

__attribute__((noinline)) void EveryMicro(size_t thread_index) {
  (void)thread_index;

  uint64_t start_timestamp_ns = orbit_base::CaptureTimestampNs();
  (void)start_timestamp_ns;
#if BUSY_SLEEP
  uint64_t timestamp_ns = orbit_base::CaptureTimestampNs();
  while (timestamp_ns - start_timestamp_ns < 1000) {
    timestamp_ns = orbit_base::CaptureTimestampNs();
  }
#else
  constexpr size_t kN = 52;
  double result = 0;
  for (size_t i = 0; i < kN; ++i) {
    // Complex enough to prevent clever optimizations with -O>=1.
    double x = i;
    result += sin(x) * cos(x) * tan(x) * exp(x);
  }
#endif
  uint64_t end_timestamp_ns = orbit_base::CaptureTimestampNs();
  (void)end_timestamp_ns;

  // --------

#if FILL_FIXED_FAKE_EVENT
  Message message;
  auto event_builder = [message = message] { return message; };
#elif MOVE_FIXED_FAKE_EVENT
  uint64_t bytes[6];
  orbit_grpc_protos::FixedFakeEvent event;
  event.set_field1(bytes[0]);
  event.set_field2(bytes[1]);
  event.set_field3(bytes[2]);
  event.set_field4(bytes[3]);
  event.set_field5(bytes[4]);
  event.set_field6(bytes[5]);
  auto event_builder = [message = std::move(event)]() mutable { return message; };
#elif FILL_BYTES_FAKE_EVENT
  Message message;
  auto event_builder = [message = message] { return message; };
#elif MOVE_BYTES_FAKE_EVENT
  uint64_t bytes[6];
  orbit_grpc_protos::BytesFakeEvent event;
  event.set_bytes(bytes, sizeof(uint64_t) * 6);
  auto event_builder = [message = std::move(event)]() mutable { return std::move(message); };
#elif MOVE_STRING_INTO_BYTES_FAKE_EVENT
  uint64_t bytes[6];
  Message message{};
  message.bytes.resize(sizeof(uint64_t) * 6);
  memcpy(message.bytes.data(), bytes, sizeof(uint64_t) * 6);
  auto event_builder = [message = std::move(message)]() mutable { return std::move(message); };
#else
  auto event_builder = [thread_index, start_timestamp_ns, end_timestamp_ns] {
    return Message{.tid = static_cast<pid_t>(thread_index),
                   .start_timestamp_ns = start_timestamp_ns,
                   .end_timestamp_ns = end_timestamp_ns};
  };
#endif

  producer.EnqueueIntermediateEventIfCapturing(event_builder);
}

void EverySecond(size_t thread_index) {
  for (int i = 0; i < 1'000'000; ++i) {
    EveryMicro(thread_index);
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
  while (!exit_requested) {
    auto start = std::chrono::steady_clock::now();
    EverySecond(thread_index);
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

static void SigintHandler(int signum) {
  if (signum == SIGINT) {
    exit_requested = true;
  }
}

int main() {
  std::signal(SIGINT, SigintHandler);
  LOG("PID: %d", getpid());

  std::vector<std::thread> writers;
#if BUSY_SLEEP
  constexpr size_t kWriterCount = 1;
#else
  constexpr size_t kWriterCount = 3;
#endif
  for (size_t i = 0; i < kWriterCount; ++i) {
    writers.emplace_back(&WriterMain, i);
  }

  producer.BuildAndStart(orbit_producer_side_channel::CreateProducerSideChannel());
  while (!exit_requested) {
    std::this_thread::sleep_for(std::chrono::seconds{1});
  }
  producer.ShutdownAndWait();

  for (std::thread& writer : writers) {
    writer.join();
  }
  return EXIT_SUCCESS;
}
