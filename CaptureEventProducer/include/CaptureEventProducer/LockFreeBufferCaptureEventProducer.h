// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CAPTURE_EVENT_PRODUCER_LOCK_FREE_BUFFER_CAPTURE_EVENT_PRODUCER_H_
#define CAPTURE_EVENT_PRODUCER_LOCK_FREE_BUFFER_CAPTURE_EVENT_PRODUCER_H_

#include "CaptureEventProducer/CaptureEventProducer.h"
#include "OrbitBase/Logging.h"
#include "concurrentqueue.h"

namespace orbit_producer {

template <typename IntermediateEventT>
class LockFreeBufferCaptureEventProducer : public CaptureEventProducer {
 public:
  [[nodiscard]] bool BringUp(std::string_view unix_domain_socket_path) {
    if (!CaptureEventProducer::ConnectAndStart(unix_domain_socket_path)) {
      return false;
    }

    forwarder_thread_ = std::thread{[this] { ForwarderThread(); }};
    return true;
  }

  void TakeDown() {
    take_down_requested_ = true;

    CHECK(forwarder_thread_.joinable());
    forwarder_thread_.join();

    CaptureEventProducer::ShutdownAndWait();
  }

  void EnqueueIntermediateEvent(const IntermediateEventT& event) {
    lock_free_queue_.enqueue(event);
  }

  void EnqueueIntermediateEvent(IntermediateEventT&& event) {
    lock_free_queue_.enqueue(std::move(event));
  }

  void EnqueueIntermediateEventIfCapturing(
      const std::function<IntermediateEventT()>& event_builder_if_capturing) {
    if (IsCapturing()) {
      lock_free_queue_.enqueue(event_builder_if_capturing());
    }
  }

  [[nodiscard]] virtual orbit_grpc_protos::CaptureEvent TranslateIntermediateEvent(
      IntermediateEventT&& intermediate_event) = 0;

 private:
  moodycamel::ConcurrentQueue<IntermediateEventT> lock_free_queue_;
  std::thread forwarder_thread_;
  std::atomic<bool> take_down_requested_ = false;

  void ForwarderThread() {
    constexpr uint64_t kMaxEventsPerRequest = 75'000;
    std::vector<IntermediateEventT> dequeued_events;
    dequeued_events.resize(kMaxEventsPerRequest);
    while (!take_down_requested_) {
      size_t dequeued_event_count;
      while ((dequeued_event_count = lock_free_queue_.try_dequeue_bulk(dequeued_events.begin(),
                                                                       kMaxEventsPerRequest)) > 0) {
        CHECK(dequeued_events.size() == kMaxEventsPerRequest);

        orbit_grpc_protos::SendCaptureEventsRequest send_request;
        for (size_t i = 0; i < dequeued_event_count; ++i) {
          orbit_grpc_protos::CaptureEvent* event = send_request.mutable_capture_events()->Add();
          *event = TranslateIntermediateEvent(std::move(dequeued_events[i]));
        }

        grpc::ClientContext send_message_context;
        orbit_grpc_protos::SendCaptureEventsResponse send_response;
        if (!SendCaptureEvents(send_request)) {
          ERROR("Forwarding %lu CaptureEvents", dequeued_event_count);
          break;
        }
        if (dequeued_event_count < kMaxEventsPerRequest) {
          break;
        }
      }

      std::this_thread::sleep_for(std::chrono::microseconds{100});
    }
  }
};

}  // namespace orbit_producer

#endif  // CAPTURE_EVENT_PRODUCER_LOCK_FREE_BUFFER_CAPTURE_EVENT_PRODUCER_H_
