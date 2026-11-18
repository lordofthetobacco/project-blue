// Material set (set 1, render::MaterialSystem), shared by mesh.frag and the
// alpha-masked depth pre-pass. Included with #include "material.glsl".

// Must match MaterialParams in src/render/material_system.h (std140).
layout(set = 1, binding = 0) uniform MaterialParams {
    vec4 baseColor;  // linear RGBA factor
    vec4 emissive;   // rgb: emissive * strength
    vec4 surface;    // x: metallic, y: roughness, z: normal scale, w: occlusion strength
    vec4 uvAlpha;    // xy: uv scale, z: alpha cutoff
    // x: bit i set = texture slot i samples uv1 (slots in binding order:
    //    base colour, metallic-roughness, normal, occlusion, emissive);
    // y: kMaterialFlag* bits.
    uvec4 options;
} material;

// The normal map stores xy only (BC5); z is reconstructed.
const uint kMaterialFlagNormalRg = 1u;
// Occlusion is the R channel of metallicRoughnessMap (ORM packing).
const uint kMaterialFlagOcclusionInMetallicRoughness = 2u;

const uint kSlotBaseColor = 0u;
const uint kSlotMetallicRoughness = 1u;
const uint kSlotNormal = 2u;
const uint kSlotOcclusion = 3u;
const uint kSlotEmissive = 4u;

// Returns the scaled texture coordinates texture slot `slot` samples with.
vec2 MaterialUv(uint slot, vec2 uv0, vec2 uv1)
{
    const bool second = (material.options.x & (1u << slot)) != 0u;
    return (second ? uv1 : uv0) * material.uvAlpha.xy;
}
layout(set = 1, binding = 1) uniform sampler2D baseColorMap;          // sRGB
layout(set = 1, binding = 2) uniform sampler2D metallicRoughnessMap;  // g: roughness, b: metallic
layout(set = 1, binding = 3) uniform sampler2D normalMap;             // tangent space, +Y up
layout(set = 1, binding = 4) uniform sampler2D occlusionMap;          // r
layout(set = 1, binding = 5) uniform sampler2D emissiveMap;           // sRGB
