#ifndef BLUE_ASSET_ANIMATED_MODEL_H_
#define BLUE_ASSET_ANIMATED_MODEL_H_

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/mat4x4.hpp>

#include "animation/clip.h"
#include "animation/hierarchy.h"
#include "asset/mesh_data.h"
#include "asset/model_importer.h"
#include "core/result.h"
#include "material/material.h"

namespace blue::asset {

class EmbeddedImages;

// The mesh drawn at one node of an AnimatedModel. `mesh` is in the node's
// local space (node transforms are not baked in), so it is drawn with the
// node's world matrix from animation::ComputeWorldMatrices(). All primitives
// of the node are merged into this one mesh, one submesh per material; the
// submeshes index AnimatedModel::materials.
struct ModelPart {
  int node = -1;  // index into AnimatedModel::hierarchy.nodes
  MeshData mesh;
};

// A model whose node transforms can be animated rigidly: its node hierarchy,
// one part per mesh-bearing node, and its animation clips (whose channels
// index `hierarchy`). `rest_bounds` encloses every part's vertices at the rest
// pose, in model space (the space of the root nodes' parents). `materials`
// is shared by all parts.
struct AnimatedModel {
  animation::Hierarchy hierarchy;
  std::vector<ModelPart> parts;
  std::vector<animation::Clip> clips;
  Bounds rest_bounds;
  std::vector<material::MaterialDesc> materials;
};

// Returns the index of the first clip called `name`, or -1 if there is none.
[[nodiscard]] int FindClip(const AnimatedModel& model, std::string_view name);

// Returns the model-space box enclosing the eight corners of each part's
// bounds transformed by world[part.node]. Exact for poses without rotation,
// conservative otherwise. `world` holds one matrix per hierarchy node.
[[nodiscard]] Bounds PoseBounds(std::span<const ModelPart> parts,
                                std::span<const glm::mat4> world);

// Loads the file at `path` (meant for glTF 2.0; any format assimp reads works)
// keeping its node hierarchy and animations instead of baking them.
// Hierarchy nodes are in parent-before-child order with their rest TRS and
// assimp's node names (glTF node names). Vertices, submeshes and materials
// are imported as by ImportModel() (UVs, tangents, merged materials, sidecar
// overrides), but per node.
// Clip times are seconds. Assimp does not report glTF sampler interpolation:
// every channel is imported as kLinear, and CUBICSPLINE tangents are dropped.
// Files without animations import with empty `clips`. Errors as ImportModel():
// kFileNotFound, kParseFailed, kNoMeshes (no part with triangles and normals)
// and kTooLarge (a part's vertices overflow Index).
// When `embedded` is not null, a successful import also moves the file's
// embedded images into it (asset/texture_loader.h), so the textures that
// `materials` reference with embedded_index can be decoded without parsing
// the file again; it is left unchanged on failure.
[[nodiscard]] core::Result<AnimatedModel, ImportError> ImportAnimatedModel(
    const std::string& path, const ImportOptions& options = {},
    EmbeddedImages* embedded = nullptr);

}  // namespace blue::asset

#endif  // BLUE_ASSET_ANIMATED_MODEL_H_
