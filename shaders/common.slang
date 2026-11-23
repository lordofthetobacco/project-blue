// Declarations shared by the scene shaders (set 0, render FrameGlobals).
// Included with #include "common.slang"; safe to include more than once.
#ifndef BLUE_SHADERS_COMMON_SLANG_
#define BLUE_SHADERS_COMMON_SLANG_

#include "bindings.slang"

static const int kCascadeCount = BLUE_CASCADE_COUNT;
// Must match render::kMaxPointLights.
static const int kMaxPointLights = BLUE_MAX_POINT_LIGHTS;

// Must match render::GpuPointLight.
struct GpuPointLight {
    float4 positionInverseRangeSq;  // xyz: world position, w: 1 / range^2 (0: unlimited)
    float4 radiance;                // rgb: color * intensity
};

// Per-frame data, one uniform buffer per frame in flight. Must match
// FrameUniforms in src/render/frame_uniforms.h (the C++ side asserts every
// offset; tests/shader_bindings_test.cc compares them with this block).
// Only 16-byte members, so std140 and the Direct3D packing agree.
struct FrameUniforms {
    float4x4 viewProj;
    float4x4 view;                                 // World to camera view space.
    float4x4 projection;                           // Reversed-Z, infinite far.
    float4x4 inverseProjection;
    float4x4 inverseViewProj;
    float4 cameraPosition;
    float4 ambientColor;
    float4 sunDirection;                           // xyz: direction the light travels
    float4 sunColor;                               // rgb: color * intensity
    float4x4 cascadeViewProjection[kCascadeCount]; // World to light clip space.
    float4 cascadeSplitFar;                        // View-space far distance per cascade.
    float4 cascadeTexelWorldSize;                  // Metres per shadow-map texel.
    float4 shadowParams;                           // x: shadows on, y: cascade debug,
                                                   // z: shadow distance, w: normal offset scale
    float4 screen;                                 // xy: target size in pixels, zw: 1 / size
    float4 irradianceSh[9];                        // Environment irradiance, SH9, rgb
    float4 environmentParams;                      // x: intensity, y: rotation (rad),
                                                   // z: max prefiltered mip, w: 1 if bound
    uint4 pointLightCount;                         // x: valid entries of pointLights
    GpuPointLight pointLights[kMaxPointLights];    // Culled on the CPU, unshadowed.
    float4x4 previousViewProj;                     // Last frame's viewProj (motion blur).
    float4 motionBlur;                             // x: blur scale (0: off), y: max blur
                                                   // length (px), z: max radius (px)
    float4x4 unjitteredViewProj;                   // viewProj without the jitter.
    float4 jitter;                                 // xy: jitter in pixels (x right,
                                                   // y down); 0 without a temporal scaler.
};

BLUE_UNIFORMS(FrameUniforms, frame, BLUE_FRAME_UNIFORMS_BINDING, BLUE_FRAME_SET);

// Cascaded sun shadow map, one layer per cascade; sampled with shadowSampler,
// which compares against the reference depth.
BLUE_TEXTURE(Texture2DArray<float>, shadowMap, BLUE_FRAME_SHADOW_MAP_BINDING, BLUE_FRAME_SET);
// Prefiltered specular environment (GGX, roughness across mips). Black 1x1
// until an environment is bound.
BLUE_TEXTURE(TextureCube<float4>, environmentMap, BLUE_FRAME_ENVIRONMENT_BINDING, BLUE_FRAME_SET);
// Split-sum BRDF lookup: x = NdotV, y = roughness; rg = scale, bias.
BLUE_TEXTURE(Texture2D<float4>, brdfLut, BLUE_FRAME_BRDF_LUT_BINDING, BLUE_FRAME_SET);
// Screen-space ambient occlusion (r). White 1x1 until SSAO exists.
BLUE_TEXTURE(Texture2D<float4>, ambientOcclusionMap, BLUE_FRAME_AO_BINDING, BLUE_FRAME_SET);
BLUE_SAMPLER(SamplerComparisonState, shadowSampler, BLUE_FRAME_SHADOW_SAMPLER_BINDING,
             BLUE_FRAME_SET);
// Bilinear, clamped: for environmentMap, brdfLut and ambientOcclusionMap.
BLUE_SAMPLER(SamplerState, frameLinearSampler, BLUE_FRAME_LINEAR_SAMPLER_BINDING, BLUE_FRAME_SET);

// The clip-space position of a model-space vertex. The depth pre-pass, the
// mesh pass, the velocity pass and the selection mask all call exactly this
// and test depth EQUAL against each other: `precise` (Direct3D) and the
// Invariant decoration the Vulkan loader adds to every vertex shader's
// position (shader::MarkPositionInvariant) make the results bit-identical.
float4 ClipPosition(float4x4 model, float3 position, out float4 worldPosition)
{
    precise float4 world = mul(model, float4(position, 1.0));
    precise float4 clip = mul(frame.viewProj, world);
    worldPosition = world;
    return clip;
}

#endif  // BLUE_SHADERS_COMMON_SLANG_
