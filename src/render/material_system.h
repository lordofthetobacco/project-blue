#ifndef BLUE_RENDER_MATERIAL_SYSTEM_H_
#define BLUE_RENDER_MATERIAL_SYSTEM_H_

#include "render/metal_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <glm/ext/vector_uint4.hpp>
#include <glm/vec4.hpp>

#include "material/material.h"
#include "render/metal_context.h"
#include "render/recording.h"
#include "render/samplers.h"
#include "render/scene_view.h"
#include "render/texture_cache.h"
#include "shaders/bindings.h"

namespace blue::render {

// The pipeline state a material needs, one mesh pipeline each.
enum class MaterialVariant : uint32_t {
  kOpaque = 0,
  kMasked = 1,             // Alpha-tested: fragments below the cutoff go.
  kDoubleSided = 2,        // No culling; back faces get a flipped normal.
  kMaskedDoubleSided = 3,  // Both.
};
inline constexpr size_t kMaterialVariantCount = 4;

[[nodiscard]] constexpr bool IsMasked(MaterialVariant variant) {
  return variant == MaterialVariant::kMasked ||
         variant == MaterialVariant::kMaskedDoubleSided;
}
[[nodiscard]] constexpr bool IsDoubleSided(MaterialVariant variant) {
  return variant == MaterialVariant::kDoubleSided ||
         variant == MaterialVariant::kMaskedDoubleSided;
}

// Returns the variant `desc` is drawn with.
[[nodiscard]] MaterialVariant VariantOf(const material::MaterialDesc& desc);

// Must match MaterialParams in shaders/material.h (float4 / uint4 members,
// so the C++ and MSL layouts agree without padding rules).
struct MaterialParams {
  glm::vec4 base_color;  // linear RGBA factor
  glm::vec4 emissive;    // rgb: emissive * emissive_strength
  // x: metallic, y: roughness, z: normal scale, w: occlusion strength.
  glm::vec4 surface;
  glm::vec4 uv_alpha;  // xy: uv scale, z: alpha cutoff
  // x: bit i set = texture slot i (material::TextureSlot) samples uv1;
  // y: kMaterialFlag* bits.
  glm::uvec4 options;
};

// MaterialParams::options.y bits; must match shaders/material.h.
inline constexpr uint32_t kMaterialFlagNormalRg = 1;
inline constexpr uint32_t kMaterialFlagOcclusionInMetallicRoughness = 2;

// Returns the shader parameters of `desc`.
[[nodiscard]] MaterialParams MakeMaterialParams(
    const material::MaterialDesc& desc);

// The renderer's materials. Per material it keeps what Bind() hands the
// fragment stage: its MaterialParams (constants at kBufferMaterial) and the
// textures of its five slots (kTextureBaseColor onwards, in
// material::TextureSlot order). Empty texture slots use TextureCache
// defaults.
//
// Nothing here is a GPU allocation: Bind() copies the parameters into the
// recording's transient constants, so Update() and Release() take effect
// for frames recorded afterwards without disturbing frames in flight.
// Textures a material drops (Release()) are owned by the TextureCache; the
// renderer's retire list keeps released Images alive and resident for
// kFramesInFlight frames, which is what frames in flight rely on (a Metal 4
// command buffer retains nothing).
//
// Borrows `textures` and `samplers`, which must outlive it. Neither
// copyable nor movable. Not thread-safe.
class MaterialSystem {
 public:
  // `context` is unused under Metal (no GPU objects are created here); it
  // keeps the construction of every render object alike.
  MaterialSystem(const MetalContext& context, const TextureCache& textures,
                 const Samplers& samplers);

  MaterialSystem(const MaterialSystem&) = delete;
  MaterialSystem& operator=(const MaterialSystem&) = delete;

  // Creates an untextured material from `desc` (Sanitize()d): every slot
  // uses its default; desc's TextureRefs are ignored.
  [[nodiscard]] MaterialId Create(const material::MaterialDesc& desc);
  // Creates a material whose texture slots are `textures` (uploaded with
  // TextureCache::Upload(); empty slots use the defaults); desc's
  // TextureRefs are ignored. For cooked ORM textures set
  // desc.occlusion_in_metallic_roughness and leave `occlusion` empty.
  [[nodiscard]] MaterialId Create(const material::MaterialDesc& desc,
                                  const MaterialTextures& textures);
  // Replaces material `id` by `desc`. Takes effect in the next recorded
  // frame; frames in flight keep the old version. Call between frames. The
  // material keeps its texture slots (parameters only). An unknown id is
  // fatal.
  void Update(MaterialId id, const material::MaterialDesc& desc);

  // Releases material `id`: it drops its textures and the id is never
  // handed out again. Draws naming it render with kDefaultMaterial (see
  // Resolve()). kDefaultMaterial, unknown and already released ids are
  // ignored. Call between frames.
  void Release(MaterialId id);
  // Returns `id`, or kDefaultMaterial when `id` is unknown or released.
  [[nodiscard]] MaterialId Resolve(MaterialId id) const {
    const auto index = static_cast<size_t>(id);
    return index < materials_.size() && !materials_[index].released
               ? id
               : kDefaultMaterial;
  }

  // Starts recording frame number `frame_serial` (1, 2, ...). Nothing is
  // retired per frame under Metal (see the class comment), so this does
  // nothing; it stays so that the renderer's frame protocol is unchanged.
  void BeginFrame(uint64_t /*frame_serial*/) {}

  // Binds the parameters, the five slot textures and the material sampler
  // (kSamplerMaterial) of material Resolve(`id`) in the argument table of
  // `rec`. kDefaultMaterial must have been created.
  void Bind(const Recording& rec, MaterialId id) const;

  // The accessors below take a valid id (< count()); not checked. A
  // released id still answers (its last state) but must not be drawn:
  // pass ids through Resolve() first.
  [[nodiscard]] MaterialVariant variant(MaterialId id) const {
    return materials_[static_cast<size_t>(id)].variant;
  }
  [[nodiscard]] const material::MaterialDesc& desc(MaterialId id) const {
    return materials_[static_cast<size_t>(id)].desc;
  }
  [[nodiscard]] size_t count() const { return materials_.size(); }

 private:
  // What Bind() sets for one material.
  struct GpuState {
    MaterialParams params{};
    std::array<id<MTLTexture>, kMaterialTextureCount> textures{};
  };
  struct Material {
    material::MaterialDesc desc;
    // Set for materials created from uploaded textures.
    std::optional<MaterialTextures> textures;
    MaterialVariant variant = MaterialVariant::kOpaque;
    GpuState gpu;
    bool released = false;
  };

  // Appends a material from `desc` and `textures`, returns its id.
  [[nodiscard]] MaterialId Add(const material::MaterialDesc& desc,
                               std::optional<MaterialTextures> textures);
  // Returns the textures of `material`'s slots (defaults for empty slots).
  [[nodiscard]] std::array<id<MTLTexture>, kMaterialTextureCount>
  SlotTextures(const Material& material) const;

  const TextureCache& textures_;
  const Samplers& samplers_;
  std::vector<Material> materials_;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_MATERIAL_SYSTEM_H_
