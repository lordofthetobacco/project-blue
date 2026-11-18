#ifndef BLUE_COOK_MODEL_COOK_H_
#define BLUE_COOK_MODEL_COOK_H_

#include <string>
#include <string_view>
#include <vector>

#include "asset/mesh_data.h"
#include "cook/cook_error.h"
#include "cook/texture_cook.h"
#include "core/result.h"
#include "material/material.h"
#include "prefab/prefab.h"

// Model import (OBJ, glTF, FBX through assimp) into a prefab: node hierarchy,
// meshes in the GPU vertex layout optimised for the vertex cache and fetch,
// materials with their textures cooked (BC7 / BC5 with mips), animation clips
// and the EditInfo needed to re-import.

namespace blue::cook {

// Which axis is "up" in the source file. The engine is Y-up.
enum class UpAxis {
  kY,  // as is
  kZ,  // rotated -90 degrees about X so +Z becomes +Y
};

struct ModelCookOptions {
  // Uniform scale applied to the whole model (e.g. 0.01 for centimetres).
  float scale = 1.0f;
  UpAxis up_axis = UpAxis::kY;
  // Bakes every node transform at the rest pose into one mesh attached to
  // the prefab root (node -1) and drops the hierarchy and the clips. For
  // static models made of many parts, such as a house.
  bool merge_static_nodes = false;
  // Nodes removed with their whole subtree, by exact name.
  std::vector<std::string> drop_nodes;
  // Replace imported materials by their sidecar .bmat files where one
  // exists (material::SidecarMaterialPath()).
  bool use_material_overrides = true;
  // Run meshoptimizer's vertex cache and vertex fetch optimisation per mesh
  // (triangle order changes within each submesh; no simplification).
  bool optimize_meshes = true;
  CookQuality texture_quality = CookQuality::kHigh;
  bool compress_textures = true;
  // > 0: textures larger than this are halved until they fit.
  int max_texture_size = 0;
};

// Returns `options` as "key = value" lines, the form stored in
// prefab::EditInfo::import_settings. Stable order; round-trips through
// ParseModelCookOptions().
[[nodiscard]] std::string FormatModelCookOptions(
    const ModelCookOptions& options);

// Parses text written by FormatModelCookOptions(). Unknown keys are ignored
// with a warning (forward compatible); missing keys keep their defaults.
// Fails with kInvalidArgument for a malformed value.
[[nodiscard]] core::Result<ModelCookOptions, CookError> ParseModelCookOptions(
    std::string_view text);

// Imports the model at `path` and cooks it into a prefab:
//   hierarchy  the file's nodes, parents first (minus dropped subtrees); a
//              node named after the file is added as the single root when
//              scale or up-axis correction is needed, so animated roots
//              keep working;
//   meshes     one per node with geometry, in the node's space, one submesh
//              per material; uv0 from UV channel 0, uv1 from channel 1 or a
//              copy of uv0; tangents follow uv0;
//   materials  the file's (or sidecar) materials, converted to prefab
//              materials whose textures are cooked once each: base colour
//              and emissive as BC7 sRGB, normal maps as BC5, occlusion /
//              roughness / metallic packed into one BC7 ORM texture (missing
//              channels 1); a texture slot keeps the source's UV set;
//   clips      every animation (times in seconds, linear keys), loop off;
//   bounds     the meshes at the rest pose;
//   edit       source_model = `path`, import_settings =
//              FormatModelCookOptions(options), the source images per
//              texture ("<model>#<index>" for embedded images).
// Physics is left kNone (see collision_cook.h), sockets, lights and
// children empty. Fails with kFileNotFound, kParseFailed, kNoMeshes,
// kTooLarge (vertex count over the index range) or kTextureLoadFailed when a
// referenced texture cannot be decoded. Logs timings.
[[nodiscard]] core::Result<prefab::Prefab, CookError> CookModel(
    const std::string& path, const ModelCookOptions& options = {});

// Building blocks for prefabs made without a model file (blue-pack samples,
// the editor):

// Stores `mesh` in `prefab` as a new prefab::Mesh named `name` attached to
// `node` (-1 = root), optionally optimised like CookModel() does, with its
// bounds recomputed. Does not update prefab.bounds.
void AddMeshToPrefab(prefab::Prefab& prefab, asset::MeshData mesh, int node,
                     std::string name, bool optimize = true);

// Converts `desc` into a prefab material appended to prefab.materials and
// returns its index. Its texture refs are decoded and cooked as CookModel()
// does (usage per slot, ORM packing, the options' quality / compression /
// size); a texture already in the prefab with the same sources and format
// (per prefab.edit.texture_sources) is reused. Fails with kTextureLoadFailed
// when a referenced texture cannot be decoded.
[[nodiscard]] core::Result<int, CookError> AddMaterialToPrefab(
    prefab::Prefab& prefab, const material::MaterialDesc& desc,
    const ModelCookOptions& options = {});

// Returns the box enclosing every mesh of `prefab` at the rest pose, in
// prefab space (an empty box at the origin without meshes).
[[nodiscard]] asset::Bounds ComputePrefabBounds(const prefab::Prefab& prefab);

}  // namespace blue::cook

#endif  // BLUE_COOK_MODEL_COOK_H_
