#ifndef BLUE_SHADERS_BINDINGS_H_
#define BLUE_SHADERS_BINDINGS_H_

// The argument-table indices shared by the Metal shaders and the renderer.
// The renderer binds everything through one MTL4ArgumentTable that both the
// vertex and the fragment stage see, so an index means the same in both.
// Included by MSL (`#include "bindings.h"`) and by C++
// (`#include "shaders/bindings.h"`); keep it to what both languages accept.
// The enums are unscoped so that MSL attributes can name them:
// `[[buffer(kBufferFrame)]]`.

#ifndef __METAL_VERSION__
namespace blue::render {
#endif

// Vertex and fragment buffer indices. A slot means the same thing in both
// stages.
enum BufferIndex {
  kBufferVertices = 0,  // Interleaved vertices (stage_in).
  kBufferFrame = 1,     // FrameUniforms, one buffer per frame in flight.
  kBufferDraw = 2,      // Per-draw constants (model matrix), set as bytes.
  kBufferPass = 3,      // Per-pass constants, at most kMaxPassConstantBytes.
  kBufferMaterial = 4,  // MaterialParams, set as bytes per material.
  // ScopeLensUniforms, one buffer per frame in flight (too big for
  // kBufferPass).
  kBufferScopeLens = 5,
};

// Slots of the renderer's argument table (render::MetalContext): one more
// than the highest index of each kind below.
enum { kBufferBindCount = 6, kTextureBindCount = 15, kSamplerBindCount = 5 };

// The size limit of a kBufferPass block.
enum { kMaxPassConstantBytes = 128 };

// Fragment texture indices.
enum TextureIndex {
  kTextureShadowMap = 0,         // depth2d_array, the sun's cascades.
  kTextureEnvironment = 1,       // texturecube, prefiltered specular.
  kTextureBrdfLut = 2,           // texture2d, split-sum lookup table.
  kTextureAmbientOcclusion = 3,  // texture2d, half resolution.
  // Material slots, in material::TextureSlot order.
  kTextureBaseColor = 4,
  kTextureMetallicRoughness = 5,
  kTextureNormal = 6,
  kTextureOcclusion = 7,
  kTextureEmissive = 8,
  kTextureSky = 9,  // texturecube, the skybox.
  // Inputs of a full-screen pass: kTextureInput0 + n, n < kMaxInputTextures.
  kTextureInput0 = 10,
  // texture2d, a rifle scope's picture (kTextureInput0 + kMaxInputTextures).
  kTextureScopePicture = 14,
};

enum { kMaterialTextureCount = 5, kMaxInputTextures = 4 };

// Fragment sampler indices.
enum SamplerIndex {
  kSamplerShadow = 0,        // Comparison sampler of the shadow map.
  kSamplerMaterial = 1,      // Trilinear, repeating, anisotropic.
  kSamplerLinearClamp = 2,   // Bilinear, clamped to the edge.
  kSamplerNearestClamp = 3,  // Nearest, clamped to the edge.
  kSamplerInput = 4,         // The sampler of a full-screen pass's inputs.
};

// Function constants: what a pipeline variant is specialized with.
enum FunctionConstantIndex {
  kConstantAlphaMask = 0,         // mesh.frag.metal: alpha-tested material.
  kConstantDoubleSided = 1,       // mesh.frag.metal: flip back-face normals.
  kConstantSelectionVisible = 2,  // selection_mask.frag.metal: depth tested.
};

// Vertex attribute indices of asset::Vertex (and of LineVertex, which has a
// position and kAttributeLineColor only).
enum VertexAttributeIndex {
  kAttributePosition = 0,
  kAttributeNormal = 1,
  kAttributeUv0 = 2,
  kAttributeTangent = 3,
  kAttributeUv1 = 4,
  kAttributeLineColor = 1,
};

#ifndef __METAL_VERSION__
}  // namespace blue::render
#endif

#endif  // BLUE_SHADERS_BINDINGS_H_
