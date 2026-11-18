// Declarations shared by the scene shaders (set 0, render::FrameGlobals).
// Included with #include "common.glsl".

const int kCascadeCount = 4;
// Must match render::kMaxPointLights.
const int kMaxPointLights = 32;

// Must match render::GpuPointLight.
struct GpuPointLight {
    vec4 positionInverseRangeSq;  // xyz: world position, w: 1 / range^2 (0: unlimited)
    vec4 radiance;                // rgb: color * intensity
};

// Per-frame data, one uniform buffer per frame in flight. Must match
// FrameUniforms in src/render/frame_globals.h (std140; the C++ side asserts
// every offset).
layout(set = 0, binding = 0) uniform FrameUniforms {
    mat4 viewProj;
    mat4 view;                                // World to camera view space.
    mat4 projection;                          // Reversed-Z, infinite far.
    mat4 inverseProjection;
    mat4 inverseViewProj;
    vec4 cameraPosition;
    vec4 ambientColor;
    vec4 sunDirection;                        // xyz: direction the light travels
    vec4 sunColor;                            // rgb: color * intensity
    mat4 cascadeViewProjection[kCascadeCount]; // World to light clip space.
    vec4 cascadeSplitFar;                     // View-space far distance per cascade.
    vec4 cascadeTexelWorldSize;               // Metres per shadow-map texel.
    vec4 shadowParams;                        // x: shadows on, y: cascade debug,
                                              // z: shadow distance, w: normal offset scale
    vec4 screen;                              // xy: target size in pixels, zw: 1 / size
    vec4 irradianceSh[9];                     // Environment irradiance, SH9, rgb
    vec4 environmentParams;                   // x: intensity, y: rotation (rad),
                                              // z: max prefiltered mip, w: 1 if bound
    uvec4 pointLightCount;                    // x: valid entries of pointLights
    GpuPointLight pointLights[kMaxPointLights]; // Culled on the CPU, unshadowed.
} frame;

// Cascaded sun shadow map, one layer per cascade; compares against the
// reference depth passed as the last texture coordinate.
layout(set = 0, binding = 1) uniform sampler2DArrayShadow shadowMap;
// Prefiltered specular environment (GGX, roughness across mips). Black 1x1
// until an environment is bound.
layout(set = 0, binding = 2) uniform samplerCube environmentMap;
// Split-sum BRDF lookup: x = NdotV, y = roughness; rg = scale, bias.
layout(set = 0, binding = 3) uniform sampler2D brdfLut;
// Screen-space ambient occlusion (r). White 1x1 until SSAO exists.
layout(set = 0, binding = 4) uniform sampler2D ambientOcclusionMap;
