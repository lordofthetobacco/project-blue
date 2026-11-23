#ifndef BLUE_RENDER_D3D12_BUFFER_H_
#define BLUE_RENDER_D3D12_BUFFER_H_

#include <D3D12MemAlloc.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <utility>

namespace blue::render::d3d12 {

class D3d12Context;

// A GPU buffer in memory from the D3D12MA allocator. Move-only; a
// moved-from buffer is empty. Not thread-safe.
class Buffer {
 public:
  enum class Access {
    kGpuOnly,  // Default heap, COMMON (copies and reads promote it
               // implicitly), filled through a staging upload.
    kUpload,   // Upload heap, GENERIC_READ, persistently mapped, written by
               // the CPU.
    kReadback  // Readback heap, COPY_DEST, persistently mapped, read by the
               // CPU once the copy into it has finished.
  };

  Buffer() = default;
  Buffer(const D3d12Context& context, uint64_t size, Access access,
         const wchar_t* name);

  Buffer(Buffer&& other) noexcept
      : allocation_(std::move(other.allocation_)),
        size_(std::exchange(other.size_, 0)),
        mapped_(std::exchange(other.mapped_, nullptr)) {}
  Buffer& operator=(Buffer&& other) noexcept {
    allocation_ = std::move(other.allocation_);
    size_ = std::exchange(other.size_, 0);
    mapped_ = std::exchange(other.mapped_, nullptr);
    return *this;
  }

  [[nodiscard]] ID3D12Resource* resource() const {
    return allocation_ ? allocation_->GetResource() : nullptr;
  }
  [[nodiscard]] uint64_t size() const { return size_; }
  [[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS gpu_address() const {
    return allocation_ ? allocation_->GetResource()->GetGPUVirtualAddress()
                       : 0;
  }
  // The persistent mapping (kUpload, kReadback), else nullptr.
  [[nodiscard]] void* mapped() const { return mapped_; }

 private:
  Microsoft::WRL::ComPtr<D3D12MA::Allocation> allocation_;
  uint64_t size_ = 0;
  void* mapped_ = nullptr;
};

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_BUFFER_H_
