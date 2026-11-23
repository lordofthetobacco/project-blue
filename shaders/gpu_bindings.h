// Binding numbers shared by the Slang shaders and C++ (render/binding_layout.h),
// so a set, a binding or an array size is written down once. Plain #defines
// only: the file is included by both languages.
//
// Vulkan: (set, binding). Direct3D 12: register b<binding> (uniform block),
// t<binding> (texture) or s<binding> (sampler) in space<set>. A number is
// used once per set whatever the kind, so both APIs share it.
#ifndef BLUE_SHADERS_GPU_BINDINGS_H_
#define BLUE_SHADERS_GPU_BINDINGS_H_

// Scene programs: set 0 is the frame (render FrameGlobals).
#define BLUE_FRAME_SET 0
#define BLUE_FRAME_UNIFORMS_BINDING 0
#define BLUE_FRAME_SHADOW_MAP_BINDING 1
#define BLUE_FRAME_ENVIRONMENT_BINDING 2
#define BLUE_FRAME_BRDF_LUT_BINDING 3
#define BLUE_FRAME_AO_BINDING 4
// Compares against the shadow map's depth.
#define BLUE_FRAME_SHADOW_SAMPLER_BINDING 5
// Bilinear, clamped: the environment, the BRDF LUT and the AO.
#define BLUE_FRAME_LINEAR_SAMPLER_BINDING 6

// Scene programs: set 1 is what the pass adds (one of the following).
#define BLUE_PASS_SET 1
// Materials (render MaterialSystem).
#define BLUE_MATERIAL_PARAMS_BINDING 0
#define BLUE_MATERIAL_BASE_COLOR_BINDING 1
#define BLUE_MATERIAL_METALLIC_ROUGHNESS_BINDING 2
#define BLUE_MATERIAL_NORMAL_BINDING 3
#define BLUE_MATERIAL_OCCLUSION_BINDING 4
#define BLUE_MATERIAL_EMISSIVE_BINDING 5
// Samples every texture of the material.
#define BLUE_MATERIAL_SAMPLER_BINDING 6
// The skybox (render SkyboxPass).
#define BLUE_SKY_CUBE_BINDING 0
#define BLUE_SKY_SAMPLER_BINDING 1
// The scope eyepiece (render ScopeLensPass).
#define BLUE_SCOPE_UNIFORMS_BINDING 0
#define BLUE_SCOPE_PICTURE_BINDING 1
#define BLUE_SCOPE_SAMPLER_BINDING 2

// Full-screen programs: set 0 holds the pass's input textures, bindings
// 0 .. n - 1, and the one sampler they share.
#define BLUE_INPUT_SET 0
#define BLUE_MAX_FULLSCREEN_INPUTS 4
#define BLUE_INPUT_SAMPLER_BINDING 4

// Push constants: Vulkan's push constant block; Direct3D 12 root constants
// at register b0 of this space. At most 128 bytes (the Vulkan minimum).
#define BLUE_PUSH_CONSTANT_SPACE 8
#define BLUE_MAX_PUSH_CONSTANT_BYTES 128

// Array sizes of the uniform blocks.
#define BLUE_CASCADE_COUNT 4          // lighting::kCascadeCount
#define BLUE_MAX_POINT_LIGHTS 32      // render::kMaxPointLights
#define BLUE_MAX_SCOPE_RETICLE_LINES 8  // render::kMaxScopeReticleLines

#endif  // BLUE_SHADERS_GPU_BINDINGS_H_
