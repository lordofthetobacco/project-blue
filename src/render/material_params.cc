#include "render/material_params.h"

#include <cstddef>
#include <cstdint>

#include <glm/ext/vector_uint4.hpp>
#include <glm/vec4.hpp>

#include "material/material.h"

namespace blue::render {

namespace {

static_assert(offsetof(MaterialParams, emissive) == 16);
static_assert(offsetof(MaterialParams, surface) == 32);
static_assert(offsetof(MaterialParams, uv_alpha) == 48);
static_assert(offsetof(MaterialParams, options) == 64);
static_assert(sizeof(MaterialParams) == 80);

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

}  // namespace blue::render
