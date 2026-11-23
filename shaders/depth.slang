// Depth-only drawing of the camera view. Programs:
//   depth                   VertexMain alone: the depth pre-pass
//                           (render DepthPrepass).
//   depth_masked            + MaskedFragmentMain: alpha-masked materials.
//   selection_mask_hidden,  + SelectionMaskFragmentMain (define VISIBLE):
//   selection_mask_visible  coverage of the selected items (render
//                           OverlayPass).

#include "common.slang"
#include "material.slang"

// Must match DrawConstants in src/render/vulkan/draw_record.cc.
struct DrawConstants {
    float4x4 model;
};
BLUE_PUSH_CONSTANTS(DrawConstants, draw);

struct VertexInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(2)]] float2 uv0 : TEXCOORD0;
    [[vk::location(4)]] float2 uv1 : TEXCOORD1;
};

struct Varyings {
    // Must be exactly the depth of mesh.slang: both call ClipPosition().
    float4 clipPosition : SV_Position;
    float4 uv : TEXCOORD0;  // xy: uv0, zw: uv1
};

[shader("vertex")]
Varyings VertexMain(VertexInput input)
{
    Varyings output;
    float4 worldPosition;
    output.clipPosition = ClipPosition(draw.model, input.position, worldPosition);
    output.uv = float4(input.uv0, input.uv1);
    return output;
}

// Alpha test of the depth pre-pass for alpha-masked materials: the same
// cutoff as mesh.slang, so the pre-pass keeps exactly the fragments the mesh
// pass shades. Writes depth only.
[shader("fragment")]
void MaskedFragmentMain(Varyings input)
{
    const float2 uv = MaterialUv(kSlotBaseColor, input.uv.xy, input.uv.zw);
    const float alpha = baseColorMap.Sample(materialSampler, uv).a * material.baseColor.a;
    if (alpha < material.uvAlpha.z) {
        discard;
    }
}

#ifndef VISIBLE
#define VISIBLE 0
#endif

// Coverage of the selected draw items: 0.5 where an item is hidden by the
// scene (pipeline without a depth test), 1 where it is visible
// (depth-tested pipeline, drawn second).
[shader("fragment")]
float4 SelectionMaskFragmentMain() : SV_Target
{
    const float coverage = VISIBLE != 0 ? 1.0 : 0.5;
    return float4(coverage);
}
