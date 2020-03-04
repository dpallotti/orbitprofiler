#include "PerfEventOpen.h"

#include <linux/perf_event.h>

#include <cerrno>
#include <cstring>

namespace LinuxTracing {
namespace {
perf_event_attr generic_event_attr() {
  perf_event_attr pe{};
  pe.size = sizeof(perf_event_attr);
  pe.sample_period = 1;
  pe.use_clockid = 1;
  pe.clockid = CLOCK_MONOTONIC;
  pe.sample_id_all = 1;  // Also include timestamps for lost events.
  pe.disabled = 1;

  // We can set these even if we do not do sampling, as without the
  // PERF_SAMPLE_STACK_USER or PERF_SAMPLE_REGS_USER flags being set in
  // perf_event_attr::sample_type they will not be used anyways.
  pe.sample_stack_user = SAMPLE_STACK_USER_SIZE;
  pe.sample_regs_user = SAMPLE_REGS_USER_ALL;

  pe.sample_type = SAMPLE_TYPE_TID_TIME_CPU;

  return pe;
}

int generic_event_open(perf_event_attr* attr, pid_t pid, int32_t cpu) {
  int fd = perf_event_open(attr, pid, cpu, -1, 0);
  if (fd == -1) {
    ERROR("perf_event_open: %s", strerror(errno));
  }
  return fd;
}

perf_event_attr uprobe_event_attr(const char* module,
                                  uint64_t function_offset) {
  perf_event_attr pe = generic_event_attr();

  pe.type = 7;  // TODO: should be read from
                //  "/sys/bus/event_source/devices/uprobe/type"
  pe.config1 =
      reinterpret_cast<uint64_t>(module);  // pe.config1 == pe.uprobe_path
  pe.config2 = function_offset;            // pe.config2 == pe.probe_offset

  return pe;
}
}  // namespace

int32_t task_event_open(pid_t pid, int32_t cpu) {
  perf_event_attr pe = generic_event_attr();
  pe.type = PERF_TYPE_SOFTWARE;
  pe.config = PERF_COUNT_SW_DUMMY;
  pe.task = 1;

  return generic_event_open(&pe, pid, cpu);
}

int32_t mmap_event_open(pid_t pid, int32_t cpu) {
  perf_event_attr pe = generic_event_attr();
  pe.type = PERF_TYPE_SOFTWARE;
  pe.config = PERF_COUNT_SW_DUMMY;
  pe.mmap = 1;

  return generic_event_open(&pe, pid, cpu);
}

int32_t mmap_task_event_open(pid_t pid, int32_t cpu) {
  perf_event_attr pe = generic_event_attr();
  pe.type = PERF_TYPE_SOFTWARE;
  pe.config = PERF_COUNT_SW_DUMMY;
  pe.mmap = 1;
  pe.task = 1;

  return generic_event_open(&pe, pid, cpu);
}

int32_t context_switch_event_open(pid_t pid, int32_t cpu) {
  perf_event_attr pe = generic_event_attr();
  pe.type = PERF_TYPE_SOFTWARE;
  pe.config = PERF_COUNT_SW_DUMMY;
  pe.context_switch = 1;

  return generic_event_open(&pe, pid, cpu);
}

int32_t sample_event_open(uint64_t period_ns, pid_t pid, int32_t cpu) {
  perf_event_attr pe = generic_event_attr();
  pe.type = PERF_TYPE_SOFTWARE;
  pe.config = PERF_COUNT_SW_CPU_CLOCK;
  pe.sample_period = period_ns;
  pe.sample_type |= PERF_SAMPLE_STACK_USER | PERF_SAMPLE_REGS_USER;

  return generic_event_open(&pe, pid, cpu);
}

int32_t sample_mmap_task_event_open(uint64_t period_ns, pid_t pid,
                                    int32_t cpu) {
  perf_event_attr pe = generic_event_attr();
  pe.type = PERF_TYPE_SOFTWARE;
  pe.config = PERF_COUNT_SW_CPU_CLOCK;
  pe.sample_period = period_ns;
  pe.sample_type |= PERF_SAMPLE_STACK_USER | PERF_SAMPLE_REGS_USER;
  // Also record mmaps, ...
  pe.mmap = 1;
  // ... forks, and termination.
  pe.task = 1;

  return generic_event_open(&pe, pid, cpu);
}

int32_t uprobe_event_open(const char* module, uint64_t function_offset,
                          pid_t pid, int32_t cpu) {
  perf_event_attr pe = uprobe_event_attr(module, function_offset);
  pe.config = 0;

  return generic_event_open(&pe, pid, cpu);
}

int32_t uprobe_stack_event_open(const char* module, uint64_t function_offset,
                                pid_t pid, int32_t cpu) {
  perf_event_attr pe = uprobe_event_attr(module, function_offset);
  pe.config = 0;
  pe.sample_type |= PERF_SAMPLE_STACK_USER | PERF_SAMPLE_REGS_USER;

  return generic_event_open(&pe, pid, cpu);
}

int32_t uretprobe_event_open(const char* module, uint64_t function_offset,
                             pid_t pid, int32_t cpu) {
  perf_event_attr pe = uprobe_event_attr(module, function_offset);
  pe.config = 1;  // Set bit 0 of config for uretprobe.

  return generic_event_open(&pe, pid, cpu);
}

int32_t uretprobe_stack_event_open(const char* module, uint64_t function_offset,
                                   pid_t pid, int32_t cpu) {
  perf_event_attr pe = uprobe_event_attr(module, function_offset);
  pe.config = 1;  // Set bit 0 of config for uretprobe.
  pe.sample_type |= PERF_SAMPLE_STACK_USER | PERF_SAMPLE_REGS_USER;

  return generic_event_open(&pe, pid, cpu);
}

void* perf_event_open_mmap_ring_buffer(int fd, uint64_t mmap_length) {
  // The size of the ring buffer excluding the metadata page must be a power of
  // two number of pages.
  static const int PAGE_SIZE = getpagesize();
  if (mmap_length < PAGE_SIZE ||
      __builtin_popcountl(mmap_length - PAGE_SIZE) != 1) {
    ERROR("mmap length for perf_event_open not 1+2^n pages: %lu", mmap_length);
    return nullptr;
  }

  // Use mmap to get access to the ring buffer.
  void* mmap_ret =
      mmap(nullptr, mmap_length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mmap_ret == reinterpret_cast<void*>(-1)) {
    ERROR("mmap: %s", strerror(errno));
    return nullptr;
  }

  return mmap_ret;
}

}  // namespace LinuxTracing
