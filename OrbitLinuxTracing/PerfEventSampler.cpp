#include "PerfEventSampler.h"

namespace LinuxTracing {

PerfEventSampler PerfEventSampler::OpenForContextSwitches(int32_t cpu) {
  return PerfEventSampler{context_switch_event_open(-1, cpu), 8192};
}

PerfEventSampler PerfEventSampler::OpenForMmapForkExit(int32_t cpu, pid_t pid) {
  PerfEventSampler sampler{mmap_task_event_open(-1, cpu), 64};
  sampler.SetFilteringPid(pid);
  return sampler;
}

PerfEventSampler PerfEventSampler::OpenForStackSamples(int32_t cpu, pid_t pid,
                                                       uint64_t period_ns) {
  PerfEventSampler sampler{sample_event_open(period_ns, -1, cpu), 8192};
  sampler.SetFilteringPid(pid);
  return sampler;
}

PerfEventSampler PerfEventSampler::OpenForUprobesWithStack(
    int32_t cpu, const Function* function, pid_t pid) {
  PerfEventSampler sampler{
      uprobe_stack_event_open(function->BinaryPath().c_str(),
                              function->FileOffset(), -1, cpu),
      8192};
  sampler.SetFilteringPid(pid);
  sampler.SetUprobesFunction(function);
  sampler.SetIsUretprobes(false);
  return sampler;
}

PerfEventSampler PerfEventSampler::OpenForUretprobes(int32_t cpu,
                                                     const Function* function,
                                                     pid_t pid) {
  PerfEventSampler sampler{
      uretprobe_event_open(function->BinaryPath().c_str(),
                           function->FileOffset(), -1, cpu),
      64};
  sampler.SetFilteringPid(pid);
  sampler.SetUprobesFunction(function);
  sampler.SetIsUretprobes(true);
  return sampler;
}

bool PerfEventSampler::HasNewEvent() {
  if (!IsOpen()) {
    return false;
  } else if (next_event_valid_) {
    return true;
  } else {
    while (ring_buffer_.HasNewData()) {
      std::unique_ptr<PerfEvent> event = ConsumeEvent();
      if (event != nullptr) {
        next_event_ = std::move(event);
        next_event_valid_ = true;
        return true;
      }
    }
    return false;
  }
}

std::unique_ptr<PerfEvent> PerfEventSampler::ReadEvent() {
  if (!IsOpen()) {
    return nullptr;
  } else if (next_event_valid_) {
    next_event_valid_ = false;
    return std::move(next_event_);
  } else {
    return ConsumeEvent();
  }
}

std::unique_ptr<PerfEvent> PerfEventSampler::ConsumeEvent() {
  if (!ring_buffer_.HasNewData()) {
    return nullptr;
  }

  if (!next_header_valid_) {
    next_header_ = ring_buffer_.ReadHeader();
    next_header_valid_ = true;
  }

  const uint32_t& record_type = next_header_.type;
  switch (record_type) {
    case PERF_RECORD_SWITCH: {
      // As OpenForContextSwitches calls perf_event_open for context switches
      // based on cpus, we only expect PERF_RECORD_SWITCH_CPU_WIDE records.
      ERROR(
          "PERF_RECORD_SWITCH instead of PERF_RECORD_SWITCH_CPU_WIDE "
          "unexpected");
      return SkipRecord();
    }

    case PERF_RECORD_SWITCH_CPU_WIDE: {
      return ConsumeSystemWideContextSwitch();
    }

    case PERF_RECORD_MMAP: {
      // Mmap records don't have a fixed layout, but pid is always right after
      // the header.
      pid_t pid = ReadPid(record_type);
      if (filtering_pid_ < 0 || pid == filtering_pid_) {
        return ConsumeMmap();
      } else {
        return SkipRecord();
      }
    }

    case PERF_RECORD_FORK: {
      pid_t pid = ReadPid(record_type);
      if (filtering_pid_ < 0 || pid == filtering_pid_) {
        return ConsumeFork();
      } else {
        return SkipRecord();
      }
    }

    case PERF_RECORD_EXIT: {
      pid_t pid = ReadPid(record_type);
      if (filtering_pid_ < 0 || pid == filtering_pid_) {
        return ConsumeExit();
      } else {
        return SkipRecord();
      }
    }

    case PERF_RECORD_SAMPLE: {
      pid_t pid = ReadPid(record_type);
      if (filtering_pid_ < 0 || pid == filtering_pid_) {
        if (uprobes_function_ == nullptr) {
          return ConsumeStackSample();
        } else if (!is_uretprobes_) {
          return ConsumeUprobesWithStack();
        } else {
          return ConsumeUretprobes();
        }
      } else {
        return SkipRecord();
      }
    }

    case PERF_RECORD_LOST: {
      return ConsumeLost();
    }

    default: {
      ERROR("Unexpected record type: %u", next_header_.type);
      return SkipRecord();
    }
  }
}

pid_t PerfEventSampler::ReadPid(uint32_t record_type) {
  switch (record_type) {
    case PERF_RECORD_SWITCH:
      return static_cast<pid_t>(ring_buffer_.ReadValueAtOffset<uint32_t>(
          offsetof(perf_event_context_switch, sample_id.pid)));

    case PERF_RECORD_SWITCH_CPU_WIDE:
      return static_cast<pid_t>(ring_buffer_.ReadValueAtOffset<uint32_t>(
          offsetof(perf_event_context_switch_cpu_wide, sample_id.pid)));

    case PERF_RECORD_MMAP:
      // Mmap records don't have a fixed layout, but pid is always right after
      // the header.
      return static_cast<pid_t>(
          ring_buffer_.ReadValueAtOffset<uint32_t>(sizeof(perf_event_header)));

    case PERF_RECORD_FORK:
      return static_cast<pid_t>(ring_buffer_.ReadValueAtOffset<uint32_t>(
          offsetof(perf_event_fork_exit, pid)));

    case PERF_RECORD_EXIT:
      return static_cast<pid_t>(ring_buffer_.ReadValueAtOffset<uint32_t>(
          offsetof(perf_event_fork_exit, pid)));

    case PERF_RECORD_SAMPLE:
      return static_cast<pid_t>(ring_buffer_.ReadValueAtOffset<uint32_t>(
          offsetof(perf_event_sample, sample_id.pid)));

    case PERF_RECORD_LOST:
      return -1;

    default:
      return -1;
  }
}

std::unique_ptr<MmapPerfEvent> PerfEventSampler::SkipRecord() {
  ring_buffer_.SkipRecordGivenHeader(next_header_);
  next_header_valid_ = false;
  return nullptr;
}

std::unique_ptr<SystemWideContextSwitchPerfEvent>
PerfEventSampler::ConsumeSystemWideContextSwitch() {
  auto context_switch = std::make_unique<SystemWideContextSwitchPerfEvent>();
  context_switch->ring_buffer_record =
      ring_buffer_.ConsumeRecordGivenHeader<perf_event_context_switch_cpu_wide>(
          next_header_);
  next_header_valid_ = false;
  return context_switch;
}

std::unique_ptr<MmapPerfEvent> PerfEventSampler::ConsumeMmap() {
  auto mmap_event = std::make_unique<MmapPerfEvent>();

  // Mmap records have the following layout:
  // struct {
  //   struct perf_event_header header;
  //   u32    pid, tid;
  //   u64    addr;
  //   u64    len;
  //   u64    pgoff;
  //   char   filename[];
  //   struct sample_id sample_id; /* if sample_id_all */
  // };
  // Because of filename, the layout is not fixed.

  uint64_t offset = 0;
  mmap_event->ring_buffer_header =
      ring_buffer_.ReadValueAtOffset<perf_event_header>(offset);
  offset += sizeof(perf_event_header);

  mmap_event->ring_buffer_pid =
      ring_buffer_.ReadValueAtOffset<uint32_t>(offset);
  offset += sizeof(uint32_t);

  mmap_event->ring_buffer_tid =
      ring_buffer_.ReadValueAtOffset<uint32_t>(offset);
  offset += sizeof(uint32_t);

  mmap_event->ring_buffer_addr =
      ring_buffer_.ReadValueAtOffset<uint64_t>(offset);
  offset += sizeof(uint64_t);

  mmap_event->ring_buffer_len =
      ring_buffer_.ReadValueAtOffset<uint64_t>(offset);
  offset += sizeof(uint64_t);

  mmap_event->ring_buffer_pgoff =
      ring_buffer_.ReadValueAtOffset<uint64_t>(offset);
  offset += sizeof(uint64_t);

  mmap_event->filename = "";
  while (true) {
    char c = ring_buffer_.ReadValueAtOffset<char>(offset);
    offset += sizeof(char);

    if (c == '\0') {
      break;
    }
    mmap_event->filename.push_back(c);
  }

  mmap_event->ring_buffer_sample_id =
      ring_buffer_.ReadValueAtOffset<perf_event_sample_id_tid_time_cpu>(offset);

  ring_buffer_.SkipRecordGivenHeader(next_header_);
  next_header_valid_ = false;
  return mmap_event;
}

std::unique_ptr<ForkPerfEvent> PerfEventSampler::ConsumeFork() {
  auto fork_event = std::make_unique<ForkPerfEvent>();
  fork_event->ring_buffer_record =
      ring_buffer_.ConsumeRecordGivenHeader<perf_event_fork_exit>(next_header_);
  next_header_valid_ = false;
  return fork_event;
}

std::unique_ptr<ExitPerfEvent> PerfEventSampler::ConsumeExit() {
  auto exit_event = std::make_unique<ExitPerfEvent>();
  exit_event->ring_buffer_record =
      ring_buffer_.ConsumeRecordGivenHeader<perf_event_fork_exit>(next_header_);
  next_header_valid_ = false;
  return exit_event;
}

std::unique_ptr<StackSamplePerfEvent> PerfEventSampler::ConsumeStackSample() {
  auto stack_sample_event = std::make_unique<StackSamplePerfEvent>();
  ReadSampleRecord(&stack_sample_event->ring_buffer_record);
  ring_buffer_.SkipRecordGivenHeader(next_header_);
  next_header_valid_ = false;
  return stack_sample_event;
}

std::unique_ptr<UprobesWithStackPerfEvent>
PerfEventSampler::ConsumeUprobesWithStack() {
  auto uprobes_event = std::make_unique<UprobesWithStackPerfEvent>();
  ReadSampleRecord(&uprobes_event->ring_buffer_record);
  uprobes_event->SetFunction(uprobes_function_);
  ring_buffer_.SkipRecordGivenHeader(next_header_);
  next_header_valid_ = false;
  return uprobes_event;
}

std::unique_ptr<UretprobesPerfEvent> PerfEventSampler::ConsumeUretprobes() {
  auto uretprobes_event = std::make_unique<UretprobesPerfEvent>();
  uretprobes_event->ring_buffer_record =
      ring_buffer_.ConsumeRecordGivenHeader<perf_event_empty>(next_header_);
  uretprobes_event->SetFunction(uprobes_function_);
  next_header_valid_ = false;
  return uretprobes_event;
}

void PerfEventSampler::ReadSampleRecord(perf_event_sample* sample) {
  sample->header = ring_buffer_.ReadValueAtOffset<perf_event_header>(
      offsetof(perf_event_sample, header));
  sample->sample_id =
      ring_buffer_.ReadValueAtOffset<perf_event_sample_id_tid_time_cpu>(
          offsetof(perf_event_sample, sample_id));
  sample->regs =
      ring_buffer_.ReadValueAtOffset<perf_event_sample_regs_user_all>(
          offsetof(perf_event_sample, regs));
  sample->stack.size = ring_buffer_.ReadValueAtOffset<uint64_t>(
      offsetof(perf_event_sample, stack.size));
  sample->stack.dyn_size = ring_buffer_.ReadValueAtOffset<uint64_t>(
      offsetof(perf_event_sample, stack.dyn_size));
  ring_buffer_.ReadRawAtOffset(sample->stack.data,
                               offsetof(perf_event_sample, stack.data),
                               sample->stack.dyn_size);
}

std::unique_ptr<LostPerfEvent> PerfEventSampler::ConsumeLost() {
  auto lost = std::make_unique<LostPerfEvent>();
  lost->ring_buffer_record =
      ring_buffer_.ConsumeRecordGivenHeader<perf_event_lost>(next_header_);
  next_header_valid_ = false;
  return lost;
}

}  // namespace LinuxTracing
