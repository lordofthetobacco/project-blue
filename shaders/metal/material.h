// Material resources (render::MaterialSystem::Bind()), shared by
// mesh.frag.metal and the alpha-masked depth pre-pass. Included with
// #include "material.h" after <metal_stdlib> and "bindings.h"; safe to
// include more than once. Types are qualified with metal:: so that the
// header does not depend on the including file's `using namespace metal;`.
//
// A fragment shader declares the material's resources as parameters
//
//     constant MaterialParams& material [[buffer(kBufferMaterial)]],
//     texture2d<float> baseColorMap [[texture(kTextureBaseColor)]],
//     texture2d<float> metallicRoughnessMap [[texture(kTextureMetallicRoughness)]],
//     texture2d<float> normalMap [[texture(kTextureNormal)]],
//     texture2d<float> occlusionMap [[texture(kTextureOcclusion)]],
//     texture2d<float> emissiveMap [[texture(kTextureEmissive)]],
//     sampler materialSampler [[sampler(kSamplerMaterial)]]
//
// and gathers them for the helpers, which take
// `thread const MaterialResources&`:
//
//     const MaterialResources mat = {&material, baseColorMap, metallicRoughnessMap,
//                                    normalMap, occlusionMap, emissiveMap,
//                                    materialSampler};
//
// MaterialUv() takes the parameters alone, so that a shader that reads only
// some of the slots (depth_masked.frag.metal) declares only those.
#ifndef BLUE_SHADERS_METAL_MATERIAL_H_
#define BLUE_SHADERS_METAL_MATERIAL_H_

// Must match MaterialParams in src/render/material_system.h (float4 / uint4
// members only, so the layouts are identical).
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
static_assert(sizeof(MaterialParams) == 80,
              "MaterialParams must match src/render/material_system.h");

// The normal map stores xy only (BC5); z is reconstructed.
constant uint kMaterialFlagNormalRg = 1u;
// Occlusion is the R channel of metallicRoughnessMap (ORM packing).
constant uint kMaterialFlagOcclusionInMetallicRoughness = 2u;

constant uint kSlotBaseColor = 0u;
constant uint kSlotMetallicRoughness = 1u;
constant uint kSlotNormal = 2u;
constant uint kSlotOcclusion = 3u;
constant uint kSlotEmissive = 4u;

// The material's resources, gathered by the entry point (see the top of
// this file) so that helpers take one argument.
struct MaterialResources {
    constant MaterialParams* params;
    metal::texture2d<float> baseColorMap;          // sRGB
    metal::texture2d<float> metallicRoughnessMap;  // g: roughness, b: metallic
    metal::texture2d<float> normalMap;             // tangent space, +Y up
    metal::texture2d<float> occlusionMap;          // r
    metal::texture2d<float> emissiveMap;           // sRGB
    // Trilinear, repeating, anisotropic: every material texture.
    metal::sampler materialSampler;
};

// Returns the scaled texture coordinates texture slot `slot` samples with.
static inline float2 MaterialUv(constant MaterialParams& material, uint slot, float2 uv0,
                                float2 uv1)
{
    const bool second = (material.options.x & (1u << slot)) != 0u;
    return (second ? uv1 : uv0) * material.uvAlpha.xy;
}

#endif  // BLUE_SHADERS_METAL_MATERIAL_H_
