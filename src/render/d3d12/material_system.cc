#include "render/d3d12/material_system.h"

#include <d3d12.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>

#include "core/log.h"
#include "material/material.h"
#include "render/binding_layout.h"
#include "render/d3d12/buffer.h"
#include "render/d3d12/d3d12_context.h"
#include "render/d3d12/descriptor_heaps.h"
#include "render/d3d12/texture_cache.h"
#include "render/draw_list.h"
#include "render/material_params.h"

namespace blue::render::d3d12 {

namespace {

// The table matches the pass set of the scene root signatures: the CBV at
// the params binding, the textures right after it.
static_assert(BLUE_MATERIAL_PARAMS_BINDING == 0);
static_assert(BLUE_MATERIAL_BASE_COLOR_BINDING == 1);
static_assert(kMaterialTextureCount == material::kTextureSlotCount,
              "One texture binding per material::TextureSlot");
static_assert(1 + kMaterialTextureCount <= DescriptorHeaps::kTableSize);
static_assert(sizeof(MaterialParams) <= MaterialSystem::kParamsStride);

}  // namespace

MaterialSystem::MaterialSystem(const D3d12Context& context,
                               const TextureCache& textures)
    : context_(context),
      textures_(textures),
      params_(context, DescriptorHeaps::kTableCount * kParamsStride,
              Buffer::Access::kUpload, L"blue material params") {}

MaterialSystem::~MaterialSystem() {
  DescriptorHeaps& heaps = context_.descriptors();
  for (const Material& material : materials_) {
    if (material.table.valid()) {
      heaps.FreeTable(material.table);
    }
  }
}

MaterialId MaterialSystem::Create(const material::MaterialDesc& desc) {
  return Add(desc, std::nullopt);
}

MaterialId MaterialSystem::Create(const material::MaterialDesc& desc,
                                  const MaterialTextures& textures) {
  return Add(desc, textures);
}

MaterialId MaterialSystem::Add(const material::MaterialDesc& desc,
                               std::optional<MaterialTextures> textures) {
  Material& added = materials_.emplace_back();
  added.desc = material::Sanitize(desc);
  added.textures = textures;
  added.variant = VariantOf(added.desc);
  added.table = Write(added);
  return static_cast<MaterialId>(materials_.size() - 1);
}

void MaterialSystem::Update(MaterialId id, const material::MaterialDesc& desc) {
  const auto index = static_cast<size_t>(id);
  if (index >= materials_.size()) {
    core::log::Fatal("Update of unknown material {}", index);
  }
  Material& updated = materials_[index];
  if (updated.released) {
    core::log::Warn("Update of released material {} ignored", index);
    return;
  }
  updated.desc = material::Sanitize(desc);
  updated.variant = VariantOf(updated.desc);
  // Frames up to the current one may have recorded the old table.
  context_.descriptors().RetireTable(
      std::exchange(updated.table, Write(updated)), frame_serial_);
}

void MaterialSystem::Release(MaterialId id) {
  const auto index = static_cast<size_t>(id);
  if (id == kDefaultMaterial || index >= materials_.size() ||
      materials_[index].released) {
    return;
  }
  Material& released = materials_[index];
  released.released = true;
  released.textures.reset();
  context_.descriptors().RetireTable(std::exchange(released.table, {}),
                                     frame_serial_);
}

Descriptor MaterialSystem::Write(const Material& material) {
  DescriptorHeaps& heaps = context_.descriptors();
  const Descriptor table = heaps.AllocateTable();
  const uint64_t slot =
      (table.index - DescriptorHeaps::kSingleCount) / DescriptorHeaps::kTableSize;

  const MaterialParams params = MakeMaterialParams(material.desc);
  std::memcpy(static_cast<std::byte*>(params_.mapped()) + slot * kParamsStride,
              &params, sizeof(params));
  ID3D12Device* device = context_.device();
  const D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{
      .BufferLocation = params_.gpu_address() + slot * kParamsStride,
      .SizeInBytes = static_cast<UINT>(kParamsStride),
  };
  device->CreateConstantBufferView(&cbv, table.cpu);

  const MaterialTextures uploaded =
      material.textures.value_or(MaterialTextures{});
  const std::array<const Image*, kMaterialTextureCount> images = {
      &textures_.ImageOr(uploaded.base_color, DefaultTexture::kWhite),
      &textures_.ImageOr(uploaded.metallic_roughness, DefaultTexture::kWhite),
      &textures_.ImageOr(uploaded.normal, DefaultTexture::kFlatNormal),
      &textures_.ImageOr(uploaded.occlusion, DefaultTexture::kWhite),
      &textures_.ImageOr(uploaded.emissive, DefaultTexture::kWhite),
  };
  const D3D12_DESCRIPTOR_HEAP_TYPE type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  const UINT increment = device->GetDescriptorHandleIncrementSize(type);
  for (uint32_t i = 0; i < kMaterialTextureCount; ++i) {
    const D3D12_CPU_DESCRIPTOR_HANDLE destination{
        table.cpu.ptr + size_t{BLUE_MATERIAL_BASE_COLOR_BINDING + i} * increment};
    device->CopyDescriptorsSimple(1, destination, images[i]->staging_srv(),
                                  type);
  }
  return table;
}

}  // namespace blue::render::d3d12
