#include "render/d3d12/upload_ring.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

#include "core/log.h"
#include "render/d3d12/buffer.h"
#include "render/frame_config.h"

namespace blue::render::d3d12 {

UploadRing::UploadRing(const D3d12Context& context)
    : buffer_(context, kCapacity, Buffer::Access::kUpload,
              L"blue upload ring") {}

UploadAllocation UploadRing::Allocate(uint64_t size) {
  const std::optional<uint64_t> offset =
      ring_.Allocate(size, kConstantAlignment);
  if (!offset) {
    core::log::Fatal("Upload ring full: {} of {} bytes in use, {} asked for",
                     ring_.used(), ring_.capacity(), size);
  }
  return {.cpu = static_cast<std::byte*>(buffer_.mapped()) + *offset,
          .gpu = buffer_.gpu_address() + *offset};
}

D3D12_GPU_VIRTUAL_ADDRESS UploadRing::Push(std::span<const std::byte> bytes) {
  const UploadAllocation allocation = Allocate(bytes.size_bytes());
  std::memcpy(allocation.cpu, bytes.data(), bytes.size_bytes());
  return allocation.gpu;
}

void UploadRing::BeginFrame(uint64_t frame_serial) {
  if (frame_serial > kFramesInFlight) {
    ring_.Release(frame_serial - kFramesInFlight);
  }
}

void UploadRing::EndFrame(uint64_t frame_serial) {
  ring_.EndFrame(frame_serial);
}

}  // namespace blue::render::d3d12
