#ifndef ORBIT_LINUX_TRACING_PERF_EVENT_H_
#define ORBIT_LINUX_TRACING_PERF_EVENT_H_

#include <OrbitLinuxTracing/Function.h>
#include <asm/perf_regs.h>
#include <linux/perf_event.h>

#include <array>

#include "PerfEventRecords.h"

namespace LinuxTracing {

class PerfEventVisitor;

// This base class will be used in order to do processing of different
// perf_event_open events using the visitor pattern. To avoid unnecessary
// copies, the data of the perf_event_open records will be copied from the ring
// buffer directly into the concrete subclass (depending on the event type).
// When possible, the target of the copy will be a field "ring_buffer_record"
// that must be present for the subclass at compile time. As the perf_event_open
// ring buffer is 8-byte aligned, this field might also need to be extended with
// dummy bytes at the end of the record.
class PerfEvent {
 public:
  virtual ~PerfEvent() = default;
  virtual uint64_t GetTimestamp() const = 0;
  virtual void Accept(PerfEventVisitor* visitor) = 0;
};

class ContextSwitchPerfEvent : public PerfEvent {
 public:
  perf_event_context_switch ring_buffer_record;

  uint64_t GetTimestamp() const override {
    return ring_buffer_record.sample_id.time;
  }

  void Accept(PerfEventVisitor* visitor) override;

  pid_t GetPid() const {
    return static_cast<pid_t>(ring_buffer_record.sample_id.pid);
  }
  pid_t GetTid() const {
    return static_cast<pid_t>(ring_buffer_record.sample_id.tid);
  }

  uint32_t GetCpu() const { return ring_buffer_record.sample_id.cpu; }

  bool IsSwitchOut() const {
    return ring_buffer_record.header.misc & PERF_RECORD_MISC_SWITCH_OUT;
  }
  bool IsSwitchIn() const { return !IsSwitchOut(); }
};

class SystemWideContextSwitchPerfEvent : public PerfEvent {
 public:
  perf_event_context_switch_cpu_wide ring_buffer_record;

  uint64_t GetTimestamp() const override {
    return ring_buffer_record.sample_id.time;
  }

  void Accept(PerfEventVisitor* visitor) override;

  pid_t GetPrevPid() const {
    return IsSwitchOut() ? static_cast<pid_t>(ring_buffer_record.sample_id.pid)
                         : static_cast<pid_t>(ring_buffer_record.next_prev_pid);
  }

  pid_t GetPrevTid() const {
    return IsSwitchOut() ? static_cast<pid_t>(ring_buffer_record.sample_id.tid)
                         : static_cast<pid_t>(ring_buffer_record.next_prev_tid);
  }

  pid_t GetNextPid() const {
    return IsSwitchOut() ? static_cast<pid_t>(ring_buffer_record.next_prev_pid)
                         : static_cast<pid_t>(ring_buffer_record.sample_id.pid);
  }

  pid_t GetNextTid() const {
    return IsSwitchOut() ? static_cast<pid_t>(ring_buffer_record.next_prev_tid)
                         : static_cast<pid_t>(ring_buffer_record.sample_id.tid);
  }

  bool IsSwitchOut() const {
    return ring_buffer_record.header.misc & PERF_RECORD_MISC_SWITCH_OUT;
  }

  bool IsSwitchIn() const { return !IsSwitchOut(); }

  uint32_t GetCpu() const { return ring_buffer_record.sample_id.cpu; }
};

class MmapPerfEvent : public PerfEvent {
 public:
  // Unfortunately, the layout of mmap record as laid out by perf_event_open in
  // the ring buffers is not fixed.
  perf_event_header ring_buffer_header;
  uint32_t ring_buffer_pid, ring_buffer_tid;
  uint64_t ring_buffer_addr;
  uint64_t ring_buffer_len;
  uint64_t ring_buffer_pgoff;
  std::string filename;
  perf_event_sample_id_tid_time_cpu ring_buffer_sample_id;

