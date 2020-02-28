#ifndef ORBIT_LINUX_TRACING_UNIQUE_FD_H_
#define ORBIT_LINUX_TRACING_UNIQUE_FD_H_

#include <unistd.h>

#include <utility>

class UniqueFd {
 public:
  UniqueFd() = default;
  explicit UniqueFd(int file_descriptor) : file_descriptor_{file_descriptor} {}

  ~UniqueFd() {
    if (file_descriptor_ != -1) {
      close(file_descriptor_);
    }
  }

  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  UniqueFd(UniqueFd&& o) noexcept {
    std::swap(file_descriptor_, o.file_descriptor_);
  }

  UniqueFd& operator=(UniqueFd&& o) noexcept {
    std::swap(file_descriptor_, o.file_descriptor_);
    return *this;
  }

  int Get() const { return file_descriptor_; }

  bool Ok() const { return file_descriptor_ >= 0; }

  bool operator==(const UniqueFd& rhs) const { return Get() == rhs.Get(); }
  bool operator!=(const UniqueFd& rhs) const { return Get() != rhs.Get(); }

  int Release() {
    int ret = file_descriptor_;
    file_descriptor_ = -1;
    return ret;
  }

  void Reset(int new_file_descriptor = -1) {
    if (file_descriptor_ >= 0) {
      close(file_descriptor_);
    }

    file_descriptor_ = new_file_descriptor;
  }

 private:
  int file_descriptor_ = -1;
};

#endif  // ORBIT_LINUX_TRACING_UNIQUE_FD_H_
