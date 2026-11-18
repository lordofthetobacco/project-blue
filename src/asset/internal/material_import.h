#ifndef BLUE_ASSET_INTERNAL_MATERIAL_IMPORT_H_
#define BLUE_ASSET_INTERNAL_MATERIAL_IMPORT_H_

#include <assimp/scene.h>

#include <set>
#include <string>
#include <vector>

#include "material/material.h"

// Conversion of assimp materials to material::MaterialDesc, shared by the
// importers in blue_asset. Internal to the library: assimp types appear
// here, so only blue_asset sources include it.

namespace blue::asset::internal {

// Returns the MaterialDesc of assimp material `material` of the model file at
// `model_path`. Reads the glTF metallic-roughness keys where present and
// falls back to the Phong/MTL ones:
//   base colour   $clr.base, else Kd (diffuse) with "d" (opacity) as alpha;
//                 colours are taken as linear;
//   metallic      metallicFactor, else 0 (MTL cannot say; Pm if present);
//   roughness     roughnessFactor (Pr), else RoughnessFromShininess(Ns) for
//                 Ns > 0, else 0.5;
//   emissive      Ke / emissiveFactor, times KHR_materials_emissive_strength;
//   alpha         glTF alphaMode MASK + alphaCutoff; BLEND imports as opaque;
//   textures      base colour (else map_Kd), glTF metallic-roughness,
//                 normals (glTF normalTexture + scale, MTL "norm", else
//                 map_Bump treated as a tangent-space normal map), occlusion
//                 (glTF occlusionTexture + strength), emissive.
// Embedded textures become TextureRef{model_path, index}; file textures are
// resolved against the model's directory and dropped with a warning (once
// per path, remembered in `missing_files`) if the file does not exist.
// Each slot's UV channel (0 or 1) is kept in MaterialDesc::uv_set; textures
// on other channels are dropped with a warning. Assimp's
// fallback material (AI_DEFAULT_MATERIAL_NAME) becomes DefaultMaterial().
[[nodiscard]] material::MaterialDesc ConvertMaterial(
    const aiScene& scene, const aiMaterial& material,
    const std::string& model_path, std::set<std::string>& missing_files);

// Returns the material used for meshes without one: matte light grey.
[[nodiscard]] material::MaterialDesc DefaultMaterial();

// Converts the scene's materials on first use and hands out indices into
// one table: duplicates (material::SameAppearance) share an entry, and
// sidecar .bmat files replace imported materials when enabled. Borrows the
// scene, which must outlive the table. Not thread-safe.
class MaterialTable {
 public:
  MaterialTable(const aiScene& scene, std::string model_path,
                bool use_overrides);

  MaterialTable(const MaterialTable&) = delete;
  MaterialTable& operator=(const MaterialTable&) = delete;

  // Returns the table index of assimp material `index`; an out-of-range
  // index maps to DefaultMaterial().
  [[nodiscard]] int Resolve(unsigned index);

  // Returns the table, leaving this object empty.
  [[nodiscard]] std::vector<material::MaterialDesc> Release();

 private:
  // Returns the index of `desc` in materials_, adding it if no entry looks
  // the same.
  int Add(material::MaterialDesc desc);

  const aiScene& scene_;
  std::string model_path_;
  bool use_overrides_;
  std::vector<int> resolved_;  // per assimp material; -1 until converted
  int default_index_ = -1;     // table entry of DefaultMaterial(), or -1
  std::set<std::string> missing_files_;
  std::vector<material::MaterialDesc> materials_;
};

}  // namespace blue::asset::internal

#endif  // BLUE_ASSET_INTERNAL_MATERIAL_IMPORT_H_
