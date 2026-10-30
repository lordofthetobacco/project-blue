// Declarations shared by the mesh shaders. Included with #include "common.glsl".

const int kCascadeCount = 4;

// Per-frame data, one uniform buffer per frame in flight. Must match
// FrameUniforms in src/render/renderer.cc (std140; the C++ side asserts every
// offset).
layout(set = 0, binding = 0) uniform FrameUniforms {
    mat4 viewProj;
    mat4 view;                                // World to camera view space.
    vec4 cameraPosition;
    vec4 lightPosition;
    vec4 lightColor;                          // rgb: color, a: intensity
    vec4 ambientColor;
    vec4 sunDirection;                        // xyz: direction the light travels
    vec4 sunColor;                            // rgb: color * intensity
    mat4 cascadeViewProjection[kCascadeCount]; // World to light clip space.
    vec4 cascadeSplitFar;                     // View-space far distance per cascade.
    vec4 cascadeTexelWorldSize;               // Metres per shadow-map texel.
    vec4 shadowParams;                        // x: shadows on, y: cascade debug,
                                              // z: shadow distance, w: normal offset scale
} frame;

// Cascaded sun shadow map, one layer per cascade; compares against the
// reference depth passed as the last texture coordinate.
layout(set = 0, binding = 1) uniform sampler2DArrayShadow shadowMap;
