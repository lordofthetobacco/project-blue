#version 450

// One upsample step of the bloom chain (render::BloomPass): a 3x3 tent
// filter (weights 1 2 1 / 2 4 2 / 1 2 1, divided by 16) of the smaller mip,
// added (blend ONE, ONE) onto the next larger one, which still holds its
// downsampled image. Mip 0 ends up with the sum of every level.

layout(set = 0, binding = 0) uniform sampler2D source;  // bilinear, clamped

// Must match UpsampleConstants in src/render/passes/bloom_pass.cc.
layout(push_constant) uniform UpsampleConstants {
    vec2 sourceTexel;  // 1 / source size: the tent's tap spacing
} params;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

vec3 Tap(float x, float y)
{
    return texture(source, inUv + vec2(x, y) * params.sourceTexel).rgb;
}

void main()
{
    vec3 sum = Tap(0.0, 0.0) * 4.0;
    sum += (Tap(-1.0, 0.0) + Tap(1.0, 0.0) + Tap(0.0, -1.0) + Tap(0.0, 1.0)) * 2.0;
    sum += Tap(-1.0, -1.0) + Tap(1.0, -1.0) + Tap(-1.0, 1.0) + Tap(1.0, 1.0);
    outColor = vec4(sum / 16.0, 1.0);
}
