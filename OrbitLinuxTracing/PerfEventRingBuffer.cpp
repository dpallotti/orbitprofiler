#include "PerfEventRingBuffer.h"

#include <linux/perf_event.h>
#include <sys/mman.h>

#include <cassert>
#include <cstring>
#include <utility>

#include "Logging.h"
#include "PerfEventOpen.h"

namespace LinuxTracing {

PerfEventRingBuffer::PerfEventRingBuffer(int perf_event_fd, uint64_t size_kb) {
  if (perf_event_fd < 0) {
    return;
  }

  static const auto PAGE_SIZE = static_cast<uint64_t>(getpagesize());

  // The size of a perf_event_open ring buffer is required to be a power of two
  // memory pages (from perf_event_open's manpage: "The mmap size should be
  // 1+2^n pages"), otherwise mmap on the file descriptor fails.
  if (1024 * size_kb < PAGE_SIZE || __builtin_popcountl(size_kb) != 1) {
    return;
  }

  ring_buffer_size_ = 1024 * size_kb;
  ring_buffer_size_log2_ = __builtin_ffsl(ring_buffer_size_) - 1;
  mmap_length_ = PAGE_SIZE + ring_buffer_size_;

  void* mmap_address =
      perf_event_open_mmap_ring_buffer(perf_event_fd, mmap_length_);
  if (mmap_address == nullptr) {
    return;
  }

  // The first page, just before the ring buffer, is the metadata page.
  metadata_page_ = reinterpret_cast<perf_event_mmap_page*>(mmap_address);
  assert(metadata_page_->data_size == ring_buffer_size_);

  ring_buffer_ =
      reinterpret_cast<char*>(mmap_address) + metadata_page_->data_offset;
  assert(metadata_page_->data_offset == PAGE_SIZE);
}

PerfEventRingBuffer::PerfEventRingBuffer(PerfEventRingBuffer&& o) noexcept {
  std::swap(mmap_length_, o.mmap_length_);
  std::swap(metadata_page_, o.metadata_page_);
  std::swap(ring_buffer_, o.ring_buffer_);
  std::swap(ring_buffer_size_, o.ring_buffer_size_);
  std::swap(ring_buffer_size_log2_, o.ring_buffer_size_log2_);
}

PerfEventRingBuffer& PerfEventRingBuffer::operator=(
    PerfEventRingBuffer&& o) noexcept {
  if (&o != this) {
    std::swap(mmap_length_, o.mmap_length_);
    std::swap(metadata_page_, o.metadata_page_);
    std::swap(ring_buffer_, o.ring_buffer_);
    std::swap(ring_buffer_size_, o.ring_buffer_size_);
    std::swap(ring_buffer_size_log2_, o.ring_buffer_size_log2_);
  }
  return *this;
}

PerfEventRingBuffer::~PerfEventRingBuffer() {
  if (metadata_page_ != nullptr) {
    int munmap_ret = munmap(metadata_page_, mmap_length_);
    if (munmap_ret != 0) {
      ERROR("munmap: %s", strerror(errno));
    }
  }
}

bool PerfEventRingBuffer::HasNewData() {
  assert(metadata_page_->data_tail == metadata_page_->data_head ||
         metadata_page_->data_head >=
             metadata_page_->data_tail + sizeof(perf_event_header));
  return metadata_page_->data_head > metadata_page_->data_tail;
}

perf_event_header PerfEventRingBuffer::ReadHeader() {
  perf_event_header header{};
  ReadAtTail(reinterpret_cast<uint8_t*>(&header), sizeof(perf_event_header));
  assert(header.type != 0);
  assert(metadata_page_->data_tail + header.size <= metadata_page_->data_head);
  return header;
}

void PerfEventRingBuffer::SkipRecordGivenHeader(
    const perf_event_header& header) {
  // Write back how far we read from the buffer.
  metadata_page_->data_tail += header.size;
}

void PerfEventRingBuffer::SkipRecord() { SkipRecordGivenHeader(ReadHeader()); }

void PerfEventRingBuffer::ReadAtTail(uint8_t* dest, uint64_t count) {
  return ReadAtOffsetFromTail(dest, 0, count);
}

void PerfEventRingBuffer::ReadAtOffsetFromTail(uint8_t* dest,
                                               uint64_t offset_from_tail,
                                               uint64_t count) {
  if (offset_from_tail + count >
      metadata_page_->data_head - metadata_page_->data_tail) {
    ERROR("Reading more data than it is available from the ring buffer");
  } else if (count > ring_buffer_size_) {
    ERROR("Reading more than the size of the ring buffer");
  } else if (metadata_page_->data_head >
             metadata_page_->data_tail + ring_buffer_size_) {
    // If mmap has been called with PROT_WRITE and
    // perf_event_mmap_page::data_tail is used properly, this should not happen,
    // as the kernel would not overwrite unread data.
    ERROR("Too slow reading from the ring buffer");
  }

  const uint64_t index = metadata_page_->data_tail + offset_from_tail;
  const uint32_t exponent = ring_buffer_size_log2_;

  // As ring_buffer_size_ is a power of two, optimize index % ring_buffer_size_:
  const uint64_t index_mod_size = index & (ring_buffer_size_ - 1);

  // Optimize index / ring_buffer_size_:
  const uint64_t index_div_size = index >> exponent;

  const uint64_t last_index = index + count - 1;
  // Optimize (index + count - 1) / ring_buffer_size_:
  const uint64_t last_index_div_size = last_index >> exponent;

  if (index_div_size == last_index_div_size) {
    memcpy(dest, ring_buffer_ + index_mod_size, count);
  } else if (index_div_size == last_index_div_size - 1) {
    // Need two copies as the data to read wraps around the ring buffer.
    memcpy(dest, ring_buffer_ + index_mod_size,
           ring_buffer_size_ - index_mod_size);
    memcpy(dest + (ring_buffer_size_ - index_mod_size), ring_buffer_,
           count - (ring_buffer_size_ - index_mod_size));
  } else {
    assert(false);  // Control shouldn't reach here.
  }
}

}  // namespace LinuxTracing
