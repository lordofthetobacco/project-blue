// Declarations shared by the scene shaders (render::FrameGlobals). Included
// with #include "common.h" after <metal_stdlib> and "bindings.h"; safe to
// include more than once. Types are qualified with metal:: so that the
// header does not depend on the including file's `using namespace metal;`.
//
// FrameGlobals::Bind() sets the frame's resources on every encoder of a
// scene pass. An entry point declares the ones it uses as parameters; a
// fragment shader that needs all of them (lighting, shadows) declares
//
//     constant FrameUniforms& frame [[buffer(kBufferFrame)]],
//     depth2d_array<float> shadowMap [[texture(kTextureShadowMap)]],
//     sampler shadowSampler [[sampler(kSamplerShadow)]],
//     texturecube<float> environmentMap [[texture(kTextureEnvironment)]],
//     texture2d<float> brdfLut [[texture(kTextureBrdfLut)]],
//     texture2d<float> ambientOcclusionMap [[texture(kTextureAmbientOcclusion)]],
//     sampler linearClamp [[sampler(kSamplerLinearClamp)]]
//
// and gathers them for the helpers, which take `thread const FrameResources&`:
//
//     const FrameResources res = {&frame, shadowMap, shadowSampler, environmentMap,
//                                 brdfLut, ambientOcclusionMap, linearClamp};
//
// A vertex shader (or a fragment shader that only reads the uniforms)
// declares just `constant FrameUniforms& frame [[buffer(kBufferFrame)]]`.
// kSamplerNearestClamp is bound as well, for exact texel reads by uv.
#ifndef BLUE_SHADERS_COMMON_H_
#define BLUE_SHADERS_COMMON_H_

constant int kCascadeCount = 4;
// Must match render::kMaxPointLights.
constant int kMaxPointLights = 32;

// Must match render::GpuPointLight.
struct GpuPointLight {
    float4 positionInverseRangeSq;  // xyz: world position, w: 1 / range^2 (0: unlimited)
    float4 radiance;                // rgb: color * intensity
};

// Per-frame data, one buffer per frame in flight. Must match FrameUniforms
// in src/render/frame_uniforms.h (the C++ side asserts every offset; float4
// and float4x4 members leave no padding, so the layouts are identical).
struct FrameUniforms {
    metal::float4x4 viewProj;
    metal::float4x4 view;                       // World to camera view space.
    metal::float4x4 projection;                 // Reversed-Z, infinite far.
    metal::float4x4 inverseProjection;
    metal::float4x4 inverseViewProj;
    float4 cameraPosition;
    float4 ambientColor;
    float4 sunDirection;                        // xyz: direction the light travels
    float4 sunColor;                            // rgb: color * intensity
    metal::float4x4 cascadeViewProjection[kCascadeCount]; // World to light clip space.
    float4 cascadeSplitFar;                     // View-space far distance per cascade.
    float4 cascadeTexelWorldSize;               // Metres per shadow-map texel.
    float4 shadowParams;                        // x: shadows on, y: cascade debug,
                                                // z: shadow distance, w: normal offset scale
    float4 screen;                              // xy: target size in pixels, zw: 1 / size
    float4 irradianceSh[9];                     // Environment irradiance, SH9, rgb
    float4 environmentParams;                   // x: intensity, y: rotation (rad),
                                                // z: max prefiltered mip, w: 1 if bound
    uint4 pointLightCount;                      // x: valid entries of pointLights
    GpuPointLight pointLights[kMaxPointLights]; // Culled on the CPU, unshadowed.
};
static_assert(sizeof(FrameUniforms) == 1904,
              "FrameUniforms must match src/render/frame_uniforms.h");

// The frame's resources, gathered by the entry point (see the top of this
// file) so that helpers take one argument.
struct FrameResources {
    constant FrameUniforms* frame;
    // Cascaded sun shadow map, one layer per cascade; sample_compare against
    // the reference depth with shadowSampler. A 1x1 map at depth 1 (lit)
    // until the shadow pass binds its own.
    metal::depth2d_array<float> shadowMap;
    metal::sampler shadowSampler;
    // Prefiltered specular environment (GGX, roughness across mips).
    metal::texturecube<float> environmentMap;
    // Split-sum BRDF lookup: x = NdotV, y = roughness; rg = scale, bias.
    metal::texture2d<float> brdfLut;
    // Screen-space ambient occlusion (r), half resolution. White while SSAO
    // is off.
    metal::texture2d<float> ambientOcclusionMap;
    // Bilinear, clamped to the edge: environmentMap, brdfLut,
    // ambientOcclusionMap.
    metal::sampler linearClamp;
};

#endif  // BLUE_SHADERS_COMMON_H_
