// Copyright (c) 2021 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include <gtest/gtest.h>

#include "ClientData/ModuleManager.h"
#include "ClientModel/CaptureData.h"
#include "TimeGraph.h"
#include "Track.h"
#include "TrackManager.h"
#include "capture.pb.h"
#include "capture_data.pb.h"

using orbit_client_protos::TimerInfo;

constexpr uint64_t kCallstackId = 1;
constexpr uint64_t kFunctionAbsoluteAddress = 0x30;
constexpr uint64_t kInstructionAbsoluteAddress = 0x31;
constexpr int32_t kThreadId = 42;
constexpr const char* kFunctionName = "example function";
constexpr const char* kModuleName = "example module";
constexpr const char* kThreadName = "example thread";

constexpr size_t kTimerOnlyThreadId = 128;
constexpr const char* kTimerOnlyThreadName = "timer only thread";

namespace {

// This is copied from CallTreeViewItemModelTest.cpp, we may want to supply this in a header,
// but it is not clear what we want to be testing here in the future.
std::unique_ptr<orbit_client_model::CaptureData> GenerateTestCaptureData() {
  auto capture_data = std::make_unique<orbit_client_model::CaptureData>(
      nullptr, orbit_grpc_protos::CaptureStarted{}, std::nullopt, absl::flat_hash_set<uint64_t>{});

  // AddressInfo
  orbit_client_protos::LinuxAddressInfo address_info;
  address_info.set_absolute_address(kInstructionAbsoluteAddress);
  address_info.set_offset_in_function(kInstructionAbsoluteAddress - kFunctionAbsoluteAddress);
  address_info.set_function_name(kFunctionName);
  address_info.set_module_path(kModuleName);
  capture_data->InsertAddressInfo(address_info);

  // CallstackInfo
  const std::vector<uint64_t> callstack_frames{kInstructionAbsoluteAddress};
  orbit_client_protos::CallstackInfo callstack_info;
  *callstack_info.mutable_frames() = {callstack_frames.begin(), callstack_frames.end()};
  callstack_info.set_type(orbit_client_protos::CallstackInfo_CallstackType_kComplete);
  capture_data->AddUniqueCallstack(kCallstackId, std::move(callstack_info));

  // CallstackEvent
  orbit_client_protos::CallstackEvent callstack_event;
  callstack_event.set_callstack_id(kCallstackId);
  callstack_event.set_thread_id(kThreadId);
  capture_data->AddCallstackEvent(std::move(callstack_event));

  capture_data->AddOrAssignThreadName(kThreadId, kThreadName);
  capture_data->AddOrAssignThreadName(kTimerOnlyThreadId, kTimerOnlyThreadName);

  return capture_data;
}

}  // namespace

namespace orbit_gl {

class UnitTestTrack : public Track {
 public:
  explicit UnitTestTrack(TimeGraphLayout* layout, Track::Type type, const std::string& name)
      : Track(nullptr, nullptr, nullptr, layout, nullptr, 0), name_(name), type_(type){};

  [[nodiscard]] Type GetType() const override { return type_; }
  [[nodiscard]] float GetHeight() const override { return 100.f; }
  [[nodiscard]] bool IsEmpty() const override { return false; }

  [[nodiscard]] std::vector<std::shared_ptr<TimerChain>> GetAllChains() const override {
    return {};
  }
  [[nodiscard]] std::vector<std::shared_ptr<TimerChain>> GetAllSerializableChains() const override {
    return {};
  }

 private:
  std::string name_;
  Type type_;
};  // namespace orbit_gl

const size_t kNumTracks = 3;
const size_t kNumThreadTracks = 2;
const size_t kNumSchedulerTracks = 1;

class TrackManagerTest : public ::testing::Test {
 public:
  explicit TrackManagerTest()
      : capture_data_(GenerateTestCaptureData()),
        track_manager_(nullptr, nullptr, &layout_, nullptr, capture_data_.get()) {}

