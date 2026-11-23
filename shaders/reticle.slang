// A collimated (reflex) sight (render ReticlePass): the dot is an image at
// infinity along the optic axis, so whether a pixel shows it depends only on
// the direction from the eye through that pixel, never on where on the lens
// the pixel lies. That alone gives true parallax behaviour: with the eye off
// the axis the dot slides across the lens yet stays on the same distant
// point, and once the axis direction falls outside the window there is no
// fragment to light. Program: reticle.

#include "common.slang"
#include "sanitize.slang"

// Must match ReticleConstants in src/render/vulkan/passes/reticle_pass.cc.
struct ReticleConstants {
    float4x4 lens;    // Unit disc (xy in [-1, 1], z = 0) to world: the lens window.
    float4 axis;      // xyz: optic axis, world, unit, pointing downrange.
                      // w: angular radius of the dot, radians.
    float4 dotColor;  // rgb: linear colour times intensity.
                      // w: smallest dot diameter on screen, pixels.
    float4 tint;      // rgb: faint light added over the whole window.
};
BLUE_PUSH_CONSTANTS(ReticleConstants, sight);

// Spans the lens window with a quad of two triangles; the fragment shader
// trims it to a disc. No vertex buffer.
static const float2 kCorners[6] = {
    float2(-1.0, -1.0), float2(1.0, -1.0), float2(1.0, 1.0),
    float2(-1.0, -1.0), float2(1.0, 1.0),  float2(-1.0, 1.0)};

struct Varyings {
    float4 clipPosition : SV_Position;
    float2 lensPosition : TEXCOORD0;  // In the unit disc.
    float3 worldPosition : WORLDPOSITION;
};

[shader("vertex")]
Varyings VertexMain(uint vertexIndex : SV_VertexID)
{
    const float2 corner = kCorners[vertexIndex];
    const float4 worldPosition = mul(sight.lens, float4(corner, 0.0, 1.0));
    Varyings output;
    output.lensPosition = corner;
    output.worldPosition = worldPosition.xyz;
    output.clipPosition = mul(frame.viewProj, worldPosition);
    return output;
}

[shader("fragment")]
float4 FragmentMain(Varyings input) : SV_Target
{
    const float lensRadius = length(input.lensPosition);
    const float rimWidth = max(fwidth(lensRadius), 1.0e-6);
    const float3 direction = normalize(input.worldPosition - frame.cameraPosition.xyz);
    // Angle one pixel spans, from how fast the direction changes on screen.
    const float pixelAngle = max(length(fwidth(direction)), 1.0e-7);

    // Antialiased rim of the window; zero outside the disc. No discard: the
    // pass is additive, so black adds nothing.
    const float window = 1.0 - smoothstep(1.0 - rimWidth, 1.0, lensRadius);

    // Angle to the axis; the chord form stays accurate for tiny angles where
    // acos(dot) loses all precision.
    const float angle = 2.0 * asin(min(0.5 * length(direction - sight.axis.xyz), 1.0));
    const float radius = max(sight.axis.w, 0.5 * sight.dotColor.w * pixelAngle);
    const float dotMask =
        1.0 - smoothstep(radius - 0.5 * pixelAngle, radius + 0.5 * pixelAngle, angle);

    // Additive: alpha is ignored by the blend.
    return float4(SanitizeRadiance((sight.dotColor.rgb * dotMask + sight.tint.rgb) * window), 0.0);
}
