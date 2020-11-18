// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CAPTURE_EVENT_PRODUCER_CAPTURE_EVENT_PRODUCER_H_
#define CAPTURE_EVENT_PRODUCER_CAPTURE_EVENT_PRODUCER_H_

#include <thread>

#include "absl/synchronization/mutex.h"
#include "producer_side_services.grpc.pb.h"

namespace orbit_producer {

class CaptureEventProducer {
 public:
  virtual ~CaptureEventProducer() = default;

  [[nodiscard]] bool SendCaptureEvents(
      const orbit_grpc_protos::SendCaptureEventsRequest& send_capture_events_request);

  [[nodiscard]] bool ConnectAndStart(std::string_view unix_domain_socket_path);
  void ShutdownAndWait();

  [[nodiscard]] bool IsCapturing() { return is_capturing_; }

 protected:
  virtual void OnCaptureStart();
  virtual void OnCaptureStop();

 private:
  void ReceiveCommandsThread();

  std::unique_ptr<orbit_grpc_protos::ProducerSideService::Stub> producer_side_service_stub_;
  std::thread receive_commands_thread_;
  bool shutdown_requested_ = false;
  absl::Mutex shutdown_requested_mutex_;
  std::unique_ptr<grpc::ClientContext> receive_commands_context_;
  absl::Mutex receive_commands_context_mutex_;
  std::atomic<bool> is_capturing_ = false;
};

}  // namespace orbit_producer

#endif  // CAPTURE_EVENT_PRODUCER_CAPTURE_EVENT_PRODUCER_H_
