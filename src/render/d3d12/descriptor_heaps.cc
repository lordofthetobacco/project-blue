#include "render/d3d12/descriptor_heaps.h"

#include <d3d12.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "core/log.h"
#include "render/d3d12/d3d12_check.h"
#include "render/frame_config.h"

namespace blue::render::d3d12 {

namespace {

// Returns the slot `allocator` hands out; fatal when `what` is exhausted.
uint32_t TakeSlot(SlotAllocator& allocator, const char* what) {
  const std::optional<uint32_t> slot = allocator.Allocate();
  if (!slot) {
    core::log::Fatal("Out of {} descriptors ({} in use)", what,
                     allocator.capacity());
  }
  return *slot;
}

}  // namespace

DescriptorHeap::DescriptorHeap(ID3D12Device* device,
                               D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count,
                               bool shader_visible, const wchar_t* name)
    : count_(count),
      increment_(device->GetDescriptorHandleIncrementSize(type)) {
  const D3D12_DESCRIPTOR_HEAP_DESC desc{
      .Type = type,
      .NumDescriptors = count,
      .Flags = shader_visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                              : D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
      .NodeMask = 0,
  };
  CheckHr(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap_)));
  heap_->SetName(name);
  cpu_start_ = heap_->GetCPUDescriptorHandleForHeapStart();
  if (shader_visible) {
    gpu_start_ = heap_->GetGPUDescriptorHandleForHeapStart();
  }
}

DescriptorHeaps::DescriptorHeaps(ID3D12Device* device)
    : device_(device),
      rtv_(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvCount, false,
           L"blue rtv heap"),
      dsv_(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvCount, false,
           L"blue dsv heap"),
      staging_(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kStagingCount,
               false, L"blue staging view heap"),
      shader_visible_(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                      kShaderVisibleCount, true, L"blue view heap"),
      samplers_(device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerCount,
                true, L"blue sampler heap"),
      view_increment_(device->GetDescriptorHandleIncrementSize(
          D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)) {
  null_texture_srv_ = AllocateStaging();
  D3D12_SHADER_RESOURCE_VIEW_DESC null_desc{};
  null_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  null_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  null_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  null_desc.Texture2D.MipLevels = 1;
  device->CreateShaderResourceView(nullptr, &null_desc, null_texture_srv_.cpu);
}

void DescriptorHeaps::Copy(uint32_t count,
                           D3D12_CPU_DESCRIPTOR_HANDLE destination,
                           D3D12_CPU_DESCRIPTOR_HANDLE source) const {
  device_->CopyDescriptorsSimple(count, destination, source,
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

Descriptor DescriptorHeaps::AllocateRtv() {
  const uint32_t index = TakeSlot(rtv_slots_, "render target view");
  return {.index = index, .cpu = rtv_.cpu(index)};
}

Descriptor DescriptorHeaps::AllocateDsv() {
  const uint32_t index = TakeSlot(dsv_slots_, "depth stencil view");
  return {.index = index, .cpu = dsv_.cpu(index)};
}

Descriptor DescriptorHeaps::AllocateStaging() {
  const uint32_t index = TakeSlot(staging_slots_, "staging view");
  return {.index = index, .cpu = staging_.cpu(index)};
}

void DescriptorHeaps::FreeRtv(const Descriptor& descriptor) {
  rtv_slots_.Free(descriptor.index);
}

void DescriptorHeaps::FreeDsv(const Descriptor& descriptor) {
  dsv_slots_.Free(descriptor.index);
}

void DescriptorHeaps::FreeStaging(const Descriptor& descriptor) {
  staging_slots_.Free(descriptor.index);
}

Descriptor DescriptorHeaps::ShaderVisible(uint32_t index) const {
  return {.index = index,
          .cpu = shader_visible_.cpu(index),
          .gpu = shader_visible_.gpu(index)};
}

Descriptor DescriptorHeaps::AllocateSingle() {
  return ShaderVisible(TakeSlot(single_slots_, "shader-visible view"));
}

void DescriptorHeaps::FreeSingle(const Descriptor& descriptor) {
  single_slots_.Free(descriptor.index);
}

void DescriptorHeaps::RetireSingle(const Descriptor& descriptor,
                                   uint64_t last_frame) {
  single_slots_.Retire(descriptor.index, last_frame);
}

Descriptor DescriptorHeaps::SingleAt(D3D12_CPU_DESCRIPTOR_HANDLE cpu) const {
  const size_t index =
      (cpu.ptr - shader_visible_.cpu(0).ptr) / view_increment_;
  if (cpu.ptr < shader_visible_.cpu(0).ptr || index >= kSingleCount) {
    core::log::Fatal("Not a single shader-visible descriptor");
  }
  return ShaderVisible(static_cast<uint32_t>(index));
}

Descriptor DescriptorHeaps::AllocateTable() {
  const uint32_t table = TakeSlot(table_slots_, "descriptor table");
  return ShaderVisible(kSingleCount + table * kTableSize);
}

void DescriptorHeaps::FreeTable(const Descriptor& descriptor) {
  table_slots_.Free((descriptor.index - kSingleCount) / kTableSize);
}

void DescriptorHeaps::RetireTable(const Descriptor& descriptor,
                                  uint64_t last_frame) {
  table_slots_.Retire((descriptor.index - kSingleCount) / kTableSize,
                      last_frame);
}

Descriptor DescriptorHeaps::AllocateTransient(uint32_t count) {
  const std::optional<uint64_t> offset = transient_.Allocate(count, 1);
  if (!offset) {
    core::log::Fatal("Out of transient descriptors ({} of {} in use, {} "
                     "asked for)",
                     transient_.used(), transient_.capacity(), count);
  }
  return ShaderVisible(kSingleCount + kTableSize * kTableCount +
                       static_cast<uint32_t>(*offset));
}

Descriptor DescriptorHeaps::AllocateSampler() {
  if (samplers_used_ == kSamplerCount) {
    core::log::Fatal("Out of samplers ({})", kSamplerCount);
  }
  const uint32_t index = samplers_used_++;
  return {.index = index, .cpu = samplers_.cpu(index), .gpu = samplers_.gpu(index)};
}

void DescriptorHeaps::BeginFrame(uint64_t frame_serial) {
  single_slots_.BeginFrame(frame_serial);
  table_slots_.BeginFrame(frame_serial);
  if (frame_serial > kFramesInFlight) {
    transient_.Release(frame_serial - kFramesInFlight);
  }
}

void DescriptorHeaps::EndFrame(uint64_t frame_serial) {
  transient_.EndFrame(frame_serial);
}

void DescriptorHeaps::ReclaimRetired() {
  single_slots_.ReclaimRetired();
  table_slots_.ReclaimRetired();
}

void DescriptorHeaps::Bind(ID3D12GraphicsCommandList* cmd) const {
  const std::array<ID3D12DescriptorHeap*, 2> heaps = {shader_visible_.heap(),
                                                      samplers_.heap()};
  cmd->SetDescriptorHeaps(static_cast<UINT>(heaps.size()), heaps.data());
}

}  // namespace blue::render::d3d12
