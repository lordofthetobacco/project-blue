#include "render/material_system.h"

#include <array>
#include <cstddef>
#include <optional>
#include <utility>

#include <glm/vec4.hpp>

#include "core/log.h"
#include "render/encoders.h"
#include "shaders/bindings.h"

namespace blue::render {

namespace {

static_assert(offsetof(MaterialParams, emissive) == 16);
static_assert(offsetof(MaterialParams, surface) == 32);
static_assert(offsetof(MaterialParams, uv_alpha) == 48);
static_assert(offsetof(MaterialParams, options) == 64);
static_assert(sizeof(MaterialParams) == 80);

static_assert(kMaterialTextureCount == material::kTextureSlotCount,
              "One texture index per material::TextureSlot");
static_assert(kTextureBaseColor + static_cast<int>(
                                      material::TextureSlot::kEmissive) ==
                  kTextureEmissive,
              "Material texture indices follow material::TextureSlot");

// Returns MaterialParams::options.x of `desc`.
uint32_t UvSetBits(const material::MaterialDesc& desc) {
  uint32_t bits = 0;
  for (size_t slot = 0; slot < desc.uv_set.size(); ++slot) {
    if (desc.uv_set[slot] != 0) {
      bits |= 1u << slot;
    }
  }
  return bits;
}

// Returns MaterialParams::options.y of `desc`.
uint32_t MaterialFlags(const material::MaterialDesc& desc) {
  uint32_t flags = 0;
  if (desc.normal_encoding == material::NormalEncoding::kRg) {
    flags |= kMaterialFlagNormalRg;
  }
  if (desc.occlusion_in_metallic_roughness) {
    flags |= kMaterialFlagOcclusionInMetallicRoughness;
  }
  return flags;
}

}  // namespace

MaterialVariant VariantOf(const material::MaterialDesc& desc) {
  const bool masked = desc.alpha_mode == material::AlphaMode::kMask;
  if (desc.double_sided) {
    return masked ? MaterialVariant::kMaskedDoubleSided
                  : MaterialVariant::kDoubleSided;
  }
  return masked ? MaterialVariant::kMasked : MaterialVariant::kOpaque;
}

MaterialParams MakeMaterialParams(const material::MaterialDesc& desc) {
  return {
      .base_color = desc.base_color,
      .emissive = glm::vec4(desc.emissive * desc.emissive_strength, 0.0f),
      .surface = glm::vec4(desc.metallic, desc.roughness, desc.normal_scale,
                           desc.occlusion_strength),
      .uv_alpha = glm::vec4(desc.uv_scale, desc.alpha_cutoff, 0.0f),
      .options = glm::uvec4(UvSetBits(desc), MaterialFlags(desc), 0, 0),
  };
}

MaterialSystem::MaterialSystem(const MetalContext& /*context*/,
                               const TextureCache& textures,
                               const Samplers& samplers)
    : textures_(textures), samplers_(samplers) {}

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
  added.textures = std::move(textures);
  added.variant = VariantOf(added.desc);
  added.gpu = {.params = MakeMaterialParams(added.desc),
               .textures = SlotTextures(added)};
  return static_cast<MaterialId>(materials_.size() - 1);
}

void MaterialSystem::Update(MaterialId id, const material::MaterialDesc& desc) {
  const auto index = static_cast<size_t>(id);
  if (index >= materials_.size()) {
    core::log::Fatal("Update of unknown material {}", index);
  }
  if (materials_[index].released) {
    core::log::Warn("Update of released material {} ignored", index);
    return;
  }
  // Frames already recorded hold a copy of the old parameters (see the
  // class comment), so they can be overwritten in place.
  Material& updated = materials_[index];
  updated.desc = material::Sanitize(desc);
  updated.variant = VariantOf(updated.desc);
  updated.gpu.params = MakeMaterialParams(updated.desc);
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
  // Frames in flight that sampled them rely on the renderer's retire list;
  // see the class comment.
  released.gpu.textures = {};
}

void MaterialSystem::Bind(const Recording& rec, MaterialId id) const {
  const GpuState& gpu = materials_[static_cast<size_t>(Resolve(id))].gpu;
  BindConstants(rec, kBufferMaterial, gpu.params);
  for (NSUInteger slot = 0; slot < kMaterialTextureCount; ++slot) {
    BindTexture(rec, kTextureBaseColor + slot, gpu.textures[slot]);
  }
  BindSampler(rec, kSamplerMaterial, samplers_.material(lod_bias_));
}

std::array<id<MTLTexture>, kMaterialTextureCount> MaterialSystem::SlotTextures(
    const Material& material) const {
  const MaterialTextures uploaded =
      material.textures.value_or(MaterialTextures{});
  return {
      textures_.TextureOr(uploaded.base_color, DefaultTexture::kWhite),
      textures_.TextureOr(uploaded.metallic_roughness, DefaultTexture::kWhite),
      textures_.TextureOr(uploaded.normal, DefaultTexture::kFlatNormal),
      textures_.TextureOr(uploaded.occlusion, DefaultTexture::kWhite),
      textures_.TextureOr(uploaded.emissive, DefaultTexture::kWhite),
  };
}

}  // namespace blue::render
