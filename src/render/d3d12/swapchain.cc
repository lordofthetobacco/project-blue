#include "render/d3d12/swapchain.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>

#include "core/extent.h"
#include "core/log.h"
#include "render/d3d12/d3d12_check.h"
#include "render/d3d12/d3d12_context.h"
#include "render/d3d12/descriptor_heaps.h"
#include "render/d3d12/image.h"
#include "render/frame_config.h"

namespace blue::render::d3d12 {

namespace {

// The one place a DXGI swapchain is created (an interposer such as
// Streamline's wraps this call).
Microsoft::WRL::ComPtr<IDXGISwapChain3> CreateSwapchain(
    IDXGIFactory4* factory, ID3D12CommandQueue* queue, HWND window,
    const DXGI_SWAP_CHAIN_DESC1& desc) {
  Microsoft::WRL::ComPtr<IDXGISwapChain1> swapchain1;
  CheckHr(factory->CreateSwapChainForHwnd(queue, window, &desc, nullptr,
                                          nullptr, &swapchain1));
  // The application handles full-screen itself (SDL).
  CheckHr(factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER));
  Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain;
  CheckHr(swapchain1.As(&swapchain));
  return swapchain;
}

}  // namespace

Swapchain::Swapchain(const D3d12Context& context, HWND window,
                     core::Extent2D extent)
    : context_(context),
      extent_{std::max(extent.width, 1u), std::max(extent.height, 1u)} {
  flags_ = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
  if (context.tearing_supported()) {
    flags_ |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
  }
  const DXGI_SWAP_CHAIN_DESC1 desc{
      .Width = extent_.width,
      .Height = extent_.height,
      .Format = kBufferFormat,
      .Stereo = FALSE,
      .SampleDesc = {1, 0},
      .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
      .BufferCount = kBufferCount,
      .Scaling = DXGI_SCALING_STRETCH,
      .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
      .AlphaMode = DXGI_ALPHA_MODE_IGNORE,
      .Flags = flags_,
  };
  swapchain_ =
      CreateSwapchain(context.factory(), context.queue(), window, desc);
  CheckHr(swapchain_->SetMaximumFrameLatency(kFramesInFlight));
  frame_latency_waitable_ = swapchain_->GetFrameLatencyWaitableObject();
  AcquireBuffers();
  core::log::Info("Swapchain {} x {}, {} buffers{}", extent_.width,
                  extent_.height, kBufferCount,
                  context.tearing_supported() ? ", tearing allowed" : "");
}

Swapchain::~Swapchain() {
  ReleaseBuffers();
  if (frame_latency_waitable_ != nullptr) {
    CloseHandle(frame_latency_waitable_);
  }
}

void Swapchain::AcquireBuffers() {
  for (uint32_t i = 0; i < kBufferCount; ++i) {
    SwapBuffer& buffer = buffers_[i];
    CheckHr(swapchain_->GetBuffer(i, IID_PPV_ARGS(&buffer.resource)));
    buffer.resource->SetName(L"blue swapchain buffer");
    buffer.rtv = context_.descriptors().AllocateRtv();
    D3D12_RENDER_TARGET_VIEW_DESC view{};
    view.Format = kViewFormat;
    view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    context_.device()->CreateRenderTargetView(buffer.resource.Get(), &view,
                                              buffer.rtv.cpu);
    buffer.state = D3D12_RESOURCE_STATE_PRESENT;
  }
  current_ = swapchain_->GetCurrentBackBufferIndex();
}

void Swapchain::ReleaseBuffers() {
  for (SwapBuffer& buffer : buffers_) {
    if (buffer.rtv.valid()) {
      context_.descriptors().FreeRtv(buffer.rtv);
    }
    buffer = SwapBuffer();
  }
}

void Swapchain::Resize(core::Extent2D extent) {
  extent_ = {std::max(extent.width, 1u), std::max(extent.height, 1u)};
  ReleaseBuffers();
  CheckHr(swapchain_->ResizeBuffers(kBufferCount, extent_.width, extent_.height,
                                    kBufferFormat, flags_));
  AcquireBuffers();
}

void Swapchain::WaitForFrame() const {
  WaitForSingleObjectEx(frame_latency_waitable_, 1000, TRUE);
}

void Swapchain::Present(bool vsync) {
  const bool tear = !vsync && context_.tearing_supported();
  CheckHr(swapchain_->Present(vsync ? 1 : 0,
                              tear ? DXGI_PRESENT_ALLOW_TEARING : 0));
  current_ = swapchain_->GetCurrentBackBufferIndex();
}

void Swapchain::Transition(BarrierBatch& barriers,
                           D3D12_RESOURCE_STATES state) {
  SwapBuffer& buffer = buffers_[current_];
  if (buffer.state != state) {
    barriers.Transition(buffer.resource.Get(), buffer.state, state);
    buffer.state = state;
  }
}

}  // namespace blue::render::d3d12
