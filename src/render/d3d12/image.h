#ifndef BLUE_RENDER_D3D12_IMAGE_H_
#define BLUE_RENDER_D3D12_IMAGE_H_

#include <D3D12MemAlloc.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "core/extent.h"
#include "render/d3d12/descriptor_heaps.h"

namespace blue::render::d3d12 {

class D3d12Context;

// Resource barriers collected while a pass prepares its resources and
// recorded in one ResourceBarrier() call (design §4.1: legacy barriers,
// batched per pass). Holds up to kCapacity; more flushes early.
class BarrierBatch {
 public:
  static constexpr size_t kCapacity = 16;

  explicit BarrierBatch(ID3D12GraphicsCommandList* cmd) : cmd_(cmd) {}
  ~BarrierBatch() { Flush(); }

  BarrierBatch(const BarrierBatch&) = delete;
  BarrierBatch& operator=(const BarrierBatch&) = delete;

  // Adds a transition of every subresource of `resource`.
  void Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                  D3D12_RESOURCE_STATES after);
  // Records the collected barriers (nothing when empty).
  void Flush();

 private:
  ID3D12GraphicsCommandList* cmd_;
  std::array<D3D12_RESOURCE_BARRIER, kCapacity> barriers_{};
  size_t count_ = 0;
};

// How an Image is created.
struct ImageDesc {
  core::Extent2D extent;
  uint32_t mip_levels = 1;
  // Array layers; 6 with `cube` for a cube map.
  uint32_t layers = 1;
  bool cube = false;
  // The resource's format; a _TYPELESS one when views of several formats
  // are needed (e.g. R8G8B8A8_TYPELESS with UNORM and _SRGB views).
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
  D3D12_RESOURCE_STATES initial_state = D3D12_RESOURCE_STATE_COMMON;
  // The optimized clear value of a render target or depth buffer.
  std::optional<D3D12_CLEAR_VALUE> clear_value;
  const wchar_t* name = L"blue image";
};

// A 2D texture, texture array or cube map in memory from the D3D12MA
// allocator, with the resource state it will be in when the commands
// recorded so far have executed (state()), and the views it owns: up to
// kMaxRtvs render target views, a depth stencil view, a shader resource
// view in the staging heap (copied into tables) and one in the
// shader-visible heap (ImGui). Views are freed with the image, which is
// only destroyed once no frame in flight uses it.
//
// The tracked state is exact because the backend records on one thread
// into one queue, in submission order (design §4.1). A pass that needs an
// image in a state calls Transition() and documents the states it leaves.
// Move-only; a moved-from image is empty. Not thread-safe.
class Image {
 public:
  static constexpr size_t kMaxRtvs = 2;

  Image() = default;
  Image(const D3d12Context& context, const ImageDesc& desc);
  ~Image();

  Image(Image&& other) noexcept;
  Image& operator=(Image&& other) noexcept;
  Image(const Image&) = delete;
  Image& operator=(const Image&) = delete;

  // Adds a render target view of mip 0, layer 0 in `format` and returns its
  // index (< kMaxRtvs).
  size_t AddRtv(DXGI_FORMAT format);
  // Creates the depth stencil view in `format`.
  void CreateDsv(DXGI_FORMAT format);
  // Creates the staging shader resource view (every mip and layer) in
  // `format`; with `shader_visible` also one in the shader-visible heap.
  void CreateSrv(DXGI_FORMAT format, bool shader_visible);

  // Adds the barrier that brings the image into `state` (nothing when it is
  // already there) and records the new state.
  void Transition(BarrierBatch& barriers, D3D12_RESOURCE_STATES state);

  [[nodiscard]] ID3D12Resource* resource() const {
    return allocation_ ? allocation_->GetResource() : nullptr;
  }
  [[nodiscard]] bool valid() const { return allocation_ != nullptr; }
  [[nodiscard]] core::Extent2D extent() const { return extent_; }
  [[nodiscard]] uint32_t mip_levels() const { return mip_levels_; }
  [[nodiscard]] uint32_t layers() const { return layers_; }
  [[nodiscard]] DXGI_FORMAT format() const { return format_; }
  [[nodiscard]] D3D12_RESOURCE_STATES state() const { return state_; }
  // Device memory of the image, in bytes.
  [[nodiscard]] uint64_t size_bytes() const { return size_bytes_; }
  [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE rtv(size_t index = 0) const {
    return rtvs_[index].cpu;
  }
  [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE dsv() const { return dsv_.cpu; }
  [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE staging_srv() const {
    return staging_srv_.cpu;
  }
  [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE shader_srv() const {
    return shader_srv_.gpu;
  }

 private:
  void Release();

  const D3d12Context* context_ = nullptr;
  Microsoft::WRL::ComPtr<D3D12MA::Allocation> allocation_;
  core::Extent2D extent_{};
  uint32_t mip_levels_ = 0;
  uint32_t layers_ = 0;
  bool cube_ = false;
  DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
  D3D12_RESOURCE_STATES state_ = D3D12_RESOURCE_STATE_COMMON;
  uint64_t size_bytes_ = 0;
  std::array<Descriptor, kMaxRtvs> rtvs_{};
  size_t rtv_count_ = 0;
  Descriptor dsv_;
  Descriptor staging_srv_;
  Descriptor shader_srv_;
};

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_IMAGE_H_
