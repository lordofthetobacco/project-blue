// Material set (set 1, render MaterialSystem), shared by mesh.slang and the
// alpha-masked depth pre-pass. Included with #include "material.slang".
#ifndef BLUE_SHADERS_MATERIAL_SLANG_
#define BLUE_SHADERS_MATERIAL_SLANG_

#include "bindings.slang"

// Must match MaterialParams in src/render/vulkan/material_system.h (only
// 16-byte members).
struct MaterialParams {
    float4 baseColor;  // linear RGBA factor
    float4 emissive;   // rgb: emissive * strength
    float4 surface;    // x: metallic, y: roughness, z: normal scale, w: occlusion strength
    float4 uvAlpha;    // xy: uv scale, z: alpha cutoff
    // x: bit i set = texture slot i samples uv1 (slots in binding order:
    //    base colour, metallic-roughness, normal, occlusion, emissive);
    // y: kMaterialFlag* bits.
    uint4 options;
};

BLUE_UNIFORMS(MaterialParams, material, BLUE_MATERIAL_PARAMS_BINDING, BLUE_PASS_SET);
// sRGB.
BLUE_TEXTURE(Texture2D<float4>, baseColorMap, BLUE_MATERIAL_BASE_COLOR_BINDING, BLUE_PASS_SET);
// g: roughness, b: metallic.
BLUE_TEXTURE(Texture2D<float4>, metallicRoughnessMap, BLUE_MATERIAL_METALLIC_ROUGHNESS_BINDING,
             BLUE_PASS_SET);
// Tangent space, +Y up.
BLUE_TEXTURE(Texture2D<float4>, normalMap, BLUE_MATERIAL_NORMAL_BINDING, BLUE_PASS_SET);
// r.
BLUE_TEXTURE(Texture2D<float4>, occlusionMap, BLUE_MATERIAL_OCCLUSION_BINDING, BLUE_PASS_SET);
// sRGB.
BLUE_TEXTURE(Texture2D<float4>, emissiveMap, BLUE_MATERIAL_EMISSIVE_BINDING, BLUE_PASS_SET);
// Trilinear, repeating, anisotropic: for every map above.
BLUE_SAMPLER(SamplerState, materialSampler, BLUE_MATERIAL_SAMPLER_BINDING, BLUE_PASS_SET);

// The normal map stores xy only (BC5); z is reconstructed.
static const uint kMaterialFlagNormalRg = 1u;
// Occlusion is the R channel of metallicRoughnessMap (ORM packing).
static const uint kMaterialFlagOcclusionInMetallicRoughness = 2u;

static const uint kSlotBaseColor = 0u;
static const uint kSlotMetallicRoughness = 1u;
static const uint kSlotNormal = 2u;
static const uint kSlotOcclusion = 3u;
static const uint kSlotEmissive = 4u;

// Returns the scaled texture coordinates texture slot `slot` samples with.
float2 MaterialUv(uint slot, float2 uv0, float2 uv1)
{
    const bool second = (material.options.x & (1u << slot)) != 0u;
    return (second ? uv1 : uv0) * material.uvAlpha.xy;
}

#endif  // BLUE_SHADERS_MATERIAL_SLANG_