 protected:
  void CreateAndFillTracks() {
    auto* scheduler_track = track_manager_.GetOrCreateSchedulerTrack();
    auto* thread_track = track_manager_.GetOrCreateThreadTrack(kThreadId);
    auto* timer_only_thread_track = track_manager_.GetOrCreateThreadTrack(kTimerOnlyThreadId);

    TimerInfo timer;
    timer.set_start(0);
    timer.set_end(100);
    timer.set_thread_id(kThreadId);
    timer.set_processor(0);
    timer.set_depth(0);
    timer.set_type(TimerInfo::kCoreActivity);

    scheduler_track->OnTimer(timer);
    timer.set_type(TimerInfo::kCoreActivity);
    thread_track->OnTimer(timer);

    timer.set_thread_id(kTimerOnlyThreadId);
    scheduler_track->OnTimer(timer);
    timer.set_type(TimerInfo::kCoreActivity);
    timer_only_thread_track->OnTimer(timer);
  }

  TimeGraphLayout layout_;
  std::unique_ptr<orbit_client_model::CaptureData> capture_data_;
  TrackManager track_manager_;
};

TEST_F(TrackManagerTest, GetOrCreateCreatesTracks) {
  EXPECT_EQ(0ull, track_manager_.GetAllTracks().size());

  track_manager_.GetOrCreateSchedulerTrack();
  EXPECT_EQ(1ull, track_manager_.GetAllTracks().size());
  track_manager_.GetOrCreateThreadTrack(42);
  EXPECT_EQ(2ull, track_manager_.GetAllTracks().size());
}

TEST_F(TrackManagerTest, AllButEmptyTracksAreVisible) {
  CreateAndFillTracks();
  track_manager_.UpdateTracksForRendering();
  EXPECT_EQ(kNumTracks, track_manager_.GetVisibleTracks().size());
}

// TODO(b/181671054): Once the scheduler track stays visible, this needs to be adjusted
TEST_F(TrackManagerTest, SimpleFiltering) {
  CreateAndFillTracks();
  track_manager_.SetFilter("example");
  track_manager_.UpdateTracksForRendering();
  EXPECT_EQ(1ull, track_manager_.GetVisibleTracks().size());

  track_manager_.SetFilter("thread");
  track_manager_.UpdateTracksForRendering();
  EXPECT_EQ(kNumThreadTracks, track_manager_.GetVisibleTracks().size());

  track_manager_.SetFilter("nonsense");
  track_manager_.UpdateTracksForRendering();
  EXPECT_EQ(0ull, track_manager_.GetVisibleTracks().size());
}

TEST_F(TrackManagerTest, NonVisibleTracksAreNotInTheList) {
  CreateAndFillTracks();
  track_manager_.GetOrCreateSchedulerTrack()->SetVisible(false);
  track_manager_.UpdateTracksForRendering();
  EXPECT_EQ(kNumTracks - 1, track_manager_.GetVisibleTracks().size());
}

TEST_F(TrackManagerTest, FiltersAndVisibilityWorkTogether) {
  CreateAndFillTracks();
  track_manager_.GetOrCreateThreadTrack(kThreadId)->SetVisible(false);
  track_manager_.SetFilter("thread");
  track_manager_.UpdateTracksForRendering();
  EXPECT_EQ(kNumThreadTracks - 1, track_manager_.GetVisibleTracks().size());
}

TEST_F(TrackManagerTest, TrackTypeVisibilityAffectsVisibleTrackList) {
  CreateAndFillTracks();

  track_manager_.SetTrackTypeVisibility(Track::Type::kThreadTrack, false);
  track_manager_.UpdateTracksForRendering();
  EXPECT_EQ(kNumTracks - kNumThreadTracks, track_manager_.GetVisibleTracks().size());

  track_manager_.SetTrackTypeVisibility(Track::Type::kSchedulerTrack, false);
  track_manager_.UpdateTracksForRendering();
  EXPECT_EQ(kNumTracks - kNumThreadTracks - kNumSchedulerTracks,
            track_manager_.GetVisibleTracks().size());
  track_manager_.SetTrackTypeVisibility(Track::Type::kSchedulerTrack, true);

  track_manager_.GetOrCreateThreadTrack(kThreadId)->SetVisible(false);
  track_manager_.UpdateTracksForRendering();
  EXPECT_EQ(kNumTracks - kNumThreadTracks, track_manager_.GetVisibleTracks().size());

  track_manager_.SetTrackTypeVisibility(Track::Type::kThreadTrack, true);
  track_manager_.UpdateTracksForRendering();
  EXPECT_EQ(kNumTracks - 1, track_manager_.GetVisibleTracks().size());
}

}  // namespace orbit_gl