  uint64_t GetTimestamp() const override { return ring_buffer_sample_id.time; }

  void Accept(PerfEventVisitor* visitor) override;

  pid_t GetPid() const { return static_cast<pid_t>(ring_buffer_pid); }
  pid_t GetTid() const { return static_cast<pid_t>(ring_buffer_tid); }
  uint64_t GetAddress() const { return ring_buffer_addr; }
  uint64_t GetLength() const { return ring_buffer_len; }
  uint64_t GetPageOffset() const { return ring_buffer_pgoff; }
  std::string GetFilename() const { return filename; }
};

class ForkPerfEvent : public PerfEvent {
 public:
  perf_event_fork_exit ring_buffer_record;

  uint64_t GetTimestamp() const override { return ring_buffer_record.time; }

  void Accept(PerfEventVisitor* visitor) override;

  pid_t GetPid() const { return static_cast<pid_t>(ring_buffer_record.pid); }
  pid_t GetParentPid() const {
    return static_cast<pid_t>(ring_buffer_record.ppid);
  }
  pid_t GetTid() const { return static_cast<pid_t>(ring_buffer_record.tid); }
  pid_t GetParentTid() const {
    return static_cast<pid_t>(ring_buffer_record.ptid);
  }
};

class ExitPerfEvent : public PerfEvent {
 public:
  perf_event_fork_exit ring_buffer_record;

  uint64_t GetTimestamp() const override { return ring_buffer_record.time; }

  void Accept(PerfEventVisitor* visitor) override;

  pid_t GetPid() const { return static_cast<pid_t>(ring_buffer_record.pid); }
  pid_t GetParentPid() const {
    return static_cast<pid_t>(ring_buffer_record.ppid);
  }
  pid_t GetTid() const { return static_cast<pid_t>(ring_buffer_record.tid); }
  pid_t GetParentTid() const {
    return static_cast<pid_t>(ring_buffer_record.ptid);
  }
};

class LostPerfEvent : public PerfEvent {
 public:
  perf_event_lost ring_buffer_record;

  uint64_t GetTimestamp() const override {
    return ring_buffer_record.sample_id.time;
  }

  void Accept(PerfEventVisitor* visitor) override;

  uint64_t GetNumLost() const { return ring_buffer_record.lost; }
};

template <typename perf_record_data_t>
class SamplePerfEvent : public PerfEvent {
 public:
  perf_record_data_t ring_buffer_record;

  uint64_t GetTimestamp() const override {
    return ring_buffer_record.sample_id.time;
  }

  pid_t GetPid() const {
    return static_cast<pid_t>(ring_buffer_record.sample_id.pid);
  }
  pid_t GetTid() const {
    return static_cast<pid_t>(ring_buffer_record.sample_id.tid);
  }

