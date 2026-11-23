#ifndef BLUE_RENDER_D3D12_MATERIAL_SYSTEM_H_
#define BLUE_RENDER_D3D12_MATERIAL_SYSTEM_H_

#include <d3d12.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "material/material.h"
#include "render/d3d12/buffer.h"
#include "render/d3d12/descriptor_heaps.h"
#include "render/d3d12/image.h"
#include "render/d3d12/samplers.h"
#include "render/d3d12/texture_cache.h"
#include "render/draw_list.h"
#include "render/material_params.h"
#include "render/scene_view.h"
#include "render/texture_types.h"

namespace blue::render::d3d12 {

class D3d12Context;

// The backend's materials. Each material version is one persistent
// descriptor table of the shader-visible heap (the pass set's table of the
// scene root signatures, root_layout.h): descriptor 0 the CBV of its
// MaterialParams, descriptors 1-5 the SRVs of its base colour,
// metallic-roughness, normal, occlusion and emissive textures (TextureCache
// defaults for empty slots). The parameters live in one upload-heap buffer,
// kParamsStride bytes per table slot (the table's slot number is the
// parameter block's), written once per version. The sampler is
// Samplers::material() (sampler()).
//
// Updating a material gives it a fresh table (and parameter block) and
// retires the old one until no frame in flight can read it (BeginFrame()):
// a frame's tables never change while the GPU reads them, so editing a
// material every frame allocates nothing once warm. The same contract as the
// Vulkan MaterialSystem. Borrows `context` and `textures`, which must
// outlive it. Neither copyable nor movable. Not thread-safe.
class MaterialSystem {
 public:
  static constexpr uint64_t kParamsStride =
      D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;

  MaterialSystem(const D3d12Context& context, const TextureCache& textures,
                 const Samplers& samplers);
  ~MaterialSystem();

  MaterialSystem(const MaterialSystem&) = delete;
  MaterialSystem& operator=(const MaterialSystem&) = delete;

  // Creates an untextured material from `desc` (Sanitize()d): every slot
  // uses its default; desc's TextureRefs are ignored.
  [[nodiscard]] MaterialId Create(const material::MaterialDesc& desc);
  // Creates a material whose texture slots are `textures` (empty slots use
  // the defaults); desc's TextureRefs are ignored.
  [[nodiscard]] MaterialId Create(const material::MaterialDesc& desc,
                                  const MaterialTextures& textures);
  // Replaces material `id` by `desc` from the next recorded frame on; frames
  // in flight keep the old version. Keeps the texture slots. An unknown id
  // is fatal.
  void Update(MaterialId id, const material::MaterialDesc& desc);
  // Releases material `id` (its table is retired); draws naming it render
  // with kDefaultMaterial (Resolve()). kDefaultMaterial, unknown and
  // already released ids are ignored.
  void Release(MaterialId id);
  // Returns `id`, or kDefaultMaterial when `id` is unknown or released.
  [[nodiscard]] MaterialId Resolve(MaterialId id) const {
    const auto index = static_cast<size_t>(id);
    return index < materials_.size() && !materials_[index].released
               ? id
               : kDefaultMaterial;
  }

  // Starts recording frame number `frame_serial` (after its slot's fence was
  // reached).
  void BeginFrame(uint64_t frame_serial) { frame_serial_ = frame_serial; }

  // The accessors below take a valid id (< count()); not checked.
  [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE table(MaterialId id) const {
    return materials_[static_cast<size_t>(id)].table.gpu;
  }
  [[nodiscard]] MaterialVariant variant(MaterialId id) const {
    return materials_[static_cast<size_t>(id)].variant;
  }
  [[nodiscard]] const material::MaterialDesc& desc(MaterialId id) const {
    return materials_[static_cast<size_t>(id)].desc;
  }
  [[nodiscard]] size_t count() const { return materials_.size(); }
  // The sampler table every material's textures are read with.
  [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE sampler() const {
    return samplers_.material();
  }

 private:
  struct Material {
    material::MaterialDesc desc;
    std::optional<MaterialTextures> textures;
    MaterialVariant variant = MaterialVariant::kOpaque;
    Descriptor table;
    bool released = false;
  };

  // Adds a material and writes its first table.
  [[nodiscard]] MaterialId Add(const material::MaterialDesc& desc,
                               std::optional<MaterialTextures> textures);
  // Returns a fresh table with the parameters and textures of `material`.
  [[nodiscard]] Descriptor Write(const Material& material);

  const D3d12Context& context_;
  const TextureCache& textures_;
  const Samplers& samplers_;
  // kParamsStride bytes per table slot of the descriptor heaps.
  Buffer params_;
  std::vector<Material> materials_;
  // The frame being recorded (or last recorded), 0 before the first.
  uint64_t frame_serial_ = 0;
};

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_MATERIAL_SYSTEM_H_
