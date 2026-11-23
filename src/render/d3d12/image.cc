#include "render/d3d12/image.h"

#include <D3D12MemAlloc.h>
#include <d3d12.h>

#include <cstddef>
#include <cstdint>
#include <utility>

#include "core/log.h"
#include "render/d3d12/d3d12_check.h"
#include "render/d3d12/d3d12_context.h"
#include "render/d3d12/descriptor_heaps.h"

namespace blue::render::d3d12 {

void BarrierBatch::Transition(ID3D12Resource* resource,
                              D3D12_RESOURCE_STATES before,
                              D3D12_RESOURCE_STATES after,
                              uint32_t subresource) {
  if (count_ == kCapacity) {
    Flush();
  }
  D3D12_RESOURCE_BARRIER& barrier = barriers_[count_++];
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.Transition = {
      .pResource = resource,
      .Subresource = subresource,
      .StateBefore = before,
      .StateAfter = after,
  };
}

void BarrierBatch::Flush() {
  if (count_ > 0) {
    cmd_->ResourceBarrier(static_cast<UINT>(count_), barriers_.data());
    count_ = 0;
  }
}

Image::Image(const D3d12Context& context, const ImageDesc& desc)
    : context_(&context),
      extent_(desc.extent),
      mip_levels_(desc.mip_levels),
      layers_(desc.layers),
      cube_(desc.cube),
      format_(desc.format),
      state_(desc.initial_state) {
  const D3D12_RESOURCE_DESC resource_desc{
      .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
      .Alignment = 0,
      .Width = desc.extent.width,
      .Height = desc.extent.height,
      .DepthOrArraySize = static_cast<UINT16>(desc.layers),
      .MipLevels = static_cast<UINT16>(desc.mip_levels),
      .Format = desc.format,
      .SampleDesc = {1, 0},
      .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
      .Flags = desc.flags,
  };
  D3D12MA::ALLOCATION_DESC allocation_desc{};
  allocation_desc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
  const D3D12_CLEAR_VALUE* clear =
      desc.clear_value ? &*desc.clear_value : nullptr;
  CheckHr(context.allocator()->CreateResource(
      &allocation_desc, &resource_desc, desc.initial_state, clear,
      &allocation_, __uuidof(ID3D12Resource), nullptr));
  allocation_->GetResource()->SetName(desc.name);
  size_bytes_ = allocation_->GetSize();
}

Image::~Image() { Release(); }

Image::Image(Image&& other) noexcept { *this = std::move(other); }

Image& Image::operator=(Image&& other) noexcept {
  if (this != &other) {
    Release();
    context_ = std::exchange(other.context_, nullptr);
    allocation_ = std::move(other.allocation_);
    extent_ = std::exchange(other.extent_, {});
    mip_levels_ = std::exchange(other.mip_levels_, 0);
    layers_ = std::exchange(other.layers_, 0);
    cube_ = std::exchange(other.cube_, false);
    format_ = std::exchange(other.format_, DXGI_FORMAT_UNKNOWN);
    state_ = std::exchange(other.state_, D3D12_RESOURCE_STATE_COMMON);
    size_bytes_ = std::exchange(other.size_bytes_, 0);
    rtvs_ = std::exchange(other.rtvs_, {});
    dsv_ = std::exchange(other.dsv_, {});
    read_only_dsv_ = std::exchange(other.read_only_dsv_, {});
    staging_srv_ = std::exchange(other.staging_srv_, {});
    mip_srvs_ = std::exchange(other.mip_srvs_, {});
    shader_srv_ = std::exchange(other.shader_srv_, {});
  }
  return *this;
}

void Image::Release() {
  if (context_ != nullptr) {
    DescriptorHeaps& heaps = context_->descriptors();
    for (const Descriptor& rtv : rtvs_) {
      heaps.FreeRtv(rtv);
    }
    if (dsv_.valid()) {
      heaps.FreeDsv(dsv_);
      heaps.FreeDsv(read_only_dsv_);
    }
    if (staging_srv_.valid()) {
      heaps.FreeStaging(staging_srv_);
    }
    for (const Descriptor& srv : mip_srvs_) {
      heaps.FreeStaging(srv);
    }
    if (shader_srv_.valid()) {
      heaps.FreeSingle(shader_srv_);
    }
  }
  rtvs_.clear();
  dsv_ = {};
  read_only_dsv_ = {};
  staging_srv_ = {};
  mip_srvs_.clear();
  shader_srv_ = {};
  allocation_.Reset();
  context_ = nullptr;
}

size_t Image::AddRtv(DXGI_FORMAT format, uint32_t mip, uint32_t layer) {
  const Descriptor rtv = context_->descriptors().AllocateRtv();
  D3D12_RENDER_TARGET_VIEW_DESC desc{};
  desc.Format = format;
  if (layers_ > 1) {
    desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
    desc.Texture2DArray = {.MipSlice = mip,
                           .FirstArraySlice = layer,
                           .ArraySize = 1,
                           .PlaneSlice = 0};
  } else {
    desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    desc.Texture2D = {.MipSlice = mip, .PlaneSlice = 0};
  }
  context_->device()->CreateRenderTargetView(resource(), &desc, rtv.cpu);
  rtvs_.push_back(rtv);
  return rtvs_.size() - 1;
}

void Image::CreateDsv(DXGI_FORMAT format) {
  DescriptorHeaps& heaps = context_->descriptors();
  dsv_ = heaps.AllocateDsv();
  read_only_dsv_ = heaps.AllocateDsv();
  D3D12_DEPTH_STENCIL_VIEW_DESC desc{};
  desc.Format = format;
  desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  desc.Flags = D3D12_DSV_FLAG_NONE;
  desc.Texture2D.MipSlice = 0;
  context_->device()->CreateDepthStencilView(resource(), &desc, dsv_.cpu);
  desc.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
  context_->device()->CreateDepthStencilView(resource(), &desc,
                                             read_only_dsv_.cpu);
}

size_t Image::AddMipSrv(DXGI_FORMAT format, uint32_t mip) {
  D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
  desc.Format = format;
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
  desc.Texture2DArray = {.MostDetailedMip = mip,
                         .MipLevels = 1,
                         .FirstArraySlice = 0,
                         .ArraySize = layers_,
                         .PlaneSlice = 0,
                         .ResourceMinLODClamp = 0.0f};
  const Descriptor srv = context_->descriptors().AllocateStaging();
  context_->device()->CreateShaderResourceView(resource(), &desc, srv.cpu);
  mip_srvs_.push_back(srv);
  return mip_srvs_.size() - 1;
}

void Image::CreateSrv(DXGI_FORMAT format, bool shader_visible) {
  D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
  desc.Format = format;
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  if (cube_) {
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    desc.TextureCube = {.MostDetailedMip = 0,
                        .MipLevels = mip_levels_,
                        .ResourceMinLODClamp = 0.0f};
  } else if (layers_ > 1) {
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    desc.Texture2DArray = {.MostDetailedMip = 0,
                           .MipLevels = mip_levels_,
                           .FirstArraySlice = 0,
                           .ArraySize = layers_,
                           .PlaneSlice = 0,
                           .ResourceMinLODClamp = 0.0f};
  } else {
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Texture2D = {.MostDetailedMip = 0,
                      .MipLevels = mip_levels_,
                      .PlaneSlice = 0,
                      .ResourceMinLODClamp = 0.0f};
  }
  DescriptorHeaps& heaps = context_->descriptors();
  staging_srv_ = heaps.AllocateStaging();
  context_->device()->CreateShaderResourceView(resource(), &desc,
                                               staging_srv_.cpu);
  if (shader_visible) {
    shader_srv_ = heaps.AllocateSingle();
    context_->device()->CreateShaderResourceView(resource(), &desc,
                                                 shader_srv_.cpu);
  }
}

void Image::Transition(BarrierBatch& barriers, D3D12_RESOURCE_STATES state) {
  if (state_ == state) {
    return;
  }
  barriers.Transition(resource(), state_, state);
  state_ = state;
}

}  // namespace blue::render::d3d12
