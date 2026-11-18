#ifndef BLUE_UI_EDITOR_PANELS_H_
#define BLUE_UI_EDITOR_PANELS_H_

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>

#include "material/material.h"
#include "render/material_cache.h"
#include "render/renderer.h"
#include "render/scene_view.h"
#include "scene/scene.h"

namespace blue::ui {

// One material the Materials panel can edit.
struct MaterialSlot {
  std::string label;  // e.g. "submesh 0: BaseMetal"
  render::MaterialId id = render::kDefaultMaterial;
  // Where "Save as .bmat" writes by default: the material's own file, its
  // model's sidecar (material::SidecarMaterialPath) or a new sample file.
  std::string save_path;
};

// What the panels hand back to the application for one frame; the edits of
// values (materials, scene settings, post settings) are already applied.
struct EditorActions {
  // Pick the entity in the middle of the view (EditorPanels::Select()).
  bool pick_entity = false;
  // Scene::environment.hdri_path changed: load it (SetEnvironment).
  bool load_environment = false;
  // Environment intensity or rotation changed (SetEnvironmentParams).
  bool environment_params_changed = false;
  // Point the sun (direction and colour) at the environment's sun.
  bool align_sun = false;
  // Set the sun's intensity to the environment's sun.
  bool sun_intensity_from_environment = false;
};

// Everything the panels read and edit, borrowed for one Draw() call.
struct EditorContext {
  scene::Scene& scene;
  render::Renderer& renderer;
  render::MaterialCache& material_files;
  render::PostSettings& post;
  // The selected entity's materials (empty without a selection), and the
  // weapons' materials.
  std::span<const MaterialSlot> entity_slots;
  std::span<const MaterialSlot> weapon_slots;
  // A sun was found in the current environment (enables the sun buttons).
  bool environment_has_sun = false;
};

// The ImGui editor windows: Materials (pick an entity, edit its materials
// live, save them as .bmat files, assign a .bmat to the entity), Environment
// (HDRI, sky and sun, exposure, tonemapper) and Post (bloom, SSAO, FXAA).
// Draw() must run inside an ImGui frame. Text fields use fixed buffers, and
// nothing is drawn or allocated while the panels are not drawn. Not
// thread-safe.
class EditorPanels {
 public:
  EditorPanels() = default;

  EditorPanels(const EditorPanels&) = delete;
  EditorPanels& operator=(const EditorPanels&) = delete;

  // Emits the three windows and applies their edits to `context`.
  [[nodiscard]] EditorActions Draw(const EditorContext& context);

  // Selects entity `entity` (an index into Scene::entities), or nothing.
  void Select(std::optional<size_t> entity);
  [[nodiscard]] std::optional<size_t> selected_entity() const {
    return selected_entity_;
  }

 private:
  // Long enough for any path the scene file accepts in practice.
  static constexpr size_t kPathCapacity = 512;
  using PathBuffer = std::array<char, kPathCapacity>;
  // The texture slots of a material, in MaterialDesc order.
  static constexpr size_t kTextureSlotCount = 7;

  // Each Draw* helper emits one window or section. Returns true when "Pick
  // in view" was pressed.
  [[nodiscard]] bool DrawMaterials(const EditorContext& context);
  [[nodiscard]] bool DrawEntityPicker(const EditorContext& context);
  // Emits the editors of the material in `slot`.
  void DrawMaterialEditor(const EditorContext& context,
                          const MaterialSlot& slot);
  // Emits the texture slot fields of `desc`; returns true if one was
  // applied.
  [[nodiscard]] bool DrawTextureSlots(material::MaterialDesc& desc);
  // Emits the file buttons of `slot`: save, and for entity materials assign.
  void DrawMaterialFiles(const EditorContext& context, const MaterialSlot& slot,
                         bool is_entity_slot);
  [[nodiscard]] EditorActions DrawEnvironment(const EditorContext& context);
  void DrawPost(render::PostSettings& post) const;
  // Fills the text buffers from material `desc` shown in `slot`.
  void LoadBuffers(const material::MaterialDesc& desc,
                   const MaterialSlot& slot);

  std::optional<size_t> selected_entity_;
  // The slot being edited: an index into the entity's slots, or into the
  // weapons' slots past them.
  size_t selected_slot_ = 0;
  // The material the buffers were filled from; refilled when it changes.
  std::optional<render::MaterialId> buffered_material_;
  std::array<PathBuffer, kTextureSlotCount> texture_paths_{};
  PathBuffer save_path_{};
  PathBuffer assign_path_{};
  PathBuffer hdri_path_{};
  // The scene HDRI path hdri_path_ was last filled from.
  std::string buffered_hdri_;
  // Outcome of the last file operation, shown under the buttons.
  std::string file_status_;
};

}  // namespace blue::ui

#endif  // BLUE_UI_EDITOR_PANELS_H_
