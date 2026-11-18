#ifndef BLUE_MATERIAL_MATERIAL_H_
#define BLUE_MATERIAL_MATERIAL_H_

#include <string>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

// Metallic-roughness PBR material description (the glTF 2.0 model), shared by
// the importers, the .bmat material files and the renderer. CPU data only:
// textures are references, decoded by asset::LoadTexture() when uploaded.

namespace blue::material {

enum class AlphaMode {
  kOpaque,  // Alpha is ignored.
  kMask,    // Fragments with alpha < alpha_cutoff are discarded.
};

// Where a texture's image comes from. Either an image file (`path`), or an
// image embedded in a model file (`path` = the model file, `embedded_index` =
// index into that file's embedded textures, aiScene::mTextures). Paths are
// absolute or relative to the working directory in memory; .bmat files store
// them relative to themselves.
struct TextureRef {
  std::string path;
  int embedded_index = -1;

  // Returns true if no texture is referenced.
  [[nodiscard]] bool empty() const { return path.empty(); }
  [[nodiscard]] bool embedded() const { return embedded_index >= 0; }

  friend bool operator==(const TextureRef&, const TextureRef&) = default;
};

// The inputs of one surface. Colours are linear RGB; colour textures are
// sRGB-encoded and data textures linear, as noted per slot. Every texture is
// multiplied by its factor (base_color, metallic/roughness, emissive *
// emissive_strength); an empty slot behaves like a white texture. Texture
// coordinates are scaled by `uv_scale` before sampling.
struct MaterialDesc {
  std::string name;
  glm::vec4 base_color{1.0f};  // linear RGBA factor; alpha used by kMask
  float metallic = 0.0f;       // [0, 1]
  float roughness = 0.5f;      // [0, 1], perceptual (alpha = roughness^2)
  glm::vec3 emissive{0.0f};    // linear
  float emissive_strength = 1.0f;
  float normal_scale = 1.0f;        // scales the normal map's xy
  float occlusion_strength = 1.0f;  // 0 = no occlusion, 1 = full
  glm::vec2 uv_scale{1.0f};
  AlphaMode alpha_mode = AlphaMode::kOpaque;
  float alpha_cutoff = 0.5f;
  bool double_sided = false;

  TextureRef base_color_texture;  // sRGB RGBA
  // Linear, glTF packing: G = roughness, B = metallic.
  TextureRef metallic_roughness_texture;
  // Separate greyscale maps (linear, R channel) as most texture libraries
  // ship them; used only when metallic_roughness_texture is empty. The
  // renderer packs them with asset::PackMetallicRoughness().
  TextureRef roughness_texture;
  TextureRef metallic_texture;
  // Linear tangent-space normal map, +Y up (glTF / OpenGL convention).
  TextureRef normal_texture;
  TextureRef occlusion_texture;  // linear, R channel
  TextureRef emissive_texture;   // sRGB RGB

  friend bool operator==(const MaterialDesc&, const MaterialDesc&) = default;
};

// Value ranges enforced by Sanitize().
inline constexpr float kMaxEmissiveStrength = 1000.0f;
inline constexpr float kMaxNormalScale = 10.0f;
inline constexpr float kMaxUvScale = 1000.0f;

// Returns `desc` with every number made finite and clamped to its range:
// base colour and metallic/roughness/occlusion/alpha cutoff to [0, 1],
// emissive components to [0, kMaxEmissiveStrength], emissive strength to [0,
// kMaxEmissiveStrength], normal scale to [0, kMaxNormalScale], uv scale
// components to [1e-4, kMaxUvScale]. Non-finite values become the field's
// default.
[[nodiscard]] MaterialDesc Sanitize(MaterialDesc desc);

// Returns true if the two materials look the same: every field but `name`
// is equal. The importers merge such duplicates.
[[nodiscard]] bool SameAppearance(const MaterialDesc& a, const MaterialDesc& b);

// Returns the GGX roughness that approximates a Blinn-Phong specular
// exponent (MTL "Ns"): sqrt(2 / (shininess + 2)), clamped to [0.05, 1].
// Shininess <= 0 (and non-finite input) gives 1.
[[nodiscard]] float RoughnessFromShininess(float shininess);

}  // namespace blue::material

#endif  // BLUE_MATERIAL_MATERIAL_H_
