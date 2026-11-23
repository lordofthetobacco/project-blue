#include "world/gpu_backend.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "material/material.h"
#include "prefab/prefab.h"
#include "render/renderer.h"
#include "render/scene_view.h"
#include "render/texture_types.h"

namespace blue::world {
namespace {

render::TextureFormat ToRenderFormat(prefab::TextureFormat format) {
  switch (format) {
    case prefab::TextureFormat::kBc7Srgb:
      return render::TextureFormat::kBc7Srgb;
    case prefab::TextureFormat::kBc7Unorm:
      return render::TextureFormat::kBc7Unorm;
    case prefab::TextureFormat::kBc5Unorm:
      return render::TextureFormat::kBc5Unorm;
    case prefab::TextureFormat::kRgba8Srgb:
      return render::TextureFormat::kRgba8Srgb;
    case prefab::TextureFormat::kRgba8Unorm:
      return render::TextureFormat::kRgba8Unorm;
  }
  return render::TextureFormat::kRgba8Unorm;
}

// Returns the uploaded id of prefab texture `index` (-1 = none).
std::optional<render::TextureId> Slot(
    int index, std::span<const render::TextureId> uploaded) {
  if (index < 0 || static_cast<size_t>(index) >= uploaded.size()) {
    return std::nullopt;
  }
  return uploaded[static_cast<size_t>(index)];
}

}  // namespace

render::MeshId RendererBackend::UploadMesh(const prefab::Mesh& mesh) {
  // Straight from the prefab's bulk data (a mapped file when loaded): no
  // intermediate heap copy.
  return renderer_.UploadMesh(mesh.vertices, mesh.indices);
}

std::vector<render::TextureId> RendererBackend::UploadTextures(
    std::span<const prefab::Texture> textures) {
  if (textures.empty()) {
    return {};
  }
  // Mip views per texture, kept alive for the call.
  std::vector<std::vector<std::span<const std::byte>>> mips(textures.size());
  std::vector<render::TextureUpload> uploads;
  uploads.reserve(textures.size());
  for (size_t i = 0; i < textures.size(); ++i) {
    const prefab::Texture& texture = textures[i];
    for (const prefab::TextureMip& mip : texture.mips) {
      mips[i].push_back(mip.data);
    }
    uploads.push_back({.format = ToRenderFormat(texture.format),
                       .width = static_cast<uint32_t>(texture.width),
                       .height = static_cast<uint32_t>(texture.height),
                       .mips = mips[i],
                       .name = texture.name});
  }
  return renderer_.UploadTextures(uploads);
}

render::MaterialId RendererBackend::CreateMaterial(
    const material::MaterialDesc& desc,
    const render::MaterialTextures& textures) {
  return renderer_.CreateMaterial(desc, textures);
}

void RendererBackend::UpdateMaterial(render::MaterialId id,
                                     const material::MaterialDesc& desc) {
  renderer_.UpdateMaterial(id, desc);
}

void RendererBackend::ReleaseMesh(render::MeshId id) {
  renderer_.ReleaseMesh(id);
}

void RendererBackend::ReleaseTextures(std::span<const render::TextureId> ids) {
  renderer_.ReleaseTextures(ids);
}

void RendererBackend::FlushReleases() { renderer_.FlushReleases(); }

void RendererBackend::ReleaseMaterial(render::MaterialId id) {
  renderer_.ReleaseMaterial(id);
}

material::MaterialDesc ToMaterialDesc(
    const prefab::Material& material,
    std::span<const prefab::Texture> textures) {
  material::MaterialDesc desc;
  desc.name = material.name;
  desc.base_color = material.base_color;
  desc.metallic = material.metallic;
  desc.roughness = material.roughness;
  desc.emissive = material.emissive;
  desc.emissive_strength = material.emissive_strength;
  desc.normal_scale = material.normal_scale;
  desc.occlusion_strength = material.occlusion_strength;
  desc.uv_scale = material.uv_scale;
  desc.alpha_mode = material.alpha_mode;
  desc.alpha_cutoff = material.alpha_cutoff;
  desc.double_sided = material.double_sided;

  const auto uv_set = [&material](prefab::TextureSlot slot) {
    return static_cast<uint8_t>(material.uv_sets[static_cast<size_t>(slot)]);
  };
  const auto set = [&desc](material::TextureSlot slot, uint8_t value) {
    desc.uv_set[static_cast<size_t>(slot)] = value;
  };
  set(material::TextureSlot::kBaseColor,
      uv_set(prefab::TextureSlot::kBaseColor));
  set(material::TextureSlot::kMetallicRoughness,
      uv_set(prefab::TextureSlot::kOrm));
  set(material::TextureSlot::kOcclusion, uv_set(prefab::TextureSlot::kOrm));
  set(material::TextureSlot::kNormal, uv_set(prefab::TextureSlot::kNormal));
  set(material::TextureSlot::kEmissive, uv_set(prefab::TextureSlot::kEmissive));

  desc.occlusion_in_metallic_roughness =
      material.texture(prefab::TextureSlot::kOrm) >= 0;
  const int normal = material.texture(prefab::TextureSlot::kNormal);
  if (normal >= 0 && static_cast<size_t>(normal) < textures.size() &&
      textures[static_cast<size_t>(normal)].format ==
          prefab::TextureFormat::kBc5Unorm) {
    desc.normal_encoding = material::NormalEncoding::kRg;
  }
  return desc;
}

render::MaterialTextures ToMaterialTextures(
    const prefab::Material& material,
    std::span<const render::TextureId> uploaded) {
  render::MaterialTextures textures;
  textures.base_color =
      Slot(material.texture(prefab::TextureSlot::kBaseColor), uploaded);
  textures.metallic_roughness =
      Slot(material.texture(prefab::TextureSlot::kOrm), uploaded);
  textures.occlusion = textures.metallic_roughness;
  textures.normal =
      Slot(material.texture(prefab::TextureSlot::kNormal), uploaded);
  textures.emissive =
      Slot(material.texture(prefab::TextureSlot::kEmissive), uploaded);
  return textures;
}

}  // namespace blue::world
