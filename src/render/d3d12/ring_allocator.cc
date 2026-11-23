#include "render/d3d12/ring_allocator.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "render/d3d12/format.h"

namespace blue::render::d3d12 {

RingAllocator::RingAllocator(uint64_t capacity) : capacity_(capacity) {}

std::optional<uint64_t> RingAllocator::Allocate(uint64_t size,
                                                uint64_t alignment) {
  if (size == 0 || size > capacity_) {
    return std::nullopt;
  }
  if (used() == 0) {
    // Nothing in flight: start over at 0, the best place for anything.
    head_ = 0;
  }
  uint64_t offset = AlignUp(head_, alignment);
  uint64_t cost = (offset - head_) + size;
  if (offset + size > capacity_) {
    // Skip the end of the region; 0 is aligned to anything.
    offset = 0;
    cost = (capacity_ - head_) + size;
  }
  if (cost > capacity_ - used()) {
    return std::nullopt;
  }
  consumed_ += cost;
  head_ = offset + size;
  if (head_ == capacity_) {
    head_ = 0;
  }
  return offset;
}

void RingAllocator::EndFrame(uint64_t frame_serial) {
  frames_.push_back({.frame_serial = frame_serial, .consumed = consumed_});
}

void RingAllocator::Release(uint64_t completed_frame) {
  size_t done = 0;
  while (done < frames_.size() &&
         frames_[done].frame_serial <= completed_frame) {
    released_ = frames_[done].consumed;
    ++done;
  }
  frames_.erase(frames_.begin(),
                frames_.begin() + static_cast<std::ptrdiff_t>(done));
}

}  // namespace blue::render::d3d12
