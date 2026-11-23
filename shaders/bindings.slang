// How shader resources are declared so one source binds the same way on
// Vulkan and Direct3D 12 (see README.md). Included with
// #include "bindings.slang"; safe to include more than once.
#ifndef BLUE_SHADERS_BINDINGS_SLANG_
#define BLUE_SHADERS_BINDINGS_SLANG_

#include "gpu_bindings.h"

#define BLUE_PASTE_(a, b) a##b
#define BLUE_PASTE(a, b) BLUE_PASTE_(a, b)

// A uniform block `type` named `name` at (set_, binding_): register
// b<binding_>, space<set_>.
#define BLUE_UNIFORMS(type, name, binding_, set_)                    \
    [[vk::binding(binding_, set_)]] ConstantBuffer<type> name        \
        : register(BLUE_PASTE(b, binding_), BLUE_PASTE(space, set_))

// A texture (`type`: Texture2D<float4>, TextureCube<float4>,
// Texture2DArray<float>, ...) at (set_, binding_): register t<binding_>.
// Sampled with one of the set's samplers: tex.Sample(sampler, uv).
#define BLUE_TEXTURE(type, name, binding_, set_)                     \
    [[vk::binding(binding_, set_)]] type name                        \
        : register(BLUE_PASTE(t, binding_), BLUE_PASTE(space, set_))

// A sampler (`type`: SamplerState or SamplerComparisonState) at
// (set_, binding_): register s<binding_>. Shared by the textures of its set
// (Direct3D 12 has no combined texture samplers).
#define BLUE_SAMPLER(type, name, binding_, set_)                     \
    [[vk::binding(binding_, set_)]] type name                        \
        : register(BLUE_PASTE(s, binding_), BLUE_PASTE(space, set_))

// The push constant block of a program (at most
// BLUE_MAX_PUSH_CONSTANT_BYTES): root constants on Direct3D 12.
#define BLUE_PUSH_CONSTANTS(type, name)                 \
    [[vk::push_constant]] ConstantBuffer<type> name     \
        : register(b0, BLUE_PASTE(space, BLUE_PUSH_CONSTANT_SPACE))

#endif  // BLUE_SHADERS_BINDINGS_SLANG_
