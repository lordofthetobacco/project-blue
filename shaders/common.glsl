// Declarations shared by the mesh shaders. Included with #include "common.glsl".

// Per-frame data, one uniform buffer per frame in flight. Must match
// FrameUniforms in src/render/renderer.cc (std140).
layout(set = 0, binding = 0) uniform FrameUniforms {
    mat4 viewProj;
    vec4 cameraPosition;
    vec4 lightPosition;
    vec4 lightColor;   // rgb: color, a: intensity
    vec4 ambientColor;
} frame;
