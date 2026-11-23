// Test-only (tests/render/d3d12_render_test.cc): copies texels of one mip of
// the input texture into a rectangle of the target, unfiltered, so the test
// can compare uploaded textures with their source bytes exactly. Program:
// test_texture_grid.

#include "fullscreen.slang"

BLUE_TEXTURE(Texture2D<float4>, source, 0, BLUE_INPUT_SET);

// Must match GridConstants in tests/render/d3d12_render_test.cc.
struct GridConstants {
    int2 origin;  // Target pixel of texel (0, 0).
    int mip;
    int padding;
};
BLUE_PUSH_CONSTANTS(GridConstants, grid);

[shader("fragment")]
float4 FragmentMain(FullscreenVaryings input) : SV_Target
{
    const int2 texel = int2(input.position.xy) - grid.origin;
    return source.Load(int3(texel, grid.mip));
}
