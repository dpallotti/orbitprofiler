// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ORBIT_SERVICE_TARGET_SIDE_SERVICE_IMPL_H_
#define ORBIT_SERVICE_TARGET_SIDE_SERVICE_IMPL_H_

#include "CaptureStartStopListener.h"
#include "absl/synchronization/mutex.h"
#include "producer_side_services.grpc.pb.h"

namespace orbit_service {

class ProducerSideServiceImpl final : public orbit_grpc_protos::ProducerSideService::Service,
                                      public CaptureStartStopListener {
 public:
  grpc::Status SendCaptureEvents(::grpc::ServerContext* context,
                                 const ::orbit_grpc_protos::SendCaptureEventsRequest* request,
                                 ::orbit_grpc_protos::SendCaptureEventsResponse* response) override;

  grpc::Status ReceiveCommands(
      ::grpc::ServerContext* context, const ::orbit_grpc_protos::ReceiveCommandsRequest* request,
      ::grpc::ServerWriter< ::orbit_grpc_protos::ReceiveCommandsResponse>* writer) override;

  void OnCaptureStartRequested(CaptureEventBuffer* capture_event_buffer) override;

  void OnCaptureStopRequested() override;

  void OnExitRequest();

 private:
  enum class ProducerSideServiceStatus { kExitRequested, kCaptureStarted, kCaptureStopped };
  ProducerSideServiceStatus status_ = ProducerSideServiceStatus::kCaptureStopped;
  absl::Mutex status_mutex_;
  CaptureEventBuffer* capture_event_buffer_ = nullptr;
  absl::Mutex buffer_mutex_;
};

}  // namespace orbit_service

#endif  // ORBIT_SERVICE_TARGET_SIDE_SERVICE_IMPL_H_
