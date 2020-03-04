#ifndef ORBIT_LINUX_TRACING_TRACER_THREAD_2_H_
#define ORBIT_LINUX_TRACING_TRACER_THREAD_2_H_

#include <OrbitLinuxTracing/Events.h>
#include <OrbitLinuxTracing/Function.h>
#include <OrbitLinuxTracing/TracerListener.h>

#include <atomic>
#include <memory>
#include <regex>
#include <vector>

#include "PerfEventProcessor.h"
#include "PerfEventProcessor2.h"
#include "PerfEventSampler.h"
#include "PerfEventVisitor.h"
#include "UprobesUnwindingVisitor.h"

namespace LinuxTracing {

class TracerThread2 : private PerfEventVisitor {
 public:
  TracerThread2(pid_t pid, uint64_t sampling_period_ns,
                std::vector<Function> instrumented_functions)
      : pid_(pid),
        sampling_period_ns_(sampling_period_ns),
        instrumented_functions_(std::move(instrumented_functions)) {}

  TracerThread2(const TracerThread2&) = delete;
  TracerThread2& operator=(const TracerThread2&) = delete;
  TracerThread2(TracerThread2&&) = default;
  TracerThread2& operator=(TracerThread2&&) = default;

  void SetListener(TracerListener* listener) { listener_ = listener; }

  void SetTraceContextSwitches(bool trace_context_switches) {
    trace_context_switches_ = trace_context_switches;
  }

  void SetTraceCallstacks(bool trace_callstacks) {
    trace_callstacks_ = trace_callstacks;
  }

  void SetTraceInstrumentedFunctions(bool trace_instrumented_functions) {
    trace_instrumented_functions_ = trace_instrumented_functions;
  }

  void Run(const std::shared_ptr<std::atomic<bool>>& exit_requested);

 private:
  pid_t pid_;
  uint64_t sampling_period_ns_;
  std::vector<Function> instrumented_functions_;

  TracerListener* listener_ = nullptr;

  bool trace_context_switches_ = true;
  bool trace_callstacks_ = true;
  bool trace_instrumented_functions_ = true;

  // Switch between PerfEventProcessor and PerfEventProcessor2 here and in the
  // .cpp file.
  std::unique_ptr<PerfEventProcessor2> uprobes_event_processor_ = nullptr;

  std::vector<PerfEventSampler> context_switch_samplers_;
  std::vector<PerfEventSampler> mmap_fork_exit_samplers_;
  std::vector<PerfEventSampler> stack_samplers_;
  std::vector<PerfEventSampler> uprobes_samplers_;
  std::vector<PerfEventSampler> uretprobes_samplers_;

  void CreateSamplers();
  void StartSamplers();
  bool PollAllSamplers();
  bool PollSingleSampler(PerfEventSampler* sampler);
  void StopAndDestroySamplers();

  // We make TracerThread2 a PerfEventVisitor in order to process each PerfEvent
  // according to its concrete type. Some of these visit methods, though, need
  // to operate on the unique_ptr that handles them (in particular, to move it
  // into uprobes_event_processor_). For this, we immediately store each newly
  // read event in current_event_ before calling Accept on it.
  void visit(SystemWideContextSwitchPerfEvent* event) override;
  void visit(MmapPerfEvent* event) override;
  void visit(ForkPerfEvent* event) override;
  void visit(ExitPerfEvent* event) override;
  void visit(StackSamplePerfEvent* event) override;
  void visit(UprobesWithStackPerfEvent* event) override;
  void visit(UretprobesPerfEvent* event) override;
  void visit(LostPerfEvent* event) override;

  int current_fd_ = -1;
  std::unique_ptr<PerfEvent> current_event_ = nullptr;

  // Record and periodically print basic statistics on the number events.
  static constexpr uint64_t EVENT_COUNT_WINDOW_S = 1;
  uint64_t event_count_window_begin_ns_ = 0;
  uint64_t context_switch_count_ = 0;
  uint64_t stack_sample_count_ = 0;
  uint64_t uprobes_uretprobes_count_ = 0;
};

}  // namespace LinuxTracing

#endif  // ORBIT_LINUX_TRACING_TRACER_THREAD_2_H_
