#ifndef BLUE_RENDER_MATERIAL_SYSTEM_H_
#define BLUE_RENDER_MATERIAL_SYSTEM_H_

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <glm/ext/vector_uint4.hpp>
#include <glm/vec4.hpp>

#include "material/material.h"
#include "render/buffer.h"
#include "render/samplers.h"
#include "render/scene_view.h"
#include "render/texture_cache.h"
#include "render/vulkan_context.h"

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

// Must match MaterialParams in shaders/mesh.frag (std140).
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

// MaterialParams::options.y bits; must match shaders/material.glsl.
inline constexpr uint32_t kMaterialFlagNormalRg = 1;
inline constexpr uint32_t kMaterialFlagOcclusionInMetallicRoughness = 2;

// Returns the shader parameters of `desc`.
[[nodiscard]] MaterialParams MakeMaterialParams(
    const material::MaterialDesc& desc);

// The renderer's materials: per material a descriptor set (set 1 of the mesh
// pipelines: binding 0 MaterialParams, bindings 1-5 base colour,
// metallic-roughness, normal, occlusion and emissive textures) and its
// parameter buffer. Empty texture slots use TextureCache defaults.
//
// Updating a material gives it a fresh set + buffer and retires the old pair
// until no frame in flight can still read it (see BeginFrame()); retired
// pairs are reused, so editing a material every frame allocates nothing once
// warm. Sets come from fixed-size pools, a new pool is added when one fills.
//
// Borrows `context`, `textures` and `samplers`, which must outlive it.
// Neither copyable nor movable. Not thread-safe.
class MaterialSystem {
 public:
  MaterialSystem(const VulkanContext& context, TextureCache& textures,
                 const Samplers& samplers);
  ~MaterialSystem();

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

  // Releases material `id`: its descriptor set and parameter buffer are
  // retired until no frame in flight can use them (then reused), and the id
  // is never handed out again. Draws naming it render with kDefaultMaterial
  // (see Resolve()). kDefaultMaterial, unknown and already released ids are
  // ignored. Call between frames.
  void Release(MaterialId id);
  // Returns `id`, or kDefaultMaterial when `id` is unknown or released.
  [[nodiscard]] MaterialId Resolve(MaterialId id) const {
    const auto index = static_cast<size_t>(id);
    return index < materials_.size() && !materials_[index].released
               ? id
               : kDefaultMaterial;
  }

  // Starts recording frame number `frame_serial` (1, 2, ...; one more per
  // recorded frame). Call after that frame's fence was waited on, which
  // guarantees frames up to frame_serial - kFramesInFlight have finished:
  // their retired sets and buffers become reusable.
  void BeginFrame(uint64_t frame_serial);

  [[nodiscard]] VkDescriptorSetLayout set_layout() const { return layout_; }
  // The accessors below take a valid id (< count()); not checked. A
  // released id still answers (its last state) but must not be drawn:
  // pass ids through Resolve() first.
  [[nodiscard]] VkDescriptorSet descriptor_set(MaterialId id) const {
    return materials_[static_cast<size_t>(id)].binding.set;
  }
  [[nodiscard]] MaterialVariant variant(MaterialId id) const {
    return materials_[static_cast<size_t>(id)].variant;
  }
  [[nodiscard]] const material::MaterialDesc& desc(MaterialId id) const {
    return materials_[static_cast<size_t>(id)].desc;
  }
  [[nodiscard]] size_t count() const { return materials_.size(); }

 private:
  // The GPU-side state of one material version.
  struct Binding {
    VkDescriptorSet set = VK_NULL_HANDLE;
    Buffer params;
  };
  struct Material {
    material::MaterialDesc desc;
    // Set for materials created from uploaded textures.
    std::optional<MaterialTextures> textures;
    MaterialVariant variant = MaterialVariant::kOpaque;
    Binding binding;
    bool released = false;
  };
  struct Retired {
    Binding binding;
    uint64_t last_frame = 0;  // The last frame that may read it.
  };

  // Returns an unused binding: a recycled one or a new one.
  [[nodiscard]] Binding AcquireBinding();
  [[nodiscard]] VkDescriptorSet AllocateSet();
  // Returns the image views of `material`'s texture slots (defaults for
  // empty slots).
  [[nodiscard]] std::array<VkImageView, material::kTextureSlotCount>
  TextureViews(const Material& material) const;
  // Writes the parameters and textures of `material` into `binding`.
  void Write(const Binding& binding, const Material& material);

  const VulkanContext& context_;
  TextureCache& textures_;
  const Samplers& samplers_;
  VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
  std::vector<VkDescriptorPool> pools_;
  uint32_t sets_in_last_pool_ = 0;
  std::vector<Material> materials_;
  std::vector<Binding> free_;
  std::vector<Retired> retired_;
  // The frame being recorded (or last recorded), 0 before the first.
  uint64_t frame_serial_ = 0;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_MATERIAL_SYSTEM_H_
