#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <set>

#include "render/d3d12/slot_allocator.h"
#include "render/frame_config.h"

namespace blue::render::d3d12 {
namespace {

TEST_CASE("Slots are handed out in order until the region is full") {
  SlotAllocator slots(3);
  CHECK(slots.Allocate() == 0u);
  CHECK(slots.Allocate() == 1u);
  CHECK(slots.Allocate() == 2u);
  CHECK(slots.used() == 3);
  CHECK_FALSE(slots.Allocate().has_value());
}

TEST_CASE("A freed slot is reused at once, most recent first") {
  SlotAllocator slots(4);
  const uint32_t a = *slots.Allocate();
  const uint32_t b = *slots.Allocate();
  slots.Free(a);
  slots.Free(b);
  CHECK(slots.used() == 0);
  CHECK(slots.Allocate() == b);
  CHECK(slots.Allocate() == a);
  CHECK(slots.Allocate() == 2u);
}

TEST_CASE("A retired slot is reused only after kFramesInFlight frames") {
  SlotAllocator slots(1);
  const uint64_t frame = 10;
  slots.BeginFrame(frame);
  const uint32_t slot = *slots.Allocate();
  slots.Retire(slot, frame);
  CHECK(slots.retired() == 1);
  CHECK(slots.used() == 1);  // Still held while frames may read it.
  // Frames up to frame + kFramesInFlight - 1 may still be in flight.
  for (uint64_t later = frame; later < frame + kFramesInFlight; ++later) {
    slots.BeginFrame(later);
    CHECK_FALSE(slots.Allocate().has_value());
  }
  slots.BeginFrame(frame + kFramesInFlight);
  CHECK(slots.retired() == 0);
  CHECK(slots.Allocate() == slot);
}

TEST_CASE("Retired slots come back in the order their frames finish") {
  SlotAllocator slots(8);
  slots.BeginFrame(1);
  const uint32_t first = *slots.Allocate();
  const uint32_t second = *slots.Allocate();
  slots.Retire(first, 1);
  slots.BeginFrame(2);
  slots.Retire(second, 2);
  slots.BeginFrame(1 + kFramesInFlight);
  CHECK(slots.retired() == 1);
  slots.BeginFrame(2 + kFramesInFlight);
  CHECK(slots.retired() == 0);
  CHECK(slots.used() == 0);
}

TEST_CASE("Reclaiming frees every retired slot now") {
  SlotAllocator slots(2);
  slots.Retire(*slots.Allocate(), 5);
  slots.Retire(*slots.Allocate(), 6);
  CHECK_FALSE(slots.Allocate().has_value());
  slots.ReclaimRetired();
  CHECK(slots.used() == 0);
  CHECK(slots.Allocate().has_value());
  CHECK(slots.Allocate().has_value());
}

TEST_CASE("Churn never hands out a slot twice") {
  SlotAllocator slots(16);
  std::set<uint32_t> live;
  uint64_t frame = 1;
  for (int step = 0; step < 1000; ++step, ++frame) {
    slots.BeginFrame(frame);
    const std::optional<uint32_t> slot = slots.Allocate();
    if (slot) {
      CHECK(live.insert(*slot).second);
    }
    if (live.size() > 8) {
      const uint32_t oldest = *live.begin();
      live.erase(live.begin());
      slots.Retire(oldest, frame);
    }
  }
  CHECK(slots.used() >= live.size());
}

}  // namespace
}  // namespace blue::render::d3d12
