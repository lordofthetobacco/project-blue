#ifndef BLUE_RENDER_BUFFER_H_
#define BLUE_RENDER_BUFFER_H_

#include "render/metal_types.h"

#include <cstddef>
#include <span>

#include "render/metal_context.h"

namespace blue::render {

// A Metal buffer, resident on the context's queue for its lifetime (see
// MetalContext). Move-only; a moved-from buffer is empty (nil handle, size
// 0, no mapping). Not thread-safe.
class Buffer {
 public:
  enum class Access {
    kGpuOnly,             // Private storage, filled through a staging upload.
    kCpuSequentialWrite,  // Shared, write-combined: written often by the CPU,
                          // never read back.
    kCpuRead,             // Shared, cached: written by the GPU, read back by
                          // the CPU once that work has completed.
  };

  Buffer() = default;
  // Creates a buffer of `size` bytes, which must not be 0, on `context`,
  // which must outlive it. `label` names it in GPU captures; nullptr gives a
  // generic name.
  Buffer(const MetalContext& context, size_t size, Access access,
         const char* label = nullptr);

  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;
  Buffer(Buffer&&) = default;
  Buffer& operator=(Buffer&&) = default;

  [[nodiscard]] id<MTLBuffer> handle() const { return buffer_; }
  [[nodiscard]] size_t size() const { return buffer_.length; }
  // Returns the CPU address of the contents (always mapped, and coherent:
  // memory is shared), or nullptr for Access::kGpuOnly.
  [[nodiscard]] void* mapped() const { return buffer_.contents; }

 private:
  // Size and mapping are read from the buffer itself, so that the defaulted
  // moves leave the moved-from buffer empty.
  id<MTLBuffer> buffer_ = nil;
  Resident resident_;
};

// Creates a private (kGpuOnly) buffer and fills it with `data`, which must not
// be empty, through a staging buffer. Blocks until the upload has finished.
[[nodiscard]] Buffer CreateGpuBuffer(const MetalContext& context,
                                     std::span<const std::byte> data,
                                     const char* label = nullptr);

}  // namespace blue::render

#endif  // BLUE_RENDER_BUFFER_H_
