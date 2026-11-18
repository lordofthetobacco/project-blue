#ifndef BLUE_EDITOR_PREFAB_MODE_PREFAB_VIEWPORT_H_
#define BLUE_EDITOR_PREFAB_MODE_PREFAB_VIEWPORT_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <glm/mat4x4.hpp>

#include "animation/transform.h"
#include "asset/mesh_data.h"
#include "editor/editor_context.h"
#include "editor/prefab_mode/prefab_mode_state.h"
#include "editor/viewport/editor_camera.h"
#include "editor/viewport/viewport_panel.h"
#include "render/scene_view.h"
#include "world/prefab_cache.h"
#include "world/prefab_pose.h"

namespace blue::editor {

// What the viewport shows in Prefab mode: the document's prefab (its
// preview pose; the rest pose comes from the document itself so node edits
// show while dragging), its child prefabs, its point lights under a default
// sun, debug draws (sockets with labels, lights, collider, selection
// bounds), the gizmo of the selected socket / light / child / node /
// primitive collider, and click picking (sockets and lights near the ray
// first, then mesh triangles, then child prefab bounds).
//
// Keeps its per-frame vectors between frames, so a warm frame does not
// allocate. Borrows `shared`. Not thread-safe.
class PrefabViewportClient final : public ViewportClient {
 public:
  explicit PrefabViewportClient(PrefabModeShared& shared) : shared_(shared) {}

  void DrawOverlay(EditorContext& context, ViewportPanel& viewport) override;
  void Pick(EditorContext& context, ViewportPanel& viewport, const Ray& ray,
            bool additive) override;
  [[nodiscard]] std::optional<asset::Bounds> FocusBounds(
      EditorContext& context) override;
  void BuildView(EditorContext& context, ViewportPanel& viewport,
                 render::SceneView& view) override;

 private:
  // A child prefab shown in the preview.
  struct ChildPreview {
    std::string path;
    std::optional<world::PrefabId> prefab;  // nullopt: failed to load
    std::optional<world::PrefabPose> pose;
  };

  // Computes this frame's node matrices and child previews (once per
  // frame, whichever callback comes first).
  void Prepare(EditorContext& context, const PrefabDocument& document);
  // Returns the model-space matrix of node `node` this frame (-1 =
  // identity).
  [[nodiscard]] glm::mat4 Node(int node) const;
  // Returns the matrix placing child prefab `child`.
  [[nodiscard]] glm::mat4 ChildMatrix(const prefab::Prefab& prefab,
                                      int child) const;
  // Frames the prefab from the front three-quarters when another document
  // was opened (the weapons point down -Z, so the default view is behind).
  void FrameNewDocument(ViewportPanel& viewport,
                        const PrefabDocument& document);
  // Shows the gizmo for the primary selection and applies its drag.
  void Manipulate(EditorContext& context, ViewportPanel& viewport,
                  PrefabDocument& document);

  PrefabModeShared& shared_;
  int prepared_frame_ = -1;
  // The document last framed and its path (a document loaded at a reused
  // address is caught by its path and untouched revision).
  const PrefabDocument* framed_document_ = nullptr;
  std::string framed_path_;
  std::vector<animation::Transform> rest_;
  std::vector<glm::mat4> nodes_;
  std::vector<ChildPreview> children_;
  std::vector<render::DrawItem> draws_;
  std::vector<uint32_t> selected_;
  std::vector<render::PointLight> lights_;
  // Draw index range of each child prefab in draws_ (first, count).
  std::vector<std::pair<uint32_t, uint32_t>> child_draws_;
};

}  // namespace blue::editor

#endif  // BLUE_EDITOR_PREFAB_MODE_PREFAB_VIEWPORT_H_
