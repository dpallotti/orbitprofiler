#ifndef ORBIT_LINUX_TRACING_PERF_EVENT_VISITOR_H_
#define ORBIT_LINUX_TRACING_PERF_EVENT_VISITOR_H_

#include "PerfEvent.h"

namespace LinuxTracing {

// Keep this class in sync with the hierarchy of PerfEvent in PerfEvent.h.
class PerfEventVisitor {
 public:
  virtual ~PerfEventVisitor() = default;
  virtual void visit(ContextSwitchPerfEvent* event) {}
  virtual void visit(SystemWideContextSwitchPerfEvent* event) {}
  virtual void visit(MmapPerfEvent* event) {}
  virtual void visit(ForkPerfEvent* event) {}
  virtual void visit(ExitPerfEvent* event) {}
  virtual void visit(StackSamplePerfEvent* event) {}
  virtual void visit(UprobesPerfEvent* event) {}
  virtual void visit(UprobesWithStackPerfEvent* event) {}
  virtual void visit(UretprobesPerfEvent* event) {}
  virtual void visit(UretprobesWithStackPerfEvent* event) {}
  virtual void visit(LostPerfEvent* event) {}
  virtual void visit(MapsPerfEvent* event) {}
};

}  // namespace LinuxTracing

#endif  // ORBIT_LINUX_TRACING_PERF_EVENT_VISITOR_H_
