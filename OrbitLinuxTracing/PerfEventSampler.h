#ifndef ORBIT_LINUX_TRACING_PERF_EVENT_SAMPLER_H_
#define ORBIT_LINUX_TRACING_PERF_EVENT_SAMPLER_H_

#include <OrbitLinuxTracing/Function.h>
#include <sys/ioctl.h>

#include <memory>
#include <optional>
#include <utility>

#include "PerfEvent.h"
#include "PerfEventRingBuffer.h"
#include "UniqueFd.h"

namespace LinuxTracing {

// The "sampler" name comes from perf_event_open's nomenclature: "Events come in
// two flavors: counting and sampled. [...] A sampling event periodically writes
// measurements to a buffer that can then be accessed via mmap(2)."
class PerfEventSampler {
 public:
  PerfEventSampler(const PerfEventSampler&) = delete;
  PerfEventSampler& operator=(const PerfEventSampler&) = delete;

  PerfEventSampler(PerfEventSampler&& o) = default;
  PerfEventSampler& operator=(PerfEventSampler&& o) = default;

  ~PerfEventSampler() { Stop(); }

  int GetFileDescriptor() { return file_descriptor_.Get(); }

  bool IsOpen() { return file_descriptor_.Ok() && ring_buffer_.IsOpen(); }

  void Start() {
    if (IsOpen()) {
      perf_event_reset_and_enable(file_descriptor_.Get());
    }
  }

  void Stop() {
    if (IsOpen()) {
      perf_event_disable(file_descriptor_.Get());
    }
  }

  bool HasNewEvent();

  std::unique_ptr<PerfEvent> ReadEvent();

  static PerfEventSampler OpenForContextSwitches(int32_t cpu);
  static PerfEventSampler OpenForMmapForkExit(int32_t cpu, pid_t pid);
  static PerfEventSampler OpenForStackSamples(int32_t cpu, pid_t pid,
                                              uint64_t period_ns);
  static PerfEventSampler OpenForUprobesWithStack(int32_t cpu,
                                                  const Function* function,
                                                  pid_t pid);
  static PerfEventSampler OpenForUretprobes(int32_t cpu,
                                            const Function* function,
                                            pid_t pid);

 private:
  UniqueFd file_descriptor_;
  PerfEventRingBuffer ring_buffer_;

  pid_t filtering_pid_ = -1;
  const Function* uprobes_function_ = nullptr;
  bool is_uretprobes_ = false;

  perf_event_header next_header_{};
  bool next_header_valid_ = false;

  std::unique_ptr<PerfEvent> next_event_ = nullptr;
  bool next_event_valid_ = false;

  explicit PerfEventSampler(int fd, uint64_t ring_buffer_size_kb)
      : file_descriptor_{UniqueFd{fd}},
        ring_buffer_{file_descriptor_.Get(), ring_buffer_size_kb} {
    if (!ring_buffer_.IsOpen()) {
      file_descriptor_.Reset();
    }
  }

  void SetFilteringPid(pid_t filtering_pid) { filtering_pid_ = filtering_pid; }

  void SetUprobesFunction(const Function* uprobes_function) {
    uprobes_function_ = uprobes_function;
  }

  void SetIsUretprobes(bool is_uretprobes) { is_uretprobes_ = is_uretprobes; }

  std::unique_ptr<PerfEvent> ConsumeEvent();
  pid_t ReadPid(uint32_t record_type);
  std::unique_ptr<MmapPerfEvent> SkipRecord();

  std::unique_ptr<SystemWideContextSwitchPerfEvent>
  ConsumeSystemWideContextSwitch();
  std::unique_ptr<MmapPerfEvent> ConsumeMmap();
  std::unique_ptr<ForkPerfEvent> ConsumeFork();
  std::unique_ptr<ExitPerfEvent> ConsumeExit();
  std::unique_ptr<StackSamplePerfEvent> ConsumeStackSample();
  std::unique_ptr<UprobesWithStackPerfEvent> ConsumeUprobesWithStack();
  std::unique_ptr<UretprobesPerfEvent> ConsumeUretprobes();
  void ReadSampleRecord(perf_event_sample* sample);
  std::unique_ptr<LostPerfEvent> ConsumeLost();
};

}  // namespace LinuxTracing

#endif  // ORBIT_LINUX_TRACING_PERF_EVENT_SAMPLER_H_
