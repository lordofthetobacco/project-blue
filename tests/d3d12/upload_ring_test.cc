#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <vector>

#include "render/d3d12/ring_allocator.h"

namespace blue::render::d3d12 {
namespace {

TEST_CASE("Ring allocations are aligned and follow each other") {
  RingAllocator ring(1024);
  CHECK(ring.Allocate(10, 256) == 0u);
  CHECK(ring.Allocate(10, 256) == 256u);
  CHECK(ring.Allocate(4, 4) == 268u);
  CHECK(ring.used() == 272);
}

TEST_CASE("A finished frame's space comes back") {
  RingAllocator ring(1024);
  REQUIRE(ring.Allocate(512, 256).has_value());
  ring.EndFrame(1);
  REQUIRE(ring.Allocate(512, 256).has_value());
  ring.EndFrame(2);
  CHECK_FALSE(ring.Allocate(1, 1).has_value());  // Both frames in flight.
  ring.Release(1);
  CHECK(ring.used() == 512);
  CHECK(ring.Allocate(256, 256) == 0u);  // Wrapped into frame 1's space.
}

TEST_CASE("The ring wraps when its head reaches the end") {
  RingAllocator ring(1024);
  REQUIRE(ring.Allocate(256, 256) == 0u);
  ring.EndFrame(1);
  REQUIRE(ring.Allocate(512, 256) == 256u);
  ring.EndFrame(2);
  ring.Release(1);  // [0, 256) free again; the head is at 768.
  // 512 bytes do not fit in [768, 1024), and skipping that end leaves only
  // [0, 256) before frame 2's space.
  CHECK_FALSE(ring.Allocate(512, 256).has_value());
  // 256 bytes still fit before the end.
  REQUIRE(ring.Allocate(256, 256) == 768u);
  ring.EndFrame(3);
  ring.Release(2);
  CHECK(ring.Allocate(256, 256) == 0u);  // The head wrapped exactly at 1024.
}

TEST_CASE("Skipped space at the end stays used until its frame is released") {
  RingAllocator ring(1000);
  REQUIRE(ring.Allocate(600, 8) == 0u);
  ring.EndFrame(1);
  ring.Release(1);
  // Nothing in flight: the ring starts over at 0.
  REQUIRE(ring.Allocate(600, 8) == 0u);
  ring.EndFrame(2);
  REQUIRE(ring.Allocate(300, 8) == 600u);
  ring.EndFrame(3);
  ring.Release(2);
  // Head at 900: 200 bytes do not fit in [900, 1000), so 100 are skipped.
  REQUIRE(ring.Allocate(200, 8) == 0u);
  CHECK(ring.used() == 300 + 100 + 200);
  ring.EndFrame(4);
  ring.Release(3);
  CHECK(ring.used() == 100 + 200);  // The skipped end belongs to frame 4.
  ring.Release(4);
  CHECK(ring.used() == 0);
}

TEST_CASE("A ring never hands out overlapping live ranges") {
  constexpr uint64_t kCapacity = 16384;
  RingAllocator ring(kCapacity);
  // Each frame allocates a few odd sizes; two frames are in flight.
  struct Range {
    uint64_t begin = 0;
    uint64_t end = 0;
    uint64_t frame = 0;
  };
  std::vector<Range> live;
  for (uint64_t frame = 1; frame < 500; ++frame) {
    if (frame > 2) {
      ring.Release(frame - 2);
      std::erase_if(live, [frame](const Range& r) { return r.frame <= frame - 2; });
    }
    for (uint64_t i = 0; i < 3; ++i) {
      const uint64_t size = 64 + (frame * 37 + i * 101) % 500;
      const std::optional<uint64_t> offset = ring.Allocate(size, 256);
      REQUIRE(offset.has_value());
      CHECK(*offset % 256 == 0);
      CHECK(*offset + size <= kCapacity);
      for (const Range& other : live) {
        CHECK((*offset + size <= other.begin || *offset >= other.end));
      }
      live.push_back({*offset, *offset + size, frame});
    }
    ring.EndFrame(frame);
  }
}

TEST_CASE("Oversized or empty requests fail") {
  RingAllocator ring(256);
  CHECK_FALSE(ring.Allocate(0, 1).has_value());
  CHECK_FALSE(ring.Allocate(257, 1).has_value());
  CHECK(ring.Allocate(256, 256) == 0u);
}

}  // namespace
}  // namespace blue::render::d3d12
