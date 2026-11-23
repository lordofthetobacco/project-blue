#include "render/d3d12/slot_allocator.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "core/log.h"
#include "render/frame_config.h"

namespace blue::render::d3d12 {

SlotAllocator::SlotAllocator(uint32_t capacity) : capacity_(capacity) {}

std::optional<uint32_t> SlotAllocator::Allocate() {
  uint32_t slot = 0;
  if (!free_.empty()) {
    slot = free_.back();
    free_.pop_back();
  } else if (next_unused_ < capacity_) {
    slot = next_unused_++;
    allocated_.push_back(false);
  } else {
    return std::nullopt;
  }
  allocated_[slot] = true;
  return slot;
}

void SlotAllocator::Free(uint32_t slot) {
  if (slot >= next_unused_ || !allocated_[slot]) {
    core::log::Fatal("Slot {} given back but not allocated", slot);
  }
  allocated_[slot] = false;
  free_.push_back(slot);
}

void SlotAllocator::Retire(uint32_t slot, uint64_t last_frame) {
  if (slot >= next_unused_ || !allocated_[slot]) {
    core::log::Fatal("Slot {} retired but not allocated", slot);
  }
  retired_.push_back({.slot = slot, .last_frame = last_frame});
}

void SlotAllocator::BeginFrame(uint64_t frame_serial) {
  // Retired in frame order, so the finished ones are a prefix; usually
  // empty or a handful.
  size_t done = 0;
  while (done < retired_.size() &&
         retired_[done].last_frame + kFramesInFlight <= frame_serial) {
    Free(retired_[done].slot);
    ++done;
  }
  retired_.erase(retired_.begin(),
                 retired_.begin() + static_cast<std::ptrdiff_t>(done));
}

void SlotAllocator::ReclaimRetired() {
  for (const Retired& retired : retired_) {
    Free(retired.slot);
  }
  retired_.clear();
}

}  // namespace blue::render::d3d12
