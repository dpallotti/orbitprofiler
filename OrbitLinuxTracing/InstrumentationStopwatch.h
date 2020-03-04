#ifndef ORBIT_LINUX_TRACING_INSTRUMENTATION_STOPWATCH_H_
#define ORBIT_LINUX_TRACING_INSTRUMENTATION_STOPWATCH_H_

#include <initializer_list>

#include "Utils.h"
#include "absl/container/flat_hash_map.h"

namespace LinuxTracing {

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

  InstrumentationStopwatch& StopAllAndStart(CategoryKeyType category_key) {
    StopAll();
    assert(starts_.count(category_key) > 0);
    starts_.at(category_key) = CurrentTimestamp();
    return *this;
  }

  InstrumentationStopwatch& StopAll() {
    for (const auto& category : categories_) {
      const CategoryKeyType& category_key = category.first;
      if (starts_.at(category.first) != 0) {
        totals_.at(category_key) +=
            CurrentTimestamp() - starts_.at(category_key);
        starts_.at(category_key) = 0;
      }
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

  double TotalS() { return TotalNs() / 1e9; }

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

  void Print(double total_window = 0.0, FILE* file = stdout) {
    double total = total_window != 0.0 ? total_window : TotalS();
    fprintf(file, "Instrumentation (over %.6f s): ", total);
    fprintf(file, "%s: %.6f (%.1f%%); ", "TOTAL", TotalS(),
            TotalS() * 100.0 / total);
    for (const auto& category : Categories()) {
      fprintf(file, "%s: %.6f (%.1f%%); ", category.second.c_str(),
              TotalS(category.first), TotalS(category.first) * 100.0 / total);
    }
    fprintf(file, "\n");
  }

 private:
  std::vector<std::pair<CategoryKeyType, CategoryNameType>> categories_;
  absl::flat_hash_map<CategoryKeyType, uint64_t> starts_;
  absl::flat_hash_map<CategoryKeyType, uint64_t> totals_;

  static uint64_t CurrentTimestamp() { return MonotonicTimestampNs(); }
};

enum InstrumentationCategoryKeys {
  CATEGORY_TRACING = 1,
  CATEGORY_UNWINDING = 2,
  CATEGORY_LISTENER = 3,
  CATEGORY_SLEEP = 4,
};

extern InstrumentationStopwatch gInstrumentationStopwatch;

}  // namespace LinuxTracing

#endif  // ORBIT_LINUX_TRACING_INSTRUMENTATION_STOPWATCH_H_
