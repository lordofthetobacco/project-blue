#ifndef BLUE_RENDER_BINDING_LAYOUT_H_
#define BLUE_RENDER_BINDING_LAYOUT_H_

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

#include "shaders/gpu_bindings.h"

// The resource bindings of the shader programs as data: one table that the
// shaders are written against (shaders/gpu_bindings.h has the numbers), that
// the Vulkan backend builds its descriptor set layouts from and the
// Direct3D 12 backend its root signatures, and that
// tests/shader_bindings_test.cc compares with the reflection of every
// compiled program. No graphics API type.
//
// A binding is (set, number): Vulkan's (set, binding); in Direct3D 12
// register b<number> / t<number> / s<number> of space<set>, by kind.

namespace blue::render {

enum class BindingKind : uint8_t {
  kUniformBuffer,  // ConstantBuffer<T>
  kTexture,        // Texture2D, Texture2DArray, TextureCube
  kSampler,        // SamplerState, SamplerComparisonState
};

// Stages that may use a binding.
inline constexpr uint8_t kStageVertex = 1;
inline constexpr uint8_t kStageFragment = 2;

struct BindingDesc {
  // The variable in the shaders.
  std::string_view name;
  uint32_t binding = 0;
  BindingKind kind = BindingKind::kUniformBuffer;
  uint8_t stages = kStageFragment;
};

// The set (space) numbers.
inline constexpr uint32_t kFrameSet = BLUE_FRAME_SET;
inline constexpr uint32_t kPassSet = BLUE_PASS_SET;
inline constexpr uint32_t kInputSet = BLUE_INPUT_SET;
// Direct3D 12: the space of the root constants that stand for push constants.
inline constexpr uint32_t kPushConstantSpace = BLUE_PUSH_CONSTANT_SPACE;
inline constexpr uint32_t kMaxPushConstantBytes = BLUE_MAX_PUSH_CONSTANT_BYTES;

// Scene programs, set kFrameSet: the frame (FrameGlobals).
inline constexpr std::array<BindingDesc, 7> kFrameBindings = {{
    {"frame", BLUE_FRAME_UNIFORMS_BINDING, BindingKind::kUniformBuffer,
     kStageVertex | kStageFragment},
    {"shadowMap", BLUE_FRAME_SHADOW_MAP_BINDING, BindingKind::kTexture},
    {"environmentMap", BLUE_FRAME_ENVIRONMENT_BINDING, BindingKind::kTexture},
    {"brdfLut", BLUE_FRAME_BRDF_LUT_BINDING, BindingKind::kTexture},
    {"ambientOcclusionMap", BLUE_FRAME_AO_BINDING, BindingKind::kTexture},
    {"shadowSampler", BLUE_FRAME_SHADOW_SAMPLER_BINDING, BindingKind::kSampler},
    {"frameLinearSampler", BLUE_FRAME_LINEAR_SAMPLER_BINDING,
     BindingKind::kSampler},
}};

// Scene programs, set kPassSet: a material (MaterialSystem). The textures
// are in material::TextureSlot order.
inline constexpr uint32_t kMaterialTextureCount = 5;
inline constexpr std::array<BindingDesc, 7> kMaterialBindings = {{
    {"material", BLUE_MATERIAL_PARAMS_BINDING, BindingKind::kUniformBuffer},
    {"baseColorMap", BLUE_MATERIAL_BASE_COLOR_BINDING, BindingKind::kTexture},
    {"metallicRoughnessMap", BLUE_MATERIAL_METALLIC_ROUGHNESS_BINDING,
     BindingKind::kTexture},
    {"normalMap", BLUE_MATERIAL_NORMAL_BINDING, BindingKind::kTexture},
    {"occlusionMap", BLUE_MATERIAL_OCCLUSION_BINDING, BindingKind::kTexture},
    {"emissiveMap", BLUE_MATERIAL_EMISSIVE_BINDING, BindingKind::kTexture},
    {"materialSampler", BLUE_MATERIAL_SAMPLER_BINDING, BindingKind::kSampler},
}};

// The skybox program, set kPassSet.
inline constexpr std::array<BindingDesc, 2> kSkyBindings = {{
    {"skyMap", BLUE_SKY_CUBE_BINDING, BindingKind::kTexture},
    {"skySampler", BLUE_SKY_SAMPLER_BINDING, BindingKind::kSampler},
}};

// The scope eyepiece program, set kPassSet.
inline constexpr std::array<BindingDesc, 3> kScopeBindings = {{
    {"scope", BLUE_SCOPE_UNIFORMS_BINDING, BindingKind::kUniformBuffer,
     kStageVertex | kStageFragment},
    {"scopePicture", BLUE_SCOPE_PICTURE_BINDING, BindingKind::kTexture},
    {"scopeSampler", BLUE_SCOPE_SAMPLER_BINDING, BindingKind::kSampler},
}};

// Full-screen programs, set kInputSet: the pass's input textures at bindings
// 0 .. n - 1 (n <= kMaxFullscreenInputs; their names differ per program) and
// the sampler they share.
inline constexpr uint32_t kMaxFullscreenInputs = BLUE_MAX_FULLSCREEN_INPUTS;
inline constexpr BindingDesc kFullscreenSampler{
    "inputSampler", BLUE_INPUT_SAMPLER_BINDING, BindingKind::kSampler};

// The sets a scene program may use, for checks: the frame, then one of the
// pass sets.
inline constexpr std::array<std::span<const BindingDesc>, 3> kPassSets = {
    std::span<const BindingDesc>(kMaterialBindings),
    std::span<const BindingDesc>(kSkyBindings),
    std::span<const BindingDesc>(kScopeBindings),
};

}  // namespace blue::render

#endif  // BLUE_RENDER_BINDING_LAYOUT_H_
