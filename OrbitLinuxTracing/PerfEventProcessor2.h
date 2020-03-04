#ifndef ORBIT_LINUX_TRACING_PERF_EVENT_PROCESSOR_2_H_
#define ORBIT_LINUX_TRACING_PERF_EVENT_PROCESSOR_2_H_

#include <ctime>
#include <memory>
#include <queue>

#include "PerfEvent.h"
#include "PerfEventVisitor.h"
#include "absl/container/flat_hash_map.h"

namespace LinuxTracing {

// This class implements a data structure that holds a large number of different
// perf_event_open records coming from multiple ring buffers, and allows reading
// them in order (oldest first).
// Instead of keeping a single priority queue with all the events to process,
// on which push/pop operations would be logarithmic in the number of events,
// we leverage the fact that events coming from the same perf_event_open ring
// buffer are already sorted. We then keep a priority queue of queues, where
// the events in each queue come from the same ring buffer. Whenever an event
// is removed from a queue, we need to move such queue down the priority
// queue. As std::priority_queue does not support decreasing the priority of
// an element, we achieve this by removing and re-inserting.
// In order to be able to add an event to a queue, we also need to maintain
// the association between a queue and its ring buffer. We use the file
// descriptor used to read from the ring buffer as identifier for a ring
// buffer. Keeping this association is what the pairs and the map are for.
// TODO: Implement a custom priority queue that supports decreasing the
//  priority.
class PerfEventQueue {
 public:
  void PushEvent(int origin_fd, std::unique_ptr<PerfEvent> event);
  bool HasEvent();
  PerfEvent* TopEvent();
  std::unique_ptr<PerfEvent> PopEvent();

 private:
  // Comparator for the priority queue: pop will return the queue associated
  // with the file descriptor from which the oldest event still to process
  // originated.
  struct QueueFrontTimestampReverseCompare {
    bool operator()(
        const std::pair<
            int, std::shared_ptr<std::queue<std::unique_ptr<PerfEvent>>>>& lhs,
        const std::pair<
            int, std::shared_ptr<std::queue<std::unique_ptr<PerfEvent>>>>&
            rhs) {
      return lhs.second->front()->GetTimestamp() >
             rhs.second->front()->GetTimestamp();
    }
  };

  std::priority_queue<
      std::pair<int, std::shared_ptr<std::queue<std::unique_ptr<PerfEvent>>>>,
      std::vector<std::pair<
          int, std::shared_ptr<std::queue<std::unique_ptr<PerfEvent>>>>>,
      QueueFrontTimestampReverseCompare>
      event_queues_queue_{};
  absl::flat_hash_map<int,
                      std::shared_ptr<std::queue<std::unique_ptr<PerfEvent>>>>
      fd_event_queues_{};
};

// This class receives perf_event_open events coming from several ring buffers
// and processes them in order according to their timestamps.
// Its implementation builds on the assumption that we never expect events with
// a timestamp older than PROCESSING_DELAY_MS to be added. By not processing
// events that are not older than this delay, we will never process events out
// of order.
class PerfEventProcessor2 {
 public:
  // Do not process events that are more recent than 0.1 seconds. There could be
  // events coming out of order as they are read from different perf_event_open
  // ring buffers and this ensure that all events are processed in the correct
  // order.
  static constexpr uint64_t PROCESSING_DELAY_MS = 100;

  explicit PerfEventProcessor2(std::unique_ptr<PerfEventVisitor> visitor)
      : visitor_(std::move(visitor)) {}

  PerfEventProcessor2(const PerfEventProcessor2&) = delete;
  PerfEventProcessor2& operator=(const PerfEventProcessor2&) = delete;

  PerfEventProcessor2(PerfEventProcessor2&& o) = default;
  PerfEventProcessor2& operator=(PerfEventProcessor2&& o) = default;

  void AddEvent(int origin_fd, std::unique_ptr<PerfEvent> event);

  void ProcessAllEvents();

  void ProcessOldEvents();

 private:
  PerfEventQueue event_queue_;
  std::unique_ptr<PerfEventVisitor> visitor_;

#ifndef NDEBUG
  uint64_t last_processed_timestamp_ = 0;
#endif
};

}  // namespace LinuxTracing

#endif  // ORBIT_LINUX_TRACING_PERF_EVENT_PROCESSOR_2_H_
