// Halves one mip of the environment's specular source cube into the next
// (render Environment of the Direct3D 12 backend, which has no blit): one
// bilinear tap at the centre of each 2 x 2 block of source texels, i.e.
// their average, what the Vulkan backend's linear blit produces. Program:
// env_downsample.

#include "fullscreen.slang"

// The source mip alone, every face.
BLUE_TEXTURE(Texture2DArray<float4>, source, 0, BLUE_INPUT_SET);

// Must match DownsampleConstants in src/render/d3d12/environment.cc.
struct DownsampleConstants {
    float2 texelSize;  // 1 / target size
    int face;
    int padding;
};
BLUE_PUSH_CONSTANTS(DownsampleConstants, push);

[shader("fragment")]
float4 FragmentMain(FullscreenVaryings input) : SV_Target
{
    const float2 uv = input.position.xy * push.texelSize;
    return source.SampleLevel(inputSampler, float3(uv, float(push.face)), 0.0);
}
