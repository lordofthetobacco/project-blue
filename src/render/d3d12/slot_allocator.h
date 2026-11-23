#ifndef BLUE_RENDER_D3D12_SLOT_ALLOCATOR_H_
#define BLUE_RENDER_D3D12_SLOT_ALLOCATOR_H_

#include <cstdint>
#include <optional>
#include <vector>

// Index bookkeeping behind the descriptor heaps and the material parameter
// pages: pure logic, no device (unit tested without a GPU).

namespace blue::render::d3d12 {

// Hands out the slots 0 .. capacity - 1 of a fixed-size region (descriptors
// of a heap, parameter blocks of a buffer). A slot given back with Retire()
// may still be read by frames in flight, so it is reused only once the frame
// that last used it has finished, i.e. kFramesInFlight frames later
// (BeginFrame()); Free() gives a slot back at once. Freed slots are reused
// most recent first; untouched slots are handed out in increasing order.
// Allocates memory only while the counts grow (no allocation once warm).
// Not thread-safe.
class SlotAllocator {
 public:
  explicit SlotAllocator(uint32_t capacity);

  // Returns a free slot, or nullopt when all are in use or retired.
  [[nodiscard]] std::optional<uint32_t> Allocate();
  // Gives `slot` back for immediate reuse (nothing on the GPU can still
  // read it). Giving back a slot that is not allocated is fatal.
  void Free(uint32_t slot);
  // Gives `slot` back once frame `last_frame` (the last frame serial that
  // may use it) has finished: reusable from BeginFrame(last_frame +
  // kFramesInFlight) on.
  void Retire(uint32_t slot, uint64_t last_frame);
  // Starts frame `frame_serial`: the frames up to frame_serial -
  // kFramesInFlight have finished, so the slots they retired become free.
  void BeginFrame(uint64_t frame_serial);
  // Frees every retired slot now (the GPU is idle).
  void ReclaimRetired();

  [[nodiscard]] uint32_t capacity() const { return capacity_; }
  // Slots allocated and not given back, retired ones included.
  [[nodiscard]] uint32_t used() const {
    return next_unused_ - static_cast<uint32_t>(free_.size());
  }
  [[nodiscard]] uint32_t retired() const {
    return static_cast<uint32_t>(retired_.size());
  }

 private:
  struct Retired {
    uint32_t slot = 0;
    uint64_t last_frame = 0;
  };

  uint32_t capacity_;
  // Slots below this were handed out at least once.
  uint32_t next_unused_ = 0;
  std::vector<uint32_t> free_;
  std::vector<Retired> retired_;
  // Debug aid: allocated (and not freed) state per handed-out slot.
  std::vector<bool> allocated_;
};

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_SLOT_ALLOCATOR_H_
