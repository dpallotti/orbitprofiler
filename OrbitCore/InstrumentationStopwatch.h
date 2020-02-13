#ifndef ORBIT_CORE_INSTRUMENTATION_STOPWATCH_H_
#define ORBIT_CORE_INSTRUMENTATION_STOPWATCH_H_

#include <initializer_list>

#include "Profiling.h"

class InstrumentationStopwatch {
 public:
  using CategoryKeyType = uint32_t;
  using CategoryNameType = std::string;

  InstrumentationStopwatch(
      std::initializer_list<std::pair<CategoryKeyType, CategoryNameType>>
          categories) {
    for (auto& category : categories) {
      categories_.emplace_back(category);
      starts_.emplace(category.first, 0);
      totals_.emplace(category.first, 0);
    }
  }

  InstrumentationStopwatch(const InstrumentationStopwatch&) = delete;
  InstrumentationStopwatch& operator=(const InstrumentationStopwatch&) = delete;
  InstrumentationStopwatch(InstrumentationStopwatch&&) = default;
  InstrumentationStopwatch& operator=(InstrumentationStopwatch&&) = default;
  ~InstrumentationStopwatch() = default;

  InstrumentationStopwatch& Start(CategoryKeyType category_key) {
    assert(starts_.count(category_key) > 0);
    assert(starts_.at(category_key) == 0);
    starts_.at(category_key) = CurrentTimestamp();
    return *this;
  }

  InstrumentationStopwatch& Stop(CategoryKeyType category_key) {
    assert(starts_.count(category_key) > 0);
    assert(totals_.count(category_key) > 0);
    assert(starts_.at(category_key) != 0);
    totals_.at(category_key) += CurrentTimestamp() - starts_.at(category_key);
    starts_.at(category_key) = 0;
    return *this;
  }

  InstrumentationStopwatch& StopAll() {
    for (const auto& category : categories_) {
      Stop(category.first);
    }
    return *this;
  }

  uint64_t TotalNs(CategoryKeyType category_key) {
    assert(starts_.count(category_key) > 0);
    assert(totals_.count(category_key) > 0);
    if (starts_.at(category_key) > 0) {
      totals_.at(category_key) += CurrentTimestamp() - starts_.at(category_key);
      starts_.at(category_key) = CurrentTimestamp();
    }
    return totals_.at(category_key);
  }

  uint64_t TotalNs() {
    uint64_t total_ns = 0;
    for (const auto& category : categories_) {
      total_ns += TotalNs(category.first);
    }
    return total_ns;
  }

  double TotalS(CategoryKeyType category_key) {
    return TotalNs(category_key) / 1e9;
  }

  double TotalS() {
    return TotalNs() / 1e9;
  }

  InstrumentationStopwatch& Reset() {
    for (const auto& category : categories_) {
      starts_.at(category.first) = 0;
      totals_.at(category.first) = 0;
    }
    return *this;
  }

  const std::vector<std::pair<CategoryKeyType, CategoryNameType>>&
  Categories() {
    return categories_;
  }

 private:
  std::vector<std::pair<CategoryKeyType, CategoryNameType>> categories_;
  std::unordered_map<CategoryKeyType, uint64_t> starts_;
  std::unordered_map<CategoryKeyType, uint64_t> totals_;

  static uint64_t CurrentTimestamp() { return OrbitTicks(CLOCK_MONOTONIC); }
};

enum InstrumentationCategoryKeys {
  CATEGORY_TRACING = 1,
  CATEGORY_UNWINDING = 2,
  CATEGORY_PROCESS_CONTEXT_SWITCH = 3,
  CATEGORY_HANDLE_CALLSTACK = 4,
  CATEGORY_HANDLE_TIMER = 5,
  CATEGORY_SLEEP = 6,
};

extern InstrumentationStopwatch gInstrumentationStopwatch;

#endif  // ORBIT_CORE_INSTRUMENTATION_STOPWATCH_H_
