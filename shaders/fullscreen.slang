// The vertex stage of every full-screen program (render FullscreenPass).
// Included with #include "fullscreen.slang" by the program's file, which adds
// FragmentMain(FullscreenVaryings).
#ifndef BLUE_SHADERS_FULLSCREEN_SLANG_
#define BLUE_SHADERS_FULLSCREEN_SLANG_

#include "bindings.slang"

// The sampler of the pass's inputs (FullscreenPass::SetInputs()).
BLUE_SAMPLER(SamplerState, inputSampler, BLUE_INPUT_SAMPLER_BINDING, BLUE_INPUT_SET);

struct FullscreenVaryings {
    // xy in the fragment stage: the pixel centre (x + 0.5, y + 0.5), y down.
    float4 position : SV_Position;
    // [0, 1], (0, 0) at the top-left pixel corner.
    float2 uv : TEXCOORD0;
};

// One triangle that covers the whole viewport. No vertex buffer: vertex i is
// at (-1, -1), (3, -1), (-1, 3). The viewport is not flipped, so clip
// y = -1 is the top row.
[shader("vertex")]
FullscreenVaryings VertexMain(uint vertexIndex : SV_VertexID)
{
    const float2 uv = float2(float((vertexIndex << 1) & 2u), float(vertexIndex & 2u));
    FullscreenVaryings output;
    output.uv = uv;
    output.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

#endif  // BLUE_SHADERS_FULLSCREEN_SLANG_