  uint32_t GetCpu() const { return ring_buffer_record.sample_id.cpu; }
};

namespace {
std::array<uint64_t, PERF_REG_X86_64_MAX>
perf_event_sample_regs_user_all_to_register_array(
    const perf_event_sample_regs_user_all& regs) {
  std::array<uint64_t, PERF_REG_X86_64_MAX> registers{};
  registers[PERF_REG_X86_AX] = regs.ax;
  registers[PERF_REG_X86_BX] = regs.bx;
  registers[PERF_REG_X86_CX] = regs.cx;
  registers[PERF_REG_X86_DX] = regs.dx;
  registers[PERF_REG_X86_SI] = regs.si;
  registers[PERF_REG_X86_DI] = regs.di;
  registers[PERF_REG_X86_BP] = regs.bp;
  registers[PERF_REG_X86_SP] = regs.sp;
  registers[PERF_REG_X86_IP] = regs.ip;
  registers[PERF_REG_X86_FLAGS] = regs.flags;
  registers[PERF_REG_X86_CS] = regs.cs;
  registers[PERF_REG_X86_SS] = regs.ss;
  // Registers ds, es, fs, gs do not actually exist.
  registers[PERF_REG_X86_DS] = 0ul;
  registers[PERF_REG_X86_ES] = 0ul;
  registers[PERF_REG_X86_FS] = 0ul;
  registers[PERF_REG_X86_GS] = 0ul;
  registers[PERF_REG_X86_R8] = regs.r8;
  registers[PERF_REG_X86_R9] = regs.r9;
  registers[PERF_REG_X86_R10] = regs.r10;
  registers[PERF_REG_X86_R11] = regs.r11;
  registers[PERF_REG_X86_R12] = regs.r12;
  registers[PERF_REG_X86_R13] = regs.r13;
  registers[PERF_REG_X86_R14] = regs.r14;
  registers[PERF_REG_X86_R15] = regs.r15;
  return registers;
}
}  // namespace

class StackSamplePerfEvent : public SamplePerfEvent<perf_event_sample> {
 public:
  std::array<uint64_t, PERF_REG_X86_64_MAX> GetRegisters() const {
    return perf_event_sample_regs_user_all_to_register_array(
        ring_buffer_record.regs);
  }

  const char* GetStackDump() const { return ring_buffer_record.stack.data; }
  uint64_t GetStackSize() const { return ring_buffer_record.stack.dyn_size; }

  void Accept(PerfEventVisitor* visitor) override;
};

template <typename perf_record_t>
class AbstractUprobesPerfEvent : public SamplePerfEvent<perf_record_t> {
 public:
  const Function* GetFunction() const { return function_; }
  void SetFunction(const Function* function) { function_ = function; }

 private:
  const Function* function_ = nullptr;
};

class UprobesPerfEvent : public AbstractUprobesPerfEvent<perf_event_empty> {
 public:
  void Accept(PerfEventVisitor* visitor) override;
};

class UprobesWithStackPerfEvent
    : public AbstractUprobesPerfEvent<perf_event_sample> {
 public:
  std::array<uint64_t, PERF_REG_X86_64_MAX> GetRegisters() const {
    return perf_event_sample_regs_user_all_to_register_array(
        ring_buffer_record.regs);
  }

  const char* GetStackDump() const { return ring_buffer_record.stack.data; }
  uint64_t GetStackSize() const { return ring_buffer_record.stack.dyn_size; }

  void Accept(PerfEventVisitor* visitor) override;
};

class UretprobesPerfEvent : public AbstractUprobesPerfEvent<perf_event_empty> {
 public:
  void Accept(PerfEventVisitor* visitor) override;
};

class UretprobesWithStackPerfEvent
    : public AbstractUprobesPerfEvent<perf_event_sample> {
 public:
  std::array<uint64_t, PERF_REG_X86_64_MAX> GetRegisters() const {
    return perf_event_sample_regs_user_all_to_register_array(
        ring_buffer_record.regs);
  }

  const char* GetStackDump() const { return ring_buffer_record.stack.data; }
  uint64_t GetStackSize() const { return ring_buffer_record.stack.dyn_size; }

  void Accept(PerfEventVisitor* visitor) override;
};

// This carries a snapshot of /proc/<pid>/maps and does not reflect a
// perf_event_open event, but we want it to be part of the same hierarchy.
class MapsPerfEvent : public PerfEvent {
 public:
  MapsPerfEvent(uint64_t timestamp, std::string maps)
      : timestamp_{timestamp}, maps_{std::move(maps)} {}

  uint64_t GetTimestamp() const override { return timestamp_; }

  const std::string& GetMaps() const { return maps_; }

  void Accept(PerfEventVisitor* visitor) override;

 private:
  uint64_t timestamp_;
  std::string maps_;
};

}  // namespace LinuxTracing

#endif  // ORBIT_LINUX_TRACING_PERF_EVENT_H_
