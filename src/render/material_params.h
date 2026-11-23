#ifndef BLUE_RENDER_MATERIAL_PARAMS_H_
#define BLUE_RENDER_MATERIAL_PARAMS_H_

#include <cstdint>

#include <glm/ext/vector_uint4.hpp>
#include <glm/vec4.hpp>

#include "material/material.h"

namespace blue::render {

// A material's shader parameters, shared by the Vulkan and Direct3D 12
// backends (the uniform block of the material set). Must match
// MaterialParams in shaders/material.slang (tests/shader_bindings_test.cc
// compares the offsets with the shaders' reflection).
struct MaterialParams {
  glm::vec4 base_color;  // linear RGBA factor
  glm::vec4 emissive;    // rgb: emissive * emissive_strength
  // x: metallic, y: roughness, z: normal scale, w: occlusion strength.
  glm::vec4 surface;
  glm::vec4 uv_alpha;  // xy: uv scale, z: alpha cutoff
  // x: bit i set = texture slot i (material::TextureSlot) samples uv1;
  // y: kMaterialFlag* bits.
  glm::uvec4 options;
};

// MaterialParams::options.y bits; must match shaders/material.slang.
inline constexpr uint32_t kMaterialFlagNormalRg = 1;
inline constexpr uint32_t kMaterialFlagOcclusionInMetallicRoughness = 2;

// Returns the shader parameters of `desc`.
[[nodiscard]] MaterialParams MakeMaterialParams(
    const material::MaterialDesc& desc);

}  // namespace blue::render

#endif  // BLUE_RENDER_MATERIAL_PARAMS_H_
