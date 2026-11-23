#ifndef BLUE_WORLD_GPU_BACKEND_H_
#define BLUE_WORLD_GPU_BACKEND_H_

#include <cstddef>
#include <span>
#include <vector>

#include "material/material.h"
#include "prefab/prefab.h"
#include "render/scene_view.h"
#include "render/texture_types.h"

namespace blue::render {
class Renderer;
}  // namespace blue::render

namespace blue::world {

// Where PrefabCache puts GPU resources: the renderer in the game and the
// editor (RendererBackend), a recording fake in tests. Called only while
// prefabs load or materials change, never per frame, so the virtual calls
// cost nothing measurable.
class GpuBackend {
 public:
  virtual ~GpuBackend() = default;

  // Uploads the vertices and indices of `mesh` (submeshes are drawn through
  // render::SubmeshDraw ranges, see PrefabGpu).
  virtual render::MeshId UploadMesh(const prefab::Mesh& mesh) = 0;
  // Uploads the textures in one batch and returns their ids in order.
  virtual std::vector<render::TextureId> UploadTextures(
      std::span<const prefab::Texture> textures) = 0;
  virtual render::MaterialId CreateMaterial(
      const material::MaterialDesc& desc,
      const render::MaterialTextures& textures) = 0;
  // Changes the parameters of `id` from the next frame on; textures stay.
  virtual void UpdateMaterial(render::MaterialId id,
                              const material::MaterialDesc& desc) = 0;

  // Release GPU resources once no frame in flight uses them. The ids must
  // not be drawn afterwards (the renderer skips stale ones). Materials are
  // released before the textures they sample.
  virtual void ReleaseMesh(render::MeshId id) = 0;
  virtual void ReleaseTextures(std::span<const render::TextureId> ids) = 0;
  virtual void ReleaseMaterial(render::MaterialId id) = 0;
  // Destroys released resources now instead of after the frames in flight
  // (waits for them), so their memory is free before a large upload. Call
  // between frames; not per frame. The default does nothing.
  virtual void FlushReleases() {}
};

// The GpuBackend of a real renderer. Borrows `renderer`, which must outlive
// it.
class RendererBackend final : public GpuBackend {
 public:
  explicit RendererBackend(render::Renderer& renderer) : renderer_(renderer) {}
  RendererBackend(const RendererBackend&) = delete;
  RendererBackend& operator=(const RendererBackend&) = delete;

  render::MeshId UploadMesh(const prefab::Mesh& mesh) override;
  std::vector<render::TextureId> UploadTextures(
      std::span<const prefab::Texture> textures) override;
  render::MaterialId CreateMaterial(
      const material::MaterialDesc& desc,
      const render::MaterialTextures& textures) override;
  void UpdateMaterial(render::MaterialId id,
                      const material::MaterialDesc& desc) override;
  void ReleaseMesh(render::MeshId id) override;
  void ReleaseTextures(std::span<const render::TextureId> ids) override;
  void ReleaseMaterial(render::MaterialId id) override;
  void FlushReleases() override;

 private:
  render::Renderer& renderer_;
};

// Returns the renderer description of `material`: its scalars, per-slot UV
// sets, BC5 normal encoding (kRg when the normal texture is kBc5Unorm) and
// packed ORM (occlusion_in_metallic_roughness when it has an ORM texture).
// Texture refs stay empty: textures are passed as render::MaterialTextures.
[[nodiscard]] material::MaterialDesc ToMaterialDesc(
    const prefab::Material& material,
    std::span<const prefab::Texture> textures);

// Returns the uploaded textures of `material`'s slots; `uploaded` holds one
// id per prefab texture. The ORM texture fills both metallic_roughness and
// occlusion.
[[nodiscard]] render::MaterialTextures ToMaterialTextures(
    const prefab::Material& material,
    std::span<const render::TextureId> uploaded);

}  // namespace blue::world

#endif  // BLUE_WORLD_GPU_BACKEND_H_
