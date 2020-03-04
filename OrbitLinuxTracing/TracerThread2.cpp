#include "TracerThread2.h"

#include "InstrumentationStopwatch.h"
#include "PerfEventProcessor.h"
#include "PerfEventProcessor2.h"
#include "Utils.h"

namespace LinuxTracing {

void TracerThread2::Run(
    const std::shared_ptr<std::atomic<bool>>& exit_requested) {
  CreateSamplers();

  StartSamplers();

  for (pid_t tid : ListThreads(pid_)) {
    if (listener_ != nullptr) {
      listener_->OnTid(tid);
    }
  }

  event_count_window_begin_ns_ = 0;
  context_switch_count_ = 0;
  stack_sample_count_ = 0;
  uprobes_uretprobes_count_ = 0;

  // Parameters for exponential backoff on sleeping time when a poll on all
  // samplers (and their ring buffers) returned no events. These values are
  // quite arbitrary and may need tweaking. Still, stay around the order of
  // magnitude of milliseconds, which has proved to be good trade-off between
  // polling frequency and not losing events.
  constexpr int64_t POLLING_SLEEP_US_MIN = 256;
  constexpr int64_t POLLING_SLEEP_US_MAX = 8192;
  constexpr int64_t POLLING_SLEEP_FACTOR = 2;

  int64_t polling_sleep_us = POLLING_SLEEP_US_MIN;
  bool last_iteration_saw_events = false;

  gInstrumentationStopwatch.Reset();
  gInstrumentationStopwatch.StopAllAndStart(CATEGORY_TRACING);

  while (!(*exit_requested)) {
    // In order not to constantly poll the buffers, sleep if there was no new
    // event in the last iteration. Exponentially increase the sleeping time
    // while no events are read. Be careful to always keep the sleeping time
    // small enough to not have the buffers overflow and therefore lose events.
    // Note that, when collecting scheduling switches, it rarely happens to have
    // two consecutive iterations without events, therefore the minimum sleeping
    // time shouldn't be too small.
    // TODO: This strategy might need more refinement, or a completely different
    //  strategy might be employed.
    if (!last_iteration_saw_events) {
      gInstrumentationStopwatch.StopAllAndStart(CATEGORY_SLEEP);
      usleep(polling_sleep_us);
      gInstrumentationStopwatch.StopAllAndStart(CATEGORY_TRACING);
      polling_sleep_us *= POLLING_SLEEP_FACTOR;
      if (polling_sleep_us > POLLING_SLEEP_US_MAX) {
        polling_sleep_us = POLLING_SLEEP_US_MAX;
      }
    } else {
      polling_sleep_us = POLLING_SLEEP_US_MIN;
    }

    last_iteration_saw_events = PollAllSamplers();

    uint64_t timestamp_ns = MonotonicTimestampNs();
    if (event_count_window_begin_ns_ == 0) {
      event_count_window_begin_ns_ = timestamp_ns;
    } else if (event_count_window_begin_ns_ +
                   EVENT_COUNT_WINDOW_S * 1'000'000'000 <
               timestamp_ns) {
      double actual_event_count_window_s =
          static_cast<double>(timestamp_ns - event_count_window_begin_ns_) /
          1e9;
      LOG("Events per second (last %.1f s): "
          "sched switches: %.0f; "
          "samples: %.0f; "
          "u(ret)probes: %.0f",
          actual_event_count_window_s,
          context_switch_count_ / actual_event_count_window_s,
          stack_sample_count_ / actual_event_count_window_s,
          uprobes_uretprobes_count_ / actual_event_count_window_s);
      context_switch_count_ = 0;
      stack_sample_count_ = 0;
      uprobes_uretprobes_count_ = 0;
      event_count_window_begin_ns_ = timestamp_ns;

      gInstrumentationStopwatch.StopAll();
      gInstrumentationStopwatch.Print(actual_event_count_window_s);
      gInstrumentationStopwatch.Reset();
    }

    uprobes_event_processor_->ProcessOldEvents();
  }

  gInstrumentationStopwatch.StopAll();

  StopAndDestroySamplers();

  uprobes_event_processor_->ProcessAllEvents();
}

void TracerThread2::CreateSamplers() {
  // perf_event_open refers to cores as "CPUs".
  int32_t num_cpus = GetNumCores();

  bool sampler_creation_error = false;

  LOG("Context switches fds");
  if (trace_context_switches_) {
    for (int32_t cpu = 0; cpu < num_cpus; cpu++) {
      PerfEventSampler context_switch_sampler =
          PerfEventSampler::OpenForContextSwitches(cpu);
      if (context_switch_sampler.IsOpen()) {
        LOG("%d", context_switch_sampler.GetFileDescriptor());
        context_switch_samplers_.push_back(std::move(context_switch_sampler));
      } else {
        sampler_creation_error = true;
      }
    }
  }

  LOG("Mmap fork exit fds");
  for (int32_t cpu = 0; cpu < num_cpus; cpu++) {
    PerfEventSampler mmap_fork_exit_sampler =
        PerfEventSampler::OpenForMmapForkExit(cpu, pid_);
    if (mmap_fork_exit_sampler.IsOpen()) {
      LOG("%d", mmap_fork_exit_sampler.GetFileDescriptor());
      mmap_fork_exit_samplers_.push_back(std::move(mmap_fork_exit_sampler));
    } else {
      sampler_creation_error = true;
    }
  }

  LOG("Stack sample fds");
  if (trace_callstacks_) {
    for (int32_t cpu = 0; cpu < num_cpus; cpu++) {
      PerfEventSampler callstack_sampler =
          PerfEventSampler::OpenForStackSamples(cpu, pid_, sampling_period_ns_);
      if (callstack_sampler.IsOpen()) {
        LOG("%d", callstack_sampler.GetFileDescriptor());
        stack_samplers_.push_back(std::move(callstack_sampler));
      } else {
        sampler_creation_error = true;
      }
    }
  }

  LOG("Uprobes/uretprobes fds");
  if (trace_instrumented_functions_) {
    for (const auto& function : instrumented_functions_) {
      for (int32_t cpu = 0; cpu < num_cpus; cpu++) {
        PerfEventSampler uprobes_sampler =
            PerfEventSampler::OpenForUprobesWithStack(cpu, &function, pid_);
        if (uprobes_sampler.IsOpen()) {
          LOG("%d", uprobes_sampler.GetFileDescriptor());
          uprobes_samplers_.push_back(std::move(uprobes_sampler));
        } else {
          sampler_creation_error = true;
        }

        PerfEventSampler uretprobes_sampler =
            PerfEventSampler::OpenForUretprobes(cpu, &function, pid_);
        if (uretprobes_sampler.IsOpen()) {
          LOG("%d", uretprobes_sampler.GetFileDescriptor());
          uretprobes_samplers_.push_back(std::move(uretprobes_sampler));
        } else {
          sampler_creation_error = true;
        }
      }
    }
  }

  if (sampler_creation_error) {
    LOG("There were errors with perf_event_open: did you forget to run as "
        "root?");
  }

  auto uprobes_unwinding_visitor =
      std::make_unique<UprobesUnwindingVisitor>(ReadMaps(pid_));
  uprobes_unwinding_visitor->SetListener(listener_);
  // Switch between PerfEventProcessor and PerfEventProcessor2 here and in the
  // .h file. PerfEventProcessor2 is supposedly faster but assumes that events
  // from the same perf_event_open ring buffer are already sorted.
  uprobes_event_processor_ = std::make_unique<PerfEventProcessor2>(
      std::move(uprobes_unwinding_visitor));
}

void TracerThread2::StartSamplers() {
  for (PerfEventSampler& sampler : context_switch_samplers_) {
    sampler.Start();
  }
  for (PerfEventSampler& sampler : mmap_fork_exit_samplers_) {
    sampler.Start();
  }
  for (PerfEventSampler& sampler : stack_samplers_) {
    sampler.Start();
  }
  for (PerfEventSampler& sampler : uprobes_samplers_) {
    sampler.Start();
  }
  for (PerfEventSampler& sampler : uretprobes_samplers_) {
    sampler.Start();
  }
}

void TracerThread2::StopAndDestroySamplers() {
  for (PerfEventSampler& sampler : context_switch_samplers_) {
    sampler.Stop();
  }
  context_switch_samplers_.clear();

  for (PerfEventSampler& sampler : mmap_fork_exit_samplers_) {
    sampler.Stop();
  }
  mmap_fork_exit_samplers_.clear();

  for (PerfEventSampler& sampler : stack_samplers_) {
    sampler.Stop();
  }
  stack_samplers_.clear();

  for (PerfEventSampler& sampler : uprobes_samplers_) {
    sampler.Stop();
  }
  uprobes_samplers_.clear();

  for (PerfEventSampler& sampler : uretprobes_samplers_) {
    sampler.Stop();
  }
  uretprobes_samplers_.clear();
}

bool TracerThread2::PollAllSamplers() {
  // TODO: These values are arbitrary and can definitely use refinement.
  constexpr int32_t CONTEXT_SWITCH_POLLING_BATCH = 5;
  constexpr int32_t MMAP_FORK_EXIT_POLLING_BATCH = 5;
  constexpr int32_t STACK_POLLING_BATCH = 5;
  constexpr int32_t UPROBES_POLLING_BATCH = 5;
  constexpr int32_t URETPROBES_POLLING_BATCH = 5;

  bool poll_saw_events = false;

  for (PerfEventSampler& sampler : context_switch_samplers_) {
    for (int32_t read = 0; read < CONTEXT_SWITCH_POLLING_BATCH; ++read) {
      if (PollSingleSampler(&sampler)) {
        poll_saw_events = true;
      } else {
        break;
      }
    }
  }

  for (PerfEventSampler& sampler : mmap_fork_exit_samplers_) {
    for (int32_t read = 0; read < MMAP_FORK_EXIT_POLLING_BATCH; ++read) {
      if (PollSingleSampler(&sampler)) {
        poll_saw_events = true;
      } else {
        break;
      }
    }
  }
  for (PerfEventSampler& sampler : stack_samplers_) {
    for (int32_t read = 0; read < STACK_POLLING_BATCH; ++read) {
      if (PollSingleSampler(&sampler)) {
        poll_saw_events = true;
      } else {
        break;
      }
    }
  }

  for (PerfEventSampler& sampler : uprobes_samplers_) {
    for (int32_t read = 0; read < UPROBES_POLLING_BATCH; ++read) {
      if (PollSingleSampler(&sampler)) {
        poll_saw_events = true;
      } else {
        break;
      }
    }
  }

  for (PerfEventSampler& sampler : uretprobes_samplers_) {
    for (int32_t read = 0; read < URETPROBES_POLLING_BATCH; ++read) {
      if (PollSingleSampler(&sampler)) {
        poll_saw_events = true;
      } else {
        break;
      }
    }
  }

  return poll_saw_events;
}

bool TracerThread2::PollSingleSampler(PerfEventSampler* sampler) {
  current_fd_ = sampler->GetFileDescriptor();
  if (sampler->HasNewEvent()) {
    current_event_ = sampler->ReadEvent();
    current_event_->Accept(this);
    return true;
  } else {
    return false;
  }
}

void TracerThread2::visit(SystemWideContextSwitchPerfEvent* event) {
  const SystemWideContextSwitchPerfEvent& context_switch = *event;

  if (context_switch.GetPrevTid() != 0) {
    ContextSwitchOut context_switch_out{
        context_switch.GetPrevTid(),
        static_cast<uint16_t>(context_switch.GetCpu()),
        context_switch.GetTimestamp()};
    if (listener_ != nullptr) {
      gInstrumentationStopwatch.StopAllAndStart(CATEGORY_LISTENER);
      listener_->OnContextSwitchOut(context_switch_out);
      gInstrumentationStopwatch.StopAllAndStart(CATEGORY_TRACING);
    }
  }

  if (context_switch.GetNextTid() != 0) {
    ContextSwitchIn context_switch_in{
        context_switch.GetNextTid(),
        static_cast<uint16_t>(context_switch.GetCpu()),
        context_switch.GetTimestamp()};
    if (listener_ != nullptr) {
      gInstrumentationStopwatch.StopAllAndStart(CATEGORY_LISTENER);
      listener_->OnContextSwitchIn(context_switch_in);
      gInstrumentationStopwatch.StopAllAndStart(CATEGORY_TRACING);
    }
  }

  current_event_ = nullptr;

  ++context_switch_count_;
}

void TracerThread2::visit(MmapPerfEvent* event) {
  std::unique_ptr<MapsPerfEvent> maps_event =
      std::make_unique<MapsPerfEvent>(MonotonicTimestampNs(), ReadMaps(pid_));
  uprobes_event_processor_->AddEvent(current_fd_, std::move(maps_event));

  current_event_ = nullptr;
}

void TracerThread2::visit(ForkPerfEvent* event) {
  if (listener_ != nullptr) {
    gInstrumentationStopwatch.StopAllAndStart(CATEGORY_LISTENER);
    listener_->OnTid(event->GetTid());
    gInstrumentationStopwatch.StopAllAndStart(CATEGORY_TRACING);
  }

  current_event_ = nullptr;
}

void TracerThread2::visit(ExitPerfEvent* event) { current_event_ = nullptr; }

void TracerThread2::visit(StackSamplePerfEvent* event) {
  uprobes_event_processor_->AddEvent(current_fd_, std::move(current_event_));
  ++stack_sample_count_;
}

void TracerThread2::visit(UprobesWithStackPerfEvent* event) {
  uprobes_event_processor_->AddEvent(current_fd_, std::move(current_event_));
  ++uprobes_uretprobes_count_;
}

void TracerThread2::visit(UretprobesPerfEvent* event) {
  uprobes_event_processor_->AddEvent(current_fd_, std::move(current_event_));
  ++uprobes_uretprobes_count_;
}

void TracerThread2::visit(LostPerfEvent* event) {
  LOG("Lost %lu events from %d", event->GetNumLost(), current_fd_);
  current_event_ = nullptr;
}

}  // namespace LinuxTracing
