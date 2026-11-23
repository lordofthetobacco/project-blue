#ifndef BLUE_RENDER_D3D12_D3D12_CONTEXT_H_
#define BLUE_RENDER_D3D12_D3D12_CONTEXT_H_

#include <D3D12MemAlloc.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "render/backend.h"
#include "render/d3d12/descriptor_heaps.h"

namespace blue::render::d3d12 {

using Microsoft::WRL::ComPtr;

// Core Direct3D 12 objects shared by the whole backend: the DXGI factory,
// the adapter, the device (feature level 12_0 or better), the one direct
// queue with its fence, the memory allocator (D3D12MA) and the descriptor
// heaps.
//
// Adapter choice (AdapterPreference): kHighPerformance takes the adapters in
// DXGI's high-performance order, skipping software ones; kSoftware takes
// WARP. An adapter qualifies with feature level 12_0, Shader Model 6.0 and
// resource binding tier 2 (design §4.1). When none does, the context has no
// device (has_device() false) and may only be destroyed: the renderer falls
// back to another backend. Every later failure is fatal.
//
// Validation: Debug builds, or `gpu_validation`, enable the debug layer
// (when d3d12SDKLayers.dll is available) and count its messages;
// `gpu_validation` also enables GPU-based validation (slow). Debug builds
// enable DRED (auto-breadcrumbs and page faults) and break into an attached
// debugger on errors. Neither copyable nor movable. Not thread-safe.
class D3d12Context {
 public:
  D3d12Context(AdapterPreference adapter, bool gpu_validation);
  ~D3d12Context();

  D3d12Context(const D3d12Context&) = delete;
  D3d12Context& operator=(const D3d12Context&) = delete;

  // False when no adapter qualified (see the class comment); nothing else
  // may be used then.
  [[nodiscard]] bool has_device() const { return device_ != nullptr; }
  [[nodiscard]] IDXGIFactory4* factory() const { return factory_.Get(); }
  [[nodiscard]] IDXGIAdapter1* adapter() const { return adapter_.Get(); }
  [[nodiscard]] ID3D12Device* device() const { return device_.Get(); }
  [[nodiscard]] ID3D12CommandQueue* queue() const { return queue_.Get(); }
  [[nodiscard]] D3D12MA::Allocator* allocator() const {
    return allocator_.Get();
  }
  // Mutable: allocating descriptors is not a visible change of the context.
  [[nodiscard]] DescriptorHeaps& descriptors() const { return *descriptors_; }
  [[nodiscard]] std::string_view device_name() const { return device_name_; }
  // The adapter is WARP (or another software rasterizer).
  [[nodiscard]] bool software_adapter() const { return software_adapter_; }
  // Ticks per second of the queue's timestamps.
  [[nodiscard]] uint64_t timestamp_frequency() const {
    return timestamp_frequency_;
  }
  // Whether DXGI can present without vsync while tearing
  // (DXGI_FEATURE_PRESENT_ALLOW_TEARING).
  [[nodiscard]] bool tearing_supported() const { return tearing_supported_; }
  // Whether pipelines can rasterize lines as one-pixel-wide rectangles
  // (D3D12_LINE_RASTERIZATION_MODE_QUADRILATERAL_NARROW through
  // D3D12_RASTERIZER_DESC2): Vulkan's strict line rasterization.
  [[nodiscard]] bool narrow_lines_supported() const {
    return narrow_lines_supported_;
  }

  // Whether the debug layer is active.
  [[nodiscard]] bool validation() const { return info_queue_ != nullptr; }
  struct ValidationCounts {
    uint32_t errors = 0;
    uint32_t warnings = 0;
  };
  // Debug layer messages so far, by severity (0 without validation); logs
  // and counts the messages stored since the last call first.
  [[nodiscard]] ValidationCounts validation_counts() const;
  // Logs and counts the debug layer messages stored since the last call.
  void PollValidationMessages() const;

  // Upload staging memory allocated right now and its high-water mark, in
  // bytes (see StagingUploader).
  struct StagingUsage {
    uint64_t current = 0;
    uint64_t peak = 0;
  };
  [[nodiscard]] StagingUsage staging_usage() const { return staging_usage_; }
  void AddStagingBytes(uint64_t bytes) const;
  void RemoveStagingBytes(uint64_t bytes) const;

  // Executes `list` (closed) on the queue and returns the fence value that
  // is reached when it has finished.
  [[nodiscard]] uint64_t Execute(ID3D12CommandList* list) const;
  // Returns whether the queue's fence has reached `value`.
  [[nodiscard]] bool Reached(uint64_t value) const;
  // Blocks until the queue's fence reaches `value`.
  void WaitFor(uint64_t value) const;
  // Blocks until the queue has finished all submitted work.
  void WaitIdle() const;

  // Records commands with `record` (the descriptor heaps are bound) and
  // blocks until the GPU has executed them. For setup work such as uploads,
  // not per-frame use.
  void SubmitImmediate(
      const std::function<void(ID3D12GraphicsCommandList*)>& record) const;

 private:
  // Each returns false (after logging why) when no device qualifies.
  [[nodiscard]] bool CreateFactory(bool debug);
  [[nodiscard]] bool CreateDevice(AdapterPreference preference);
  void EnableInfoQueue();
  void CreateQueueAndAllocator();

  ComPtr<IDXGIFactory4> factory_;
  ComPtr<IDXGIAdapter1> adapter_;
  ComPtr<ID3D12Device> device_;
  ComPtr<ID3D12InfoQueue> info_queue_;
  ComPtr<ID3D12CommandQueue> queue_;
  ComPtr<ID3D12Fence> fence_;
  HANDLE fence_event_ = nullptr;
  // The value the next Execute() signals.
  mutable uint64_t next_fence_value_ = 1;
  ComPtr<D3D12MA::Allocator> allocator_;
  std::unique_ptr<DescriptorHeaps> descriptors_;
  ComPtr<ID3D12CommandAllocator> immediate_allocator_;
  ComPtr<ID3D12GraphicsCommandList> immediate_list_;
  std::string device_name_;
  bool software_adapter_ = false;
  bool tearing_supported_ = false;
  bool narrow_lines_supported_ = false;
  bool gpu_based_validation_ = false;
  uint64_t timestamp_frequency_ = 0;
  mutable ValidationCounts validation_counts_;
  mutable StagingUsage staging_usage_;
};

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_D3D12_CONTEXT_H_
