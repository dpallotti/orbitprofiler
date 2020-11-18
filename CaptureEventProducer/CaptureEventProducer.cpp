// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "CaptureEventProducer/CaptureEventProducer.h"

#include "OrbitBase/Logging.h"
#include "grpcpp/grpcpp.h"

using orbit_grpc_protos::ProducerSideService;
using orbit_grpc_protos::ReceiveCommandsRequest;
using orbit_grpc_protos::ReceiveCommandsResponse;
using orbit_grpc_protos::SendCaptureEventsRequest;
using orbit_grpc_protos::SendCaptureEventsResponse;

namespace orbit_producer {

bool CaptureEventProducer::SendCaptureEvents(
    const SendCaptureEventsRequest& send_capture_events_request) {
  CHECK(producer_side_service_stub_ != nullptr);
  {
    // Acquiring the mutex just for the CHECK might seem expensive,
    // but the gRPC call that follows is orders of magnitude slower.
    absl::ReaderMutexLock lock{&shutdown_requested_mutex_};
    CHECK(!shutdown_requested_);
  }

  grpc::ClientContext send_capture_events_context;
  SendCaptureEventsResponse send_capture_events_response;
  grpc::Status status = producer_side_service_stub_->SendCaptureEvents(
      &send_capture_events_context, send_capture_events_request, &send_capture_events_response);
  if (!status.ok()) {
    ERROR("Sending CaptureEvents from CaptureEventProducer: %s", status.error_message());
    return false;
  }
  return true;
}

bool CaptureEventProducer::ConnectAndStart(std::string_view unix_domain_socket_path) {
  CHECK(producer_side_service_stub_ == nullptr);
  {
    absl::ReaderMutexLock lock{&shutdown_requested_mutex_};
    CHECK(!shutdown_requested_);
  }

  std::string server_address = absl::StrFormat("unix://%s", unix_domain_socket_path);
  std::shared_ptr<grpc::Channel> channel = grpc::CreateCustomChannel(
      server_address, grpc::InsecureChannelCredentials(), grpc::ChannelArguments{});
  if (channel == nullptr) {
    ERROR("Creating gRPC channel for CaptureEventProducer");
    return false;
  }

  producer_side_service_stub_ = ProducerSideService::NewStub(channel);
  if (producer_side_service_stub_ == nullptr) {
    ERROR("Creating gRPC stub for CaptureEventProducer");
    return false;
  }

  receive_commands_thread_ = std::thread{[this] { ReceiveCommandsThread(); }};
  return true;
}

void CaptureEventProducer::ShutdownAndWait() {
  CHECK(producer_side_service_stub_ != nullptr);

  {
    absl::MutexLock lock{&shutdown_requested_mutex_};
    CHECK(!shutdown_requested_);
    shutdown_requested_ = true;
  }
  {
    absl::MutexLock lock{&receive_commands_context_mutex_};
    CHECK(receive_commands_context_ != nullptr);
    receive_commands_context_->TryCancel();
  }

  CHECK(receive_commands_thread_.joinable());
  receive_commands_thread_.join();

  producer_side_service_stub_ = nullptr;
}

void CaptureEventProducer::OnCaptureStart() { LOG("CaptureEventProducer called OnCaptureStart"); }

void CaptureEventProducer::OnCaptureStop() { LOG("CaptureEventProducer called OnCaptureStop"); }

void CaptureEventProducer::ReceiveCommandsThread() {
  CHECK(producer_side_service_stub_ != nullptr);
  {
    absl::ReaderMutexLock lock{&shutdown_requested_mutex_};
    CHECK(!shutdown_requested_);
  }
  constexpr absl::Duration kRetryConnectingDelay = absl::Seconds(5);

  while (true) {
    {
      absl::ReaderMutexLock lock{&shutdown_requested_mutex_};
      if (shutdown_requested_) {
        break;
      }
    }

    std::unique_ptr<grpc::ClientReader<ReceiveCommandsResponse>> reader;
    {
      absl::MutexLock lock{&receive_commands_context_mutex_};
      receive_commands_context_ = std::make_unique<grpc::ClientContext>();
      ReceiveCommandsRequest receive_commands_request;
      reader = producer_side_service_stub_->ReceiveCommands(receive_commands_context_.get(),
                                                            receive_commands_request);
    }

    if (reader == nullptr) {
      ERROR(
          "Establishing gRPC connection for CaptureEventProducer to receive "
          "ReceiveCommandsResponses");
      // This is the reason why we protect shutdown_requested_ with a Mutex
      // instead of using an std::atomic<bool>: so that we can use LockWhenWithTimeout.
      shutdown_requested_mutex_.ReaderLockWhenWithTimeout(
          absl::Condition(
              +[](bool* shutdown_requested) { return *shutdown_requested; }, &shutdown_requested_),
          kRetryConnectingDelay);
      shutdown_requested_mutex_.Unlock();
      continue;
    }
    LOG("CaptureEventProducer ready to receive ReceiveCommandsResponses");

    ReceiveCommandsResponse receive_commands_response;
    while (true) {
      {
        absl::ReaderMutexLock lock{&shutdown_requested_mutex_};
        if (shutdown_requested_) {
          break;
        }
      }

      if (!reader->Read(&receive_commands_response)) {
        ERROR("Receiving ReceiveCommandsResponse for CaptureEventProducer");
        if (is_capturing_) {
          OnCaptureStop();
          is_capturing_ = false;
        }
        reader->Finish();
        shutdown_requested_mutex_.ReaderLockWhenWithTimeout(
            absl::Condition(
                +[](bool* shutdown_requested) { return *shutdown_requested; },
                &shutdown_requested_),
            kRetryConnectingDelay);
        shutdown_requested_mutex_.Unlock();
        break;
      }

      switch (receive_commands_response.command_case()) {
        case ReceiveCommandsResponse::kCheckAliveCommand: {
          LOG("CaptureEventProducer received CheckAliveCommand");
        } break;
        case ReceiveCommandsResponse::kStartCaptureCommand: {
          LOG("CaptureEventProducer received StartCaptureCommand");
          if (!is_capturing_) {
            OnCaptureStart();
            is_capturing_ = true;
          }
        } break;
        case ReceiveCommandsResponse::kStopCaptureCommand: {
          LOG("CaptureEventProducer received StopCaptureCommand");
          if (is_capturing_) {
            OnCaptureStop();
            is_capturing_ = false;
          }
        } break;
        case ReceiveCommandsResponse::COMMAND_NOT_SET: {
          ERROR("CaptureEventProducer received COMMAND_NOT_SET");
        } break;
      }
    }

    {
      absl::MutexLock lock{&receive_commands_context_mutex_};
      receive_commands_context_ = nullptr;
    }
  }
}

}  // namespace orbit_producer
