#ifndef BLUE_EDITOR_PREFAB_MODE_PREFAB_PANELS_H_
#define BLUE_EDITOR_PREFAB_MODE_PREFAB_PANELS_H_

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "editor/editor_context.h"
#include "editor/panel.h"
#include "editor/prefab_mode/prefab_mode_state.h"
#include "editor/viewport/uv_view.h"
#include "editor/viewport/viewport_panel.h"
#include "prefab/prefab.h"

// The panels of the editor's Prefab mode. Each works on the open prefab
// document (EditorContext::prefab_document()) and shows a hint when there
// is none; every change goes through the document's undo history. They
// borrow the mode's PrefabModeShared, which outlives them. UI-rate code.

namespace blue::editor {

// "Hierarchy": the node tree with the meshes, sockets, lights and child
// prefabs attached to each node, and the prefab root's attachments. Click
// selects, drag a node onto another node to reparent it (keeping its pose),
// drag sockets / lights / children onto a node to re-attach them, drop a
// .pbo onto a node to add it as a child prefab. Context menu: rename,
// delete, add socket / light / child here. Its Update() also runs the mode's
// modal dialogs (import, ORM packing, rename) and the background cook.
//
// It also owns the mode's shared state and viewport client (the other
// panels, the viewport and the commands borrow them), so they live exactly
// as long as the registered panels.
class PrefabHierarchyPanel final : public Panel {
 public:
  PrefabHierarchyPanel(std::unique_ptr<PrefabModeShared> shared,
                       std::unique_ptr<ViewportClient> viewport_client)
      : owned_shared_(std::move(shared)),
        owned_client_(std::move(viewport_client)),
        shared_(*owned_shared_) {}

  void Update(EditorContext& context, float dt) override;
  void Draw(EditorContext& context) override;

 private:
  void DrawNode(EditorContext& context, PrefabDocument& document, int node);
  // Draws the meshes, sockets, lights and children attached to `node`.
  void DrawAttachments(EditorContext& context, PrefabDocument& document,
                       int node);
  // Context menu and drag-and-drop of the last item, a node or the root
  // (-1).
  void NodeTargets(EditorContext& context, PrefabDocument& document, int node);
  void DrawImportModal(EditorContext& context);
  void DrawOrmModal(EditorContext& context);
  void DrawRenameModal(EditorContext& context);

  std::unique_ptr<PrefabModeShared> owned_shared_;
  std::unique_ptr<ViewportClient> owned_client_;
  PrefabModeShared& shared_;
  // Children of every node, rebuilt each frame from the hierarchy.
  std::vector<std::vector<int>> children_;
};

// "Inspector": the primary selection's properties (node transform, mesh
// statistics and submesh materials, socket, light, child prefab, clip,
// material, texture, collider), or the prefab summary without a selection.
class PrefabInspectorPanel final : public Panel {
 public:
  explicit PrefabInspectorPanel(PrefabModeShared& shared) : shared_(shared) {}

  void Draw(EditorContext& context) override;

 private:
  void DrawNode(EditorContext& context, PrefabDocument& document, int node);
  void DrawMesh(EditorContext& context, PrefabDocument& document, int mesh);
  void DrawSocket(PrefabDocument& document, int socket);
  void DrawLight(PrefabDocument& document, int light);
  void DrawChild(EditorContext& context, PrefabDocument& document, int child);
  void DrawTexture(EditorContext& context, PrefabDocument& document,
                   int texture);
  // The prefab's tags: edit a value, remove a tag, add one.
  void DrawTags(EditorContext& context, PrefabDocument& document);

  PrefabModeShared& shared_;
  // The tag being added, typed in the Tags section.
  std::string new_tag_key_;
  std::string new_tag_value_;
};

// "Materials": the material list (add, duplicate, delete) with the editor
// of the selected material, and the texture list (format, size, memory,
// users; remove, remove unused).
class PrefabMaterialsPanel final : public Panel {
 public:
  explicit PrefabMaterialsPanel(PrefabModeShared& shared) : shared_(shared) {}

  void Draw(EditorContext& context) override;

 private:
  void DrawTextureList(EditorContext& context, PrefabDocument& document);

  PrefabModeShared& shared_;
  int material_ = 0;  // shown when no material is selected
};

// "Animation": clip list (rename, loop, delete, merge the checked clips),
// the timeline of the current clip with play / pause / scrub / speed / loop
// preview on the document's pose, trim range, and event markers (drag to
// move, add at the playhead, rename, delete). Its Update() advances the
// preview.
class PrefabAnimationPanel final : public Panel {
 public:
  explicit PrefabAnimationPanel(PrefabModeShared& shared) : shared_(shared) {}

  void Update(EditorContext& context, float dt) override;
  void Draw(EditorContext& context) override;

 private:
  void DrawClipList(EditorContext& context, PrefabDocument& document);
  void DrawTimeline(EditorContext& context, PrefabDocument& document);
  void DrawEvents(EditorContext& context, PrefabDocument& document);

  PrefabModeShared& shared_;
  std::vector<bool> merge_checked_;
  std::string merge_name_ = "merged";
  float trim_start_ = 0.0f;
  float trim_end_ = 0.0f;
  int trim_clip_ = -1;  // the clip the trim range was set for
  // Event marker being dragged on the timeline (-1 = none) and its time.
  int dragged_event_ = -1;
  float dragged_time_ = 0.0f;
  std::string new_event_name_ = "event";
};

// "Physics": the prefab's collider and body.
class PrefabPhysicsPanel final : public Panel {
 public:
  explicit PrefabPhysicsPanel(PrefabModeShared& shared) : shared_(shared) {}

  void Draw(EditorContext& context) override;

 private:
  PrefabModeShared& shared_;
};

// "Prefab info": counts and sizes, save validation, the source model and
// its import settings, re-import.
class PrefabInfoPanel final : public Panel {
 public:
  explicit PrefabInfoPanel(PrefabModeShared& shared) : shared_(shared) {}

  void Draw(EditorContext& context) override;

 private:
  PrefabModeShared& shared_;
};

// "UV layout": a mesh's uv0 or uv1 triangles over an optional texture.
class PrefabUvPanel final : public Panel {
 public:
  explicit PrefabUvPanel(PrefabModeShared& shared) : shared_(shared) {}

  void Draw(EditorContext& context) override;

 private:
  PrefabModeShared& shared_;
  UvViewState view_;
  int background_ = -1;  // texture index, -1 = none
};

// Starts a re-import of the open document from its recorded source with
// its stored options (shown in the import dialog first).
void RequestReimport(EditorContext& context, PrefabModeShared& shared);

}  // namespace blue::editor

#endif  // BLUE_EDITOR_PREFAB_MODE_PREFAB_PANELS_H_
