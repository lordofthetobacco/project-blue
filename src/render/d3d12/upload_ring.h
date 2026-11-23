#ifndef BLUE_RENDER_D3D12_UPLOAD_RING_H_
#define BLUE_RENDER_D3D12_UPLOAD_RING_H_

#include <d3d12.h>

#include <cstddef>
#include <cstdint>
#include <span>

#include "render/d3d12/buffer.h"
#include "render/d3d12/ring_allocator.h"

namespace blue::render::d3d12 {

class D3d12Context;

// A piece of the upload ring: where the CPU writes it and where the GPU
// reads it.
struct UploadAllocation {
  std::byte* cpu = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
};

// Per-frame constants (FrameUniforms, large pass constants) in one
// persistently mapped upload buffer, sub-allocated in a ring with
// kConstantAlignment (the constant buffer view alignment): what a frame
// allocates is reusable once its fence is reached (BeginFrame()). The
// counterpart of the Metal backend's transient arena. Running out (far
// beyond what a frame uses) is fatal. Neither copyable nor movable. Not
// thread-safe.
class UploadRing {
 public:
  static constexpr uint64_t kCapacity = uint64_t{4} << 20;
  static constexpr uint64_t kConstantAlignment =
      D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;

  explicit UploadRing(const D3d12Context& context);

  UploadRing(const UploadRing&) = delete;
  UploadRing& operator=(const UploadRing&) = delete;

  // Returns `size` bytes for the frame being recorded.
  [[nodiscard]] UploadAllocation Allocate(uint64_t size);
  // Copies `bytes` into the ring and returns their GPU address.
  [[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS Push(std::span<const std::byte> bytes);

  // Starts recording frame `frame_serial` (the frames up to frame_serial -
  // kFramesInFlight have finished).
  void BeginFrame(uint64_t frame_serial);
  void EndFrame(uint64_t frame_serial);

  [[nodiscard]] uint64_t size_bytes() const { return buffer_.size(); }

 private:
  Buffer buffer_;
  RingAllocator ring_{kCapacity};
};

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_UPLOAD_RING_H_
