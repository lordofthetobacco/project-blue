#ifndef BLUE_EDITOR_SCENE_MODE_SCENE_MODE_H_
#define BLUE_EDITOR_SCENE_MODE_SCENE_MODE_H_

#include <imgui.h>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/vec3.hpp>

#include "asset/image_decoder.h"
#include "core/result.h"
#include "editor/document.h"
#include "editor/editor_context.h"
#include "editor/panel.h"
#include "editor/undo.h"
#include "editor/viewport/viewport_panel.h"
#include "prefab/prefab.h"
#include "scene/scene_data.h"
#include "world/world.h"

// Scene mode internals shared by its panels (outliner, inspector, scene
// settings, problems), its viewport client and its text commands. UI code:
// main thread only.

namespace blue::editor {

// The runtime side of the open scene document: its world::World, the
// problems found while building it, the scene's environment as applied to
// the renderer, and helpers the panels share. One per editor, owned by the
// Scene mode registration and borrowed by the panels; it borrows the
// context, which must outlive it. Neither copyable nor movable.
//
// Rebuild policy: the world is rebuilt when the document object changes, or
// when its structural revision advanced and no edit is open. While an edit
// is open (a drag over several frames) only the transforms of the selected
// instances are refreshed, so a material slider does not create a derived
// GPU material per frame (the cache never frees them); its result shows on
// release. Transform-only edits use ChangeKind::kParameters and are applied
// with World::SetInstanceTransform.
class SceneModeState {
 public:
  explicit SceneModeState(EditorContext& context);
  ~SceneModeState();
  SceneModeState(const SceneModeState&) = delete;
  SceneModeState& operator=(const SceneModeState&) = delete;

  // Brings the world and (in Scene mode) the renderer's environment up to
  // date with the document; call once per frame before panels draw. Logs
  // every rebuild and environment load with its time, and notes when a
  // sync takes longer than 20 ms.
  void Sync();

  // The open scene document, or null.
  [[nodiscard]] SceneDocument* document() { return context_.scene_document(); }
  // The world of the open document, or null without one.
  [[nodiscard]] world::World* world() { return world_.get(); }
  [[nodiscard]] EditorContext& context() { return context_; }

  // Problems of the last build: world issues (unloadable prefabs, nested
  // cycles, ...) then override issues (scene::ValidateAgainstPrefabs()).
  struct Problem {
    scene::InstanceId instance = scene::kNoInstance;
    std::string message;
  };
  [[nodiscard]] std::span<const Problem> problems() const { return problems_; }
  // Material names per scene.prefabs entry from the last build (empty list
  // for an unloadable prefab).
  [[nodiscard]] std::span<const std::vector<std::string>> material_names()
      const {
    return material_names_;
  }
  // Number of scene point lights at the last build: the first entries of
  // world()->point_lights() when it still matches the document.
  [[nodiscard]] size_t built_scene_light_count() const {
    return built_scene_lights_;
  }

  // Forces a world rebuild on the next Sync() (e.g. after a prefab file
  // changed on disk).
  void RequestRebuild() { rebuild_requested_ = true; }

  // Runs `change` on the document's state as one undo step labelled
  // `label`. Returns false (nothing done) without a scene document.
  template <typename Change>
  bool Edit(std::string_view label, ChangeKind kind, Change&& change) {
    SceneDocument* doc = document();
    if (doc == nullptr) {
      return false;
    }
    doc->Edit(label, kind, std::forward<Change>(change));
    return true;
  }

  // Returns the material names of the prefab at `path` (loading it through
  // the cache), or an empty list if it cannot be loaded. For the override
  // library picker.
  [[nodiscard]] std::vector<std::string> PrefabMaterialNames(
      const std::string& path);

  // Returns the material `name` of the prefab at `path` (its own materials
  // only, not nested prefabs), or nullopt. Its values seed new override
  // tweaks.
  [[nodiscard]] std::optional<prefab::Material> FindPrefabMaterial(
      const std::string& path, std::string_view name);

  // Returns where an instance dropped along the viewport ray through ImGui
  // screen position `screen` (or the image centre when nullopt) lands:
  // picked geometry, else the ground, else in front of the camera.
  [[nodiscard]] glm::vec3 DropPoint(std::optional<ImVec2> screen);

