#ifndef BLUE_RENDER_D3D12_SWAPCHAIN_H_
#define BLUE_RENDER_D3D12_SWAPCHAIN_H_

#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>

#include "core/extent.h"
#include "render/d3d12/descriptor_heaps.h"
#include "render/d3d12/image.h"

namespace blue::render::d3d12 {

class D3d12Context;

// The flip-model swapchain of a window (design §4.1): kBufferCount
// R8G8B8A8_UNORM buffers written through _SRGB render target views (flip
// model forbids sRGB buffer formats), DXGI_SWAP_EFFECT_FLIP_DISCARD, a
// frame latency waitable object with a maximum latency of kFramesInFlight,
// tearing allowed when vsync is off and DXGI supports it, Alt+Enter left to
// the application (SDL; the window is never exclusive full-screen, so
// tearing is always allowed in windowed and borderless mode). Created in
// exactly one function (CreateSwapchain() in swapchain.cc) so a frame
// generation interposer can wrap it later (§6).
//
// Frame pacing: WaitForFrame() at the start of every frame blocks until
// fewer than kFramesInFlight presents are queued, then the renderer records
// into buffer() and Present()s it. Present() never fails for an occluded or
// minimised window (DXGI_STATUS_OCCLUDED is a success code); a removed
// device is fatal with its reason (CheckHr()).
//
// Borrows `context`, which must outlive it. Neither copyable nor movable.
// Not thread-safe.
class Swapchain {
 public:
  static constexpr uint32_t kBufferCount = 3;
  static constexpr DXGI_FORMAT kBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
  static constexpr DXGI_FORMAT kViewFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  static constexpr DWORD kFrameWaitMs = 1000;

  // `window` is the HWND to present to.
  Swapchain(const D3d12Context& context, HWND window, core::Extent2D extent);
  ~Swapchain();

  Swapchain(const Swapchain&) = delete;
  Swapchain& operator=(const Swapchain&) = delete;

  // Resizes the buffers to `extent` (each side at least 1). The GPU must no
  // longer use them; every buffer is in PRESENT afterwards.
  void Resize(core::Extent2D extent);
  // Blocks until DXGI accepts another frame (the latency waitable object),
  // at most kFrameWaitMs (a hung present then shows as a slow frame, not a
  // frozen application).
  void WaitForFrame() const;
  // Presents the current buffer (PRESENT state); vsync on: synchronized to
  // the display, off: immediately (tearing when supported).
  void Present(bool vsync);

  [[nodiscard]] core::Extent2D extent() const { return extent_; }
  // The buffer to render to next.
  [[nodiscard]] ID3D12Resource* buffer() const {
    return buffers_[current_].resource.Get();
  }
  [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE rtv() const {
    return buffers_[current_].rtv.cpu;
  }
  // Adds the barrier that brings the current buffer into `state`.
  void Transition(BarrierBatch& barriers, D3D12_RESOURCE_STATES state);
  [[nodiscard]] D3D12_RESOURCE_STATES state() const {
    return buffers_[current_].state;
  }

 private:
  struct SwapBuffer {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    Descriptor rtv;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_PRESENT;
  };

  void AcquireBuffers();
  void ReleaseBuffers();

  const D3d12Context& context_;
  Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain_;
  HANDLE frame_latency_waitable_ = nullptr;
  UINT flags_ = 0;
  core::Extent2D extent_{};
  std::array<SwapBuffer, kBufferCount> buffers_;
  uint32_t current_ = 0;
};

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_SWAPCHAIN_H_
