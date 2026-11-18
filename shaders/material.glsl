// Material set (set 1, render::MaterialSystem), shared by mesh.frag and the
// alpha-masked depth pre-pass. Included with #include "material.glsl".

// Must match MaterialParams in src/render/material_system.h (std140).
layout(set = 1, binding = 0) uniform MaterialParams {
    vec4 baseColor;  // linear RGBA factor
    vec4 emissive;   // rgb: emissive * strength
    vec4 surface;    // x: metallic, y: roughness, z: normal scale, w: occlusion strength
    vec4 uvAlpha;    // xy: uv scale, z: alpha cutoff
} material;
layout(set = 1, binding = 1) uniform sampler2D baseColorMap;          // sRGB
layout(set = 1, binding = 2) uniform sampler2D metallicRoughnessMap;  // g: roughness, b: metallic
layout(set = 1, binding = 3) uniform sampler2D normalMap;             // tangent space, +Y up
layout(set = 1, binding = 4) uniform sampler2D occlusionMap;          // r
layout(set = 1, binding = 5) uniform sampler2D emissiveMap;           // sRGB
