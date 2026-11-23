#ifndef BLUE_RENDER_D3D12_DESCRIPTOR_HEAPS_H_
#define BLUE_RENDER_D3D12_DESCRIPTOR_HEAPS_H_

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <optional>

#include "render/d3d12/ring_allocator.h"
#include "render/d3d12/slot_allocator.h"

namespace blue::render::d3d12 {

// One descriptor heap of one type: CPU-only, or shader-visible (then also
// GPU handles). Neither copyable nor movable.
class DescriptorHeap {
 public:
  DescriptorHeap(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                 uint32_t count, bool shader_visible, const wchar_t* name);

  DescriptorHeap(const DescriptorHeap&) = delete;
  DescriptorHeap& operator=(const DescriptorHeap&) = delete;

  [[nodiscard]] ID3D12DescriptorHeap* heap() const { return heap_.Get(); }
  [[nodiscard]] uint32_t count() const { return count_; }
  [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE cpu(uint32_t index) const {
    return {cpu_start_.ptr + size_t{index} * increment_};
  }
  // Shader-visible heaps only.
  [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE gpu(uint32_t index) const {
    return {gpu_start_.ptr + uint64_t{index} * increment_};
  }

 private:
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
  uint32_t count_;
  uint32_t increment_;
  D3D12_CPU_DESCRIPTOR_HANDLE cpu_start_{};
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_start_{};
};

// A descriptor handed out by DescriptorHeaps: its index in its heap and its
// handles (gpu.ptr is 0 for CPU-only heaps).
struct Descriptor {
  uint32_t index = UINT32_MAX;
  D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
  D3D12_GPU_DESCRIPTOR_HANDLE gpu{};

  [[nodiscard]] bool valid() const { return index != UINT32_MAX; }
};

// The renderer's descriptor heaps (design §4.1):
//   - CPU-only heaps for render target views, depth stencil views and
//     "staging" shader resource views (the sources that are copied into
//     shader-visible tables);
//   - one shader-visible CBV/SRV/UAV heap of kShaderVisibleCount
//     descriptors, split into a region of single persistent descriptors
//     (texture SRVs that ImGui shows, ImGui's own textures), a region of
//     persistent tables of kTableSize descriptors (one per material
//     version), and a per-frame ring of transient tables (pass inputs);
//   - one shader-visible sampler heap (the static samplers, see Samplers).
// Both shader-visible heaps are bound once per command list
// (SetDescriptorHeaps()).
//
// Persistent descriptors are given back with Retire() (reused once the
// frames in flight have finished) or Free() (now: nothing on the GPU can
// read them). Running out of any heap is fatal (the sizes are far above
// what the renderer uses). Neither copyable nor movable. Not thread-safe.
class DescriptorHeaps {
 public:
  static constexpr uint32_t kShaderVisibleCount = 65536;
  static constexpr uint32_t kSingleCount = 24576;
  static constexpr uint32_t kTableSize = 8;
  static constexpr uint32_t kTableCount = 4096;
  static constexpr uint32_t kTransientCount =
      kShaderVisibleCount - kSingleCount - kTableSize * kTableCount;
  static constexpr uint32_t kRtvCount = 256;
  static constexpr uint32_t kDsvCount = 64;
  static constexpr uint32_t kStagingCount = 32768;
  static constexpr uint32_t kSamplerCount = 256;

  explicit DescriptorHeaps(ID3D12Device* device);

  DescriptorHeaps(const DescriptorHeaps&) = delete;
  DescriptorHeaps& operator=(const DescriptorHeaps&) = delete;

  // CPU-only views.
  [[nodiscard]] Descriptor AllocateRtv();
  [[nodiscard]] Descriptor AllocateDsv();
  [[nodiscard]] Descriptor AllocateStaging();
  void FreeRtv(const Descriptor& descriptor);
  void FreeDsv(const Descriptor& descriptor);
  void FreeStaging(const Descriptor& descriptor);

  // A single persistent shader-visible descriptor.
  [[nodiscard]] Descriptor AllocateSingle();
  void FreeSingle(const Descriptor& descriptor);
  void RetireSingle(const Descriptor& descriptor, uint64_t last_frame);
  // Returns the single persistent descriptor whose CPU handle is `cpu`.
  [[nodiscard]] Descriptor SingleAt(D3D12_CPU_DESCRIPTOR_HANDLE cpu) const;

  // A persistent table of kTableSize contiguous shader-visible descriptors
  // (the first one's handles).
  [[nodiscard]] Descriptor AllocateTable();
  void FreeTable(const Descriptor& descriptor);
  void RetireTable(const Descriptor& descriptor, uint64_t last_frame);

  // `count` contiguous shader-visible descriptors valid for the frame being
  // recorded only (BeginFrame() .. EndFrame()); allocated between frames
  // (setup work), they belong to the next frame.
  [[nodiscard]] Descriptor AllocateTransient(uint32_t count);

  // A null Texture2D SRV in the staging heap: what unused entries of a
  // table get (descriptors are static: always initialized).
  [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE null_texture_srv() const {
    return null_texture_srv_.cpu;
  }
  // Copies `count` staging descriptors from `source` to `destination`.
  void Copy(uint32_t count, D3D12_CPU_DESCRIPTOR_HANDLE destination,
            D3D12_CPU_DESCRIPTOR_HANDLE source) const;
  // The `index`-th descriptor after `first` in the CBV/SRV/UAV heaps.
  [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE Offset(
      D3D12_CPU_DESCRIPTOR_HANDLE first, uint32_t index) const {
    return {first.ptr + size_t{index} * view_increment_};
  }
  [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE Offset(
      D3D12_GPU_DESCRIPTOR_HANDLE first, uint32_t index) const {
    return {first.ptr + uint64_t{index} * view_increment_};
  }

  // A shader-visible sampler (the static set; never given back).
  [[nodiscard]] Descriptor AllocateSampler();

  // Starts recording frame `frame_serial`: the frames up to frame_serial -
  // kFramesInFlight have finished (their fences were waited on), so what
  // they retired or used transiently is reusable.
  void BeginFrame(uint64_t frame_serial);
  // Ends the frame's transient allocations.
  void EndFrame(uint64_t frame_serial);
  // Makes everything retired reusable now (the GPU is idle).
  void ReclaimRetired();

  // Binds the shader-visible heaps to `cmd`.
  void Bind(ID3D12GraphicsCommandList* cmd) const;

  [[nodiscard]] ID3D12Device* device() const { return device_; }
  [[nodiscard]] ID3D12DescriptorHeap* shader_visible_heap() const {
    return shader_visible_.heap();
  }

 private:
  // Returns the descriptor at `index` of the shader-visible heap.
  [[nodiscard]] Descriptor ShaderVisible(uint32_t index) const;

  ID3D12Device* device_;
  DescriptorHeap rtv_;
  DescriptorHeap dsv_;
  DescriptorHeap staging_;
  DescriptorHeap shader_visible_;
  DescriptorHeap samplers_;
  SlotAllocator rtv_slots_{kRtvCount};
  SlotAllocator dsv_slots_{kDsvCount};
  SlotAllocator staging_slots_{kStagingCount};
  SlotAllocator single_slots_{kSingleCount};
  SlotAllocator table_slots_{kTableCount};
  RingAllocator transient_{kTransientCount};
  uint32_t samplers_used_ = 0;
  uint32_t view_increment_ = 0;
  Descriptor null_texture_srv_;
};

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_DESCRIPTOR_HEAPS_H_
