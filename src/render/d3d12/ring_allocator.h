#ifndef BLUE_RENDER_D3D12_RING_ALLOCATOR_H_
#define BLUE_RENDER_D3D12_RING_ALLOCATOR_H_

#include <cstdint>
#include <optional>
#include <vector>

// Offset bookkeeping of the per-frame rings (the upload ring of constants,
// the transient region of the shader-visible descriptor heap): pure logic,
// no device (unit tested without a GPU).

namespace blue::render::d3d12 {

// Sub-allocates a region of `capacity` units (bytes, descriptors) in a ring:
// each frame allocates after the previous one and its space comes back when
// the GPU has finished that frame (Release()). An allocation never straddles
// the end of the region: when it does not fit before the end, the rest of
// the region is skipped (and stays used until its frame is released) and
// the allocation starts at 0. Allocates memory only for its first frames
// (no allocation once warm). Not thread-safe.
class RingAllocator {
 public:
  explicit RingAllocator(uint64_t capacity);

  // Returns the offset of `size` units aligned to `alignment` (a power of
  // two), or nullopt when the unreleased frames leave no room (or `size`
  // exceeds the capacity).
  [[nodiscard]] std::optional<uint64_t> Allocate(uint64_t size,
                                                 uint64_t alignment);
  // Ends frame `frame_serial`: what was allocated since the previous
  // EndFrame() belongs to it.
  void EndFrame(uint64_t frame_serial);
  // Gives back what the frames up to `completed_frame` allocated (their GPU
  // work has finished).
  void Release(uint64_t completed_frame);

  [[nodiscard]] uint64_t capacity() const { return capacity_; }
  // Units in use: allocations, alignment padding and skipped ends of the
  // unreleased frames (and of the frame being recorded).
  [[nodiscard]] uint64_t used() const { return consumed_ - released_; }

 private:
  struct FrameEnd {
    uint64_t frame_serial = 0;
    uint64_t consumed = 0;  // consumed_ at the frame's end.
  };

  uint64_t capacity_;
  // The next free offset.
  uint64_t head_ = 0;
  // Units ever consumed (allocations, padding, skipped ends) and ever given
  // back; used() is the difference.
  uint64_t consumed_ = 0;
  uint64_t released_ = 0;
  // Ended, unreleased frames, oldest first (at most a few).
  std::vector<FrameEnd> frames_;
};

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_RING_ALLOCATOR_H_