  // Places an instance of `prefab_path` at `position` (one undo step),
  // selects it and returns its id; kNoInstance without a document.
  scene::InstanceId PlaceInstance(const std::string& prefab_path,
                                  const glm::vec3& position);

  // Selection-wide operations used by shortcuts, toolbar and commands; each
  // is one undo step and reports in the status bar. Return false when there
  // was nothing to do.
  bool DuplicateSelection();
  bool DeleteSelection();
  bool GroupSelection();
  bool UngroupSelection();
  // Adds a point light named "light <n>" at `position` and selects it;
  // returns its index, or nullopt without a document.
  std::optional<size_t> AddPointLight(const glm::vec3& position);
  // Selects the next (+1) / previous (-1) instance in outliner order.
  bool StepSelection(int step);

  // Points the sun away from the brightest region of the scene's HDRI
  // (taking the environment rotation into account). Returns a readable
  // error when the scene has no HDRI or it cannot be loaded.
  core::Result<void, std::string> AlignSunToHdri();

 private:
  // Sync() without the timing.
  void SyncDocument();
  void Rebuild(const SceneDocument& doc);
  void SyncEnvironment(const scene::SceneData& scene);
  // Returns the HDRI at `path`, loading (and keeping) it if it is not the
  // one loaded last; null with `error` set on failure.
  const asset::ImageDataF* LoadHdri(const std::string& path,
                                    std::string& error);

  EditorContext& context_;
  std::unique_ptr<world::World> world_;
  const SceneDocument* built_document_ = nullptr;
  std::string built_path_;
  DocumentRevision built_revision_;
  bool rebuild_requested_ = false;
  std::vector<Problem> problems_;
  std::vector<std::vector<std::string>> material_names_;
  size_t built_scene_lights_ = 0;

  // The environment the renderer shows for Scene mode; nullopt when it is
  // not (another mode was active since).
  struct AppliedEnvironment {
    std::string hdri_path;
    float intensity = 0.0f;
    float rotation_radians = 0.0f;
  };
  std::optional<AppliedEnvironment> applied_environment_;
  std::string hdri_path_;  // of hdri_
  std::optional<asset::ImageDataF> hdri_;
};

// Applies the change of the ImGui widget drawn just before as part of one
// undo step that lasts while the widget is active (a drag): `apply` gets the
// state to change when `changed`; the step is committed when the widget is
// released.
template <typename Apply>
void EditFromWidget(SceneDocument& doc, bool changed, std::string_view label,
                    ChangeKind kind, Apply&& apply) {
  if (changed) {
    std::forward<Apply>(apply)(doc.BeginEdit(label, kind));
  }
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    doc.CommitEdit();
  }
}

// The panels and the viewport client of Scene mode (defined in their own
// files); `state` must outlive them. The outliner owns the state and the
// viewport client (the shell keeps panels alive until exit) and calls
// SceneModeState::Sync() from its Update(), which runs every frame.
std::unique_ptr<Panel> MakeOutlinerPanel(
    std::unique_ptr<SceneModeState> state,
    std::unique_ptr<ViewportClient> client);
std::unique_ptr<Panel> MakeSceneInspectorPanel(SceneModeState& state);
std::unique_ptr<Panel> MakeSceneSettingsPanel(SceneModeState& state);
std::unique_ptr<Panel> MakeProblemsPanel(SceneModeState& state);
std::unique_ptr<ViewportClient> MakeSceneViewportClient(SceneModeState& state);

// Registers Scene mode's text commands ("scene-..."); see
// scene_commands.cc for the list.
void RegisterSceneCommands(SceneModeState& state);

// Draws the sun, environment, gravity and player start settings (Scene
// Settings panel; also the inspector for those selection kinds).
void DrawSunSettings(SceneModeState& state);
void DrawEnvironmentSettings(SceneModeState& state);
void DrawPlayerStartSettings(SceneModeState& state);
void DrawPointLightSettings(SceneModeState& state, size_t index);

}  // namespace blue::editor

#endif  // BLUE_EDITOR_SCENE_MODE_SCENE_MODE_H_
