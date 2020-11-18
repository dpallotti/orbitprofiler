// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ProducerSideServiceImpl.h"

#include <thread>

#include "OrbitBase/Logging.h"

namespace orbit_service {

grpc::Status ProducerSideServiceImpl::SendCaptureEvents(
    ::grpc::ServerContext* /*context*/,
    const ::orbit_grpc_protos::SendCaptureEventsRequest* request,
    ::orbit_grpc_protos::SendCaptureEventsResponse* /*response*/) {
  // TODO(dpallotti): Remove this LOG.
  LOG("A producer sent %d events", request->capture_events_size());
  {
    absl::ReaderMutexLock lock{&buffer_mutex_};
    if (capture_event_buffer_ != nullptr) {
      for (orbit_grpc_protos::CaptureEvent event : request->capture_events()) {
        capture_event_buffer_->AddEvent(std::move(event));
      }
    }
  }
  return grpc::Status::OK;
}

grpc::Status ProducerSideServiceImpl::ReceiveCommands(
    ::grpc::ServerContext* /*context*/,
    const ::orbit_grpc_protos::ReceiveCommandsRequest* /*request*/,
    ::grpc::ServerWriter< ::orbit_grpc_protos::ReceiveCommandsResponse>* writer) {
  LOG("A producer has connected and is receiving ReceiveCommandsResponses");
  ProducerSideServiceStatus prev_status = ProducerSideServiceStatus::kCaptureStopped;

  while (true) {
    status_mutex_.Lock();
    if (status_ == ProducerSideServiceStatus::kExitRequested) {
      status_mutex_.Unlock();
      LOG("Terminating gRPC call to ReceiveCommands as exit was requested");
      return grpc::Status::OK;
    }

    if (status_ != prev_status) {
      if (status_ == ProducerSideServiceStatus::kCaptureStarted) {
        prev_status = status_;
        status_mutex_.Unlock();
        orbit_grpc_protos::ReceiveCommandsResponse command;
        command.mutable_start_capture_command();
        if (!writer->Write(command)) {
          ERROR("Sending StartCaptureCommand to producer");
          return grpc::Status::CANCELLED;
        }
        LOG("Sent StartCaptureCommand to producer");
      } else if (status_ == ProducerSideServiceStatus::kCaptureStopped) {
        prev_status = status_;
        status_mutex_.Unlock();
        orbit_grpc_protos::ReceiveCommandsResponse command;
        command.mutable_stop_capture_command();
        if (!writer->Write(command)) {
          ERROR("Sending StopCaptureCommand to producer");
          return grpc::Status::CANCELLED;
        }
        LOG("Sent StartCaptureCommand to producer");
      }
      continue;
    }

    constexpr absl::Duration kCheckAliveCommandDelay = absl::Seconds(5);
    if (status_ == ProducerSideServiceStatus::kCaptureStopped) {
      status_mutex_.AwaitWithTimeout(absl::Condition(
                                         +[](ProducerSideServiceStatus* status) {
                                           return *status !=
                                                  ProducerSideServiceStatus::kCaptureStopped;
                                         },
                                         &status_),
                                     kCheckAliveCommandDelay);
      status_mutex_.Unlock();
    } else if (status_ == ProducerSideServiceStatus::kCaptureStarted) {
      status_mutex_.AwaitWithTimeout(absl::Condition(
                                         +[](ProducerSideServiceStatus* status) {
                                           return *status !=
                                                  ProducerSideServiceStatus::kCaptureStarted;
                                         },
                                         &status_),
                                     kCheckAliveCommandDelay);
      status_mutex_.Unlock();
    }

    {
      orbit_grpc_protos::ReceiveCommandsResponse command;
      command.mutable_check_alive_command();
      if (!writer->Write(command)) {
        ERROR("Sending CheckAliveCommand to producer");
        return grpc::Status::CANCELLED;
      }
      LOG("Sent CheckAliveCommand to producer");
    }
  }
}

void ProducerSideServiceImpl::OnCaptureStartRequested(CaptureEventBuffer* capture_event_buffer) {
  LOG("About to send StartCaptureCommand to producers (if any)");
  {
    absl::MutexLock lock{&status_mutex_};
    status_ = ProducerSideServiceStatus::kCaptureStarted;
  }
  {
    absl::WriterMutexLock lock{&buffer_mutex_};
    capture_event_buffer_ = capture_event_buffer;
  }
}

void ProducerSideServiceImpl::OnCaptureStopRequested() {
  LOG("About to send StopCaptureCommand to producers (if any)");
  {
    absl::MutexLock lock{&status_mutex_};
    status_ = ProducerSideServiceStatus::kCaptureStopped;
  }
  // For now, give the producers a fixed time to finish sending their CaptureEvents.
  // TODO(dpallotti): Instead, wait for capture-stopped signals sent by the producers themselves.
  std::this_thread::sleep_for(std::chrono::seconds(1));
  {
    absl::WriterMutexLock lock{&buffer_mutex_};
    capture_event_buffer_ = nullptr;
  }
}

void ProducerSideServiceImpl::OnExitRequest() {
  {
    absl::MutexLock lock{&status_mutex_};
    status_ = ProducerSideServiceStatus::kExitRequested;
  }
  {
    absl::WriterMutexLock lock{&buffer_mutex_};
    capture_event_buffer_ = nullptr;
  }
}

}  // namespace orbit_service
