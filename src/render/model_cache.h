#ifndef BLUE_RENDER_MODEL_CACHE_H_
#define BLUE_RENDER_MODEL_CACHE_H_

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "animation/clip.h"
#include "animation/hierarchy.h"
#include "asset/mesh_data.h"
#include "render/renderer.h"

namespace blue::render {

// One mesh-bearing node of an uploaded model.
struct UploadedPart {
  int node = -1;  // Index into UploadedModel::hierarchy.nodes.
  MeshId mesh{};
  // Bounds of the mesh in the node's local space, for DrawItem::bounds.
  asset::Bounds bounds;
};

// An animated model on the GPU: one mesh per part, drawn with the world
// matrix of its node. The CPU meshes are dropped after the upload; the node
// hierarchy and the clips are kept for posing.
struct UploadedModel {
  animation::Hierarchy hierarchy;
  std::vector<animation::Clip> clips;
  std::vector<UploadedPart> parts;
  asset::Bounds rest_bounds;  // Model space, rest pose.
};

// Resolves a model path to an UploadedModel, importing it with
// asset::ImportAnimatedModel and uploading its parts once. Failures are
// remembered, so a broken path is logged once. Returned models live at a
// stable address until the cache is destroyed. Meant for loading at startup:
// Get() normalizes the path, which allocates. Borrows `renderer`, which must
// outlive the cache. Not thread-safe.
class ModelCache {
 public:
  explicit ModelCache(Renderer& renderer) : renderer_(renderer) {}

  ModelCache(const ModelCache&) = delete;
  ModelCache& operator=(const ModelCache&) = delete;

  // Returns the model at `path`, loading it on first use (the load time is
  // logged), or nullptr if loading failed (the failure was logged).
  [[nodiscard]] const UploadedModel* Get(std::string_view path);

 private:
  Renderer& renderer_;
  // Keyed by absolute, normalized path; null marks a failed load.
  std::unordered_map<std::string, std::unique_ptr<UploadedModel>> models_;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_MODEL_CACHE_H_
