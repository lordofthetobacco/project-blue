#include "asset/internal/material_import.h"

#include <assimp/GltfMaterial.h>
#include <assimp/material.h>

#include <cstddef>
#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/log.h"
#include "core/path.h"
#include "material/material_file.h"
#include "platform/file_system.h"

namespace blue::asset::internal {
namespace {

using material::MaterialDesc;
using material::TextureRef;

// assimp's glTF importer stores the occlusion strength under
// "$tex.file.strength" while GltfMaterial.h documents "$tex.strength"
// (AI_MATKEY_GLTF_TEXTURE_STRENGTH); both are read.
constexpr const char* kImporterStrengthKey = "$tex.file.strength";

// Returns the texture of `type` (slot 0) of `material`, or an empty ref.
TextureRef FindTexture(const aiScene& scene, const aiMaterial& material,
                       aiTextureType type, const std::string& model_path,
                       std::set<std::string>& missing_files) {
  aiString path;
  unsigned uv_channel = 0;
  if (material.GetTexture(type, 0, &path, nullptr, &uv_channel) !=
      aiReturn_SUCCESS) {
    return {};
  }
  if (uv_channel != 0) {
    core::log::Warn(
        "{}: material '{}': texture '{}' uses UV channel {}; only "
        "channel 0 is supported, dropping it",
        model_path, material.GetName().C_Str(), path.C_Str(), uv_channel);
    return {};
  }
  const auto [embedded, index] = scene.GetEmbeddedTextureAndIndex(path.C_Str());
  if (embedded != nullptr && index >= 0) {
    return {.path = model_path, .embedded_index = index};
  }
  const std::string file =
      core::JoinPath(core::ParentPath(model_path), path.C_Str());
  if (!platform::IsRegularFile(file)) {
    if (missing_files.insert(file).second) {
      core::log::Warn("{}: texture '{}' not found", model_path, file);
    }
    return {};
  }
  return {.path = file};
}

// Returns the first non-empty texture of `types`.
TextureRef FindFirstTexture(const aiScene& scene, const aiMaterial& material,
                            std::initializer_list<aiTextureType> types,
                            const std::string& model_path,
                            std::set<std::string>& missing_files) {
  for (const aiTextureType type : types) {
    TextureRef ref =
        FindTexture(scene, material, type, model_path, missing_files);
    if (!ref.empty()) {
      return ref;
    }
  }
  return {};
}

// Reads the colour factors of `material` into `desc`.
void ReadFactors(const aiMaterial& material, MaterialDesc& desc) {
  aiColor4D base;
  aiColor3D diffuse;
  if (material.Get(AI_MATKEY_BASE_COLOR, base) == aiReturn_SUCCESS) {
    desc.base_color = {base.r, base.g, base.b, base.a};
  } else if (material.Get(AI_MATKEY_COLOR_DIFFUSE, diffuse) ==
             aiReturn_SUCCESS) {
    float opacity = 1.0f;
    if (material.Get(AI_MATKEY_OPACITY, opacity) != aiReturn_SUCCESS) {
      opacity = 1.0f;
    }
    desc.base_color = {diffuse.r, diffuse.g, diffuse.b, opacity};
  }

  if (material.Get(AI_MATKEY_METALLIC_FACTOR, desc.metallic) !=
      aiReturn_SUCCESS) {
    desc.metallic = 0.0f;
  }
  float shininess = 0.0f;
  if (material.Get(AI_MATKEY_ROUGHNESS_FACTOR, desc.roughness) !=
      aiReturn_SUCCESS) {
    desc.roughness =
        material.Get(AI_MATKEY_SHININESS, shininess) == aiReturn_SUCCESS &&
                shininess > 0.0f
            ? material::RoughnessFromShininess(shininess)
            : MaterialDesc{}.roughness;
  }

  aiColor3D emissive;
  if (material.Get(AI_MATKEY_COLOR_EMISSIVE, emissive) == aiReturn_SUCCESS) {
    desc.emissive = {emissive.r, emissive.g, emissive.b};
  }
  if (material.Get(AI_MATKEY_EMISSIVE_INTENSITY, desc.emissive_strength) !=
      aiReturn_SUCCESS) {
    desc.emissive_strength = 1.0f;
  }

  aiString alpha_mode;
  if (material.Get(AI_MATKEY_GLTF_ALPHAMODE, alpha_mode) == aiReturn_SUCCESS &&
      std::string_view(alpha_mode.C_Str()) == "MASK") {
    desc.alpha_mode = material::AlphaMode::kMask;
    if (material.Get(AI_MATKEY_GLTF_ALPHACUTOFF, desc.alpha_cutoff) !=
        aiReturn_SUCCESS) {
      desc.alpha_cutoff = 0.5f;
    }
  }
  int two_sided = 0;
  desc.double_sided =
      material.Get(AI_MATKEY_TWOSIDED, two_sided) == aiReturn_SUCCESS &&
      two_sided != 0;
}

}  // namespace

MaterialDesc DefaultMaterial() {
  return {.name = "default", .base_color = {0.8f, 0.8f, 0.8f, 1.0f}};
}

MaterialDesc ConvertMaterial(const aiScene& scene, const aiMaterial& material,
                             const std::string& model_path,
                             std::set<std::string>& missing_files) {
  const aiString name = material.GetName();
  if (name == aiString(AI_DEFAULT_MATERIAL_NAME)) {
    return DefaultMaterial();
  }
  MaterialDesc desc{.name = name.C_Str()};
  ReadFactors(material, desc);

  const auto find = [&](std::initializer_list<aiTextureType> types) {
    return FindFirstTexture(scene, material, types, model_path, missing_files);
  };
  desc.base_color_texture =
      find({aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE});
  desc.metallic_roughness_texture =
      find({aiTextureType_GLTF_METALLIC_ROUGHNESS});
  desc.normal_texture = find({aiTextureType_NORMALS, aiTextureType_HEIGHT});
  desc.occlusion_texture = find({aiTextureType_LIGHTMAP});
  desc.emissive_texture = find({aiTextureType_EMISSIVE});

  if (material.Get(AI_MATKEY_GLTF_TEXTURE_SCALE(aiTextureType_NORMALS, 0),
                   desc.normal_scale) != aiReturn_SUCCESS) {
    desc.normal_scale = 1.0f;
  }
  if (material.Get(kImporterStrengthKey, aiTextureType_LIGHTMAP, 0,
                   desc.occlusion_strength) != aiReturn_SUCCESS &&
      material.Get(AI_MATKEY_GLTF_TEXTURE_STRENGTH(aiTextureType_LIGHTMAP, 0),
                   desc.occlusion_strength) != aiReturn_SUCCESS) {
    desc.occlusion_strength = 1.0f;
  }
  return material::Sanitize(std::move(desc));
}

MaterialTable::MaterialTable(const aiScene& scene, std::string model_path,
                             bool use_overrides)
    : scene_(scene),
      model_path_(std::move(model_path)),
      use_overrides_(use_overrides),
      resolved_(scene.mNumMaterials, -1) {}

int MaterialTable::Resolve(unsigned index) {
  if (index >= resolved_.size()) {
    if (default_index_ < 0) {
      default_index_ = Add(DefaultMaterial());
    }
    return default_index_;
  }
  int& resolved = resolved_[index];
  if (resolved >= 0) {
    return resolved;
  }
  const aiMaterial& source = *scene_.mMaterials[index];
  MaterialDesc desc =
      ConvertMaterial(scene_, source, model_path_, missing_files_);
  if (use_overrides_) {
    const std::string sidecar =
        material::SidecarMaterialPath(model_path_, source.GetName().C_Str());
    if (platform::IsRegularFile(sidecar)) {
      auto loaded = material::LoadMaterialFile(sidecar);
      if (loaded.ok()) {
        desc = std::move(loaded).value();
      } else {
        core::log::Warn("cannot load material override '{}': {}", sidecar,
                        material::ToString(loaded.error()));
      }
    }
  }
  resolved = Add(std::move(desc));
  return resolved;
}

std::vector<MaterialDesc> MaterialTable::Release() {
  return std::move(materials_);
}

int MaterialTable::Add(MaterialDesc desc) {
  for (size_t i = 0; i < materials_.size(); ++i) {
    if (material::SameAppearance(materials_[i], desc)) {
      return static_cast<int>(i);
    }
  }
  materials_.push_back(std::move(desc));
  return static_cast<int>(materials_.size() - 1);
}

}  // namespace blue::asset::internal
