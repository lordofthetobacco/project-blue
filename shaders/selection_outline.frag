#version 450

// Outline of the selection mask (render::OverlayPass): pixels outside the
// mask within kRadius pixels of it get the selection colour, at full
// opacity next to visible parts and dimmed next to hidden ones.

// R8: 0 = not selected, 0.5 = selected but hidden, 1 = selected, visible.
layout(set = 0, binding = 0) uniform sampler2D selectionMask;

// Must match OutlineConstants in src/render/passes/overlay_pass.cc.
layout(push_constant) uniform OutlineConstants {
    vec4 color;  // display space; alpha = opacity
} params;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

const int kRadius = 2;
const float kHiddenOpacity = 0.35;

void main()
{
    const ivec2 size = textureSize(selectionMask, 0);
    const ivec2 center = ivec2(inUv * vec2(size));
    if (texelFetch(selectionMask, center, 0).r > 0.25) {
        discard;  // Inside the selection: no outline.
    }
    float nearest = 0.0;
    for (int y = -kRadius; y <= kRadius; ++y) {
        for (int x = -kRadius; x <= kRadius; ++x) {
            if (x * x + y * y > kRadius * kRadius + 1) {
                continue;
            }
            const ivec2 texel = clamp(center + ivec2(x, y), ivec2(0), size - 1);
            nearest = max(nearest, texelFetch(selectionMask, texel, 0).r);
        }
    }
    if (nearest < 0.25) {
        discard;
    }
    const float opacity = nearest > 0.75 ? 1.0 : kHiddenOpacity;
    outColor = vec4(params.color.rgb, params.color.a * opacity);
}
