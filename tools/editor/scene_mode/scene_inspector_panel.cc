// Scene mode's inspector: the primary selection's properties. Instances get
// name, visibility, prefab, transform, material overrides (replacement from
// a material library .pbo and parameter tweaks) and the physics override;
// lights, the player start, the sun and the environment reuse the Scene
// Settings widgets.

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "core/path.h"
#include "editor/dialogs.h"
#include "editor/drag_drop.h"
#include "editor/editor_context.h"
#include "editor/panel.h"
#include "editor/scene_mode/scene_edit.h"
#include "editor/scene_mode/scene_mode.h"
#include "editor/selection.h"
#include "scene/scene.h"
#include "scene/scene_data.h"

namespace blue::editor {
namespace {

// Copies `text` into a fixed buffer for ImGui::InputText.
std::array<char, 256> TextBuffer(std::string_view text) {
  std::array<char, 256> buffer{};
  const size_t length = std::min(text.size(), buffer.size() - 1);
  std::memcpy(buffer.data(), text.data(), length);
  return buffer;
}

class SceneInspectorPanel final : public Panel {
 public:
  explicit SceneInspectorPanel(SceneModeState& state) : state_(state) {}

  void Draw(EditorContext& context) override;

 private:
  void DrawInstance(EditorContext& context, SceneDocument& doc,
                    scene::InstanceId id);
  void DrawTransform(SceneDocument& doc, scene::InstanceId id);
  void DrawMaterialOverrides(SceneDocument& doc, scene::InstanceId id);
  void DrawMaterialOverride(SceneDocument& doc, scene::InstanceId id,
                            const std::string& prefab_path,
                            const std::string& material_name);
  void DrawReplacement(SceneDocument& doc, scene::InstanceId id,
                       const std::string& material_name);
  void DrawTweaks(SceneDocument& doc, scene::InstanceId id,
                  const std::string& prefab_path,
                  const std::string& material_name);
  void DrawPhysicsOverride(SceneDocument& doc, scene::InstanceId id);

  SceneModeState& state_;
  // Euler angles (degrees) shown for the rotation while it is dragged, so
  // the three values do not jump between equivalent decompositions.
  scene::InstanceId euler_instance_ = scene::kNoInstance;
  glm::vec3 euler_degrees_{0.0f};
  bool euler_active_ = false;
  // Material library chosen per overridden material name before a material
  // of it is picked.
  std::string pending_library_material_;
  std::string pending_library_path_;
};

void SceneInspectorPanel::Draw(EditorContext& context) {
  SceneDocument* doc = state_.document();
  if (doc == nullptr) {
    ImGui::TextDisabled("No scene open.");
    return;
  }
  const std::optional<SelectionItem> primary = doc->selection().primary();
  if (!primary.has_value()) {
    ImGui::TextDisabled("Nothing selected.");
    ImGui::TextDisabled("Click an object in the viewport or the outliner.");
    return;
  }
  if (doc->selection().items().size() > 1) {
    ImGui::TextDisabled("%zu objects selected; showing the last one.",
                        doc->selection().items().size());
    ImGui::Separator();
  }
  switch (primary->kind) {
    case SelectionKind::kInstance:
      DrawInstance(context, *doc,
                   static_cast<scene::InstanceId>(primary->index));
      break;
    case SelectionKind::kScenePointLight:
      if (primary->index >= 0) {
        DrawPointLightSettings(state_, static_cast<size_t>(primary->index));
      }
      break;
    case SelectionKind::kPlayerStart:
      DrawPlayerStartSettings(state_);
      break;
    case SelectionKind::kSun:
      DrawSunSettings(state_);
      break;
    case SelectionKind::kEnvironment:
      DrawEnvironmentSettings(state_);
      break;
    default:
      ImGui::TextDisabled("Not a scene object.");
      break;
  }
}

void SceneInspectorPanel::DrawInstance(EditorContext& context,
                                       SceneDocument& doc,
                                       scene::InstanceId id) {
  const scene::Instance* instance = scene::FindInstance(doc.state(), id);
  if (instance == nullptr) {
    return;
  }
  ImGui::PushID(static_cast<int>(id));

  std::array<char, 256> name = TextBuffer(instance->name);
  ImGui::InputText("Name", name.data(), name.size());
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    const std::string renamed(name.data());
    doc.Edit("Rename", ChangeKind::kParameters, [&](scene::SceneData& scene) {
      scene::FindInstance(scene, id)->name = renamed;
    });
  }
  ImGui::TextDisabled("id %u", id);

  bool visible = instance->visible;
  if (ImGui::Checkbox("Visible", &visible)) {
    doc.Edit(visible ? "Show" : "Hide", ChangeKind::kStructure,
             [&](scene::SceneData& scene) {
               scene::FindInstance(scene, id)->visible = visible;
             });
  }

  // Prefab.
  const scene::SceneData& scene = doc.state();
  const std::string prefab_path =
      instance->prefab == scene::kNoPrefab
          ? std::string()
          : scene.prefabs[static_cast<size_t>(instance->prefab)];
  ImGui::SeparatorText("Prefab");
  if (prefab_path.empty()) {
    ImGui::TextDisabled("Group (no prefab)");
  } else {
    ImGui::TextWrapped("%s", prefab_path.c_str());
  }
  // Dropping a .pbo on the path replaces the prefab.
  if (const std::optional<std::string> dropped =
          AcceptAssetDrop(FileKind::kPrefab)) {
    const std::string path = *dropped;
    doc.Edit("Change prefab", ChangeKind::kStructure,
             [&](scene::SceneData& edited) {
               scene::FindInstance(edited, id)->prefab =
                   scene::AddPrefab(edited, path);
               scene::CompactPrefabs(edited);
             });
  }
  if (!prefab_path.empty() && ImGui::Button("Open in Prefab mode")) {
    // Copied: OpenFile may replace documents.
    const std::string path = prefab_path;
    if (auto opened = context.OpenFile(path); !opened.ok()) {
      context.Status(opened.error());
    }
    ImGui::PopID();
    return;
  }
  if (!prefab_path.empty()) {
    ImGui::SameLine();
  }
  if (ImGui::Button("Change prefab...")) {
    SceneModeState* state = &state_;
    context.dialogs().Open(
        FileKind::kPrefab, context.asset_root(), [state, id](std::string path) {
          state->Edit(
              "Change prefab", ChangeKind::kStructure,
              [&](scene::SceneData& edited) {
                if (scene::Instance* target = scene::FindInstance(edited, id)) {
                  target->prefab = scene::AddPrefab(edited, path);
                  scene::CompactPrefabs(edited);
                }
              });
        });
  }

  ImGui::SeparatorText("Transform");
  DrawTransform(doc, id);

  if (!prefab_path.empty()) {
    ImGui::SeparatorText("Material overrides");
    DrawMaterialOverrides(doc, id);
    ImGui::SeparatorText("Physics");
    DrawPhysicsOverride(doc, id);
  }
  ImGui::PopID();
}

void SceneInspectorPanel::DrawTransform(SceneDocument& doc,
                                        scene::InstanceId id) {
  const scene::Transform& transform =
      scene::FindInstance(doc.state(), id)->transform;
  const auto edit_transform =
      [&](scene::SceneData& scene) -> scene::Transform& {
    return scene::FindInstance(scene, id)->transform;
  };

  glm::vec3 position = transform.position;
  EditFromWidget(doc, ImGui::DragFloat3("Position", &position.x, 0.01f),
                 "Move instance", ChangeKind::kParameters,
                 [&](scene::SceneData& scene) {
                   edit_transform(scene).position = position;
                 });

  if (euler_instance_ != id || !euler_active_) {
    euler_degrees_ = glm::degrees(glm::eulerAngles(transform.rotation));
    euler_instance_ = id;
  }
  const bool rotated = ImGui::DragFloat3("Rotation", &euler_degrees_.x, 0.5f,
                                         0.0f, 0.0f, "%.1f deg");
  euler_active_ = ImGui::IsItemActive();
  EditFromWidget(doc, rotated, "Rotate instance", ChangeKind::kParameters,
                 [&](scene::SceneData& scene) {
                   edit_transform(scene).rotation =
                       glm::quat(glm::radians(euler_degrees_));
                 });

  glm::vec3 scale = transform.scale;
  EditFromWidget(
      doc, ImGui::DragFloat3("Scale", &scale.x, 0.01f), "Scale instance",
      ChangeKind::kParameters,
      [&](scene::SceneData& scene) { edit_transform(scene).scale = scale; });
  if (ImGui::SmallButton("Reset transform")) {
    doc.Edit("Reset transform", ChangeKind::kParameters,
             [&](scene::SceneData& scene) {
               edit_transform(scene) = scene::Transform{};
             });
  }
}

void SceneInspectorPanel::DrawMaterialOverrides(SceneDocument& doc,
                                                scene::InstanceId id) {
  const scene::Instance* instance = scene::FindInstance(doc.state(), id);
  const auto prefab = static_cast<size_t>(instance->prefab);
  const std::string prefab_path = doc.state().prefabs[prefab];
  const std::span<const std::vector<std::string>> all_names =
      state_.material_names();
  if (prefab >= all_names.size() || all_names[prefab].empty()) {
    ImGui::TextDisabled("The prefab has no materials (or is not loaded).");
  } else {
    for (const std::string& material_name : all_names[prefab]) {
      DrawMaterialOverride(doc, id, prefab_path, material_name);
    }
  }
  // Overrides of names the prefab does not have (shown in Problems too).
  for (const scene::MaterialOverride& material : instance->materials) {
    if (prefab < all_names.size() &&
        std::ranges::find(all_names[prefab], material.material_name) ==
            all_names[prefab].end()) {
      ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                         "Unknown material '%s'",
                         material.material_name.c_str());
      ImGui::SameLine();
      ImGui::PushID(material.material_name.c_str());
      if (ImGui::SmallButton("Remove")) {
        const std::string name = material.material_name;
        doc.Edit("Remove override", ChangeKind::kStructure,
                 [&](scene::SceneData& scene) {
                   RemoveMaterialOverride(scene, id, name);
                 });
        ImGui::PopID();
        return;  // `instance` changed.
      }
      ImGui::PopID();
    }
  }
}

void SceneInspectorPanel::DrawMaterialOverride(
    SceneDocument& doc, scene::InstanceId id, const std::string& prefab_path,
    const std::string& material_name) {
  const scene::MaterialOverride* material =
      scene::FindMaterialOverride(doc.state(), id, material_name);
  const std::string header =
      std::format("{}{}###{}", material_name,
                  material != nullptr ? "  (overridden)" : "", material_name);
  if (!ImGui::TreeNode(header.c_str())) {
    return;
  }
  DrawReplacement(doc, id, material_name);
  DrawTweaks(doc, id, prefab_path, material_name);
  if (scene::FindMaterialOverride(doc.state(), id, material_name) != nullptr &&
      ImGui::SmallButton("Remove override")) {
    doc.Edit("Remove override", ChangeKind::kStructure,
             [&](scene::SceneData& scene) {
               RemoveMaterialOverride(scene, id, material_name);
             });
  }
  ImGui::TreePop();
}

void SceneInspectorPanel::DrawReplacement(SceneDocument& doc,
                                          scene::InstanceId id,
                                          const std::string& material_name) {
  const scene::SceneData& scene = doc.state();
  const scene::MaterialOverride* material =
      scene::FindMaterialOverride(scene, id, material_name);
  const bool replaced =
      material != nullptr && material->source_prefab != scene::kNoPrefab;
  std::string library;
  if (pending_library_material_ == material_name) {
    library = pending_library_path_;
  } else if (replaced) {
    library = scene.prefabs[static_cast<size_t>(material->source_prefab)];
  }

  if (replaced) {
    ImGui::Text("Replaced by '%s'", material->source_material.c_str());
    ImGui::TextDisabled("from %s", core::FileName(library).c_str());
  } else {
    ImGui::TextDisabled("Own material");
  }
  if (ImGui::SmallButton("Library...")) {
    SceneInspectorPanel* self = this;
    state_.context().dialogs().Open(
        FileKind::kPrefab, state_.context().asset_root(),
        [self, material_name](std::string path) {
          self->pending_library_material_ = material_name;
          self->pending_library_path_ = std::move(path);
        });
  }
  ImGui::SetItemTooltip("Choose a material library (.pbo), or drop one here");
  if (const std::optional<std::string> dropped =
          AcceptAssetDrop(FileKind::kPrefab)) {
    pending_library_material_ = material_name;
    pending_library_path_ = *dropped;
    library = *dropped;
  }
  if (replaced) {
    ImGui::SameLine();
    if (ImGui::SmallButton("Use own material")) {
      doc.Edit("Clear replacement", ChangeKind::kStructure,
               [&](scene::SceneData& edited) {
                 SetMaterialReplacement(edited, id, material_name, "", "");
                 scene::Instance* instance = scene::FindInstance(edited, id);
                 if (const scene::MaterialOverride* left =
                         scene::FindMaterialOverride(*instance, material_name);
                     left != nullptr && IsEmptyOverride(*left)) {
                   RemoveMaterialOverride(edited, id, material_name);
                 }
               });
      return;
    }
  }
  if (library.empty()) {
    return;
  }
  const std::vector<std::string> names = state_.PrefabMaterialNames(library);
  const char* preview = replaced ? material->source_material.c_str() : "";
  if (ImGui::BeginCombo("Replacement", preview)) {
    for (const std::string& name : names) {
      if (ImGui::Selectable(name.c_str(),
                            replaced && name == material->source_material)) {
        doc.Edit("Replace material", ChangeKind::kStructure,
                 [&](scene::SceneData& edited) {
                   SetMaterialReplacement(edited, id, material_name, library,
                                          name);
                 });
        pending_library_material_.clear();
      }
    }
    if (names.empty()) {
      ImGui::TextDisabled("No materials in this file.");
    }
    ImGui::EndCombo();
  }
}

void SceneInspectorPanel::DrawTweaks(SceneDocument& doc, scene::InstanceId id,
                                     const std::string& prefab_path,
                                     const std::string& material_name) {
  const scene::MaterialOverride* material =
      scene::FindMaterialOverride(doc.state(), id, material_name);
  // Defaults for newly enabled tweaks: the prefab material's own values.
  const prefab::Material base =
      state_.FindPrefabMaterial(prefab_path, material_name)
          .value_or(prefab::Material{});
  const auto override_of =
      [&](scene::SceneData& scene) -> scene::MaterialOverride& {
    return FindOrAddMaterialOverride(*scene::FindInstance(scene, id),
                                     material_name);
  };
  // Enabling a tweak is one step; dragging its value is another.
  const auto toggle = [&](const char* label, bool enabled, auto&& set) {
    bool checked = enabled;
    if (ImGui::Checkbox(label, &checked)) {
      doc.Edit(checked ? "Add material tweak" : "Remove material tweak",
               ChangeKind::kStructure, [&](scene::SceneData& scene) {
                 set(override_of(scene), checked);
                 if (IsEmptyOverride(override_of(scene))) {
                   RemoveMaterialOverride(scene, id, material_name);
                 }
               });
    }
  };
  const auto has = [&](const auto member) {
    return material != nullptr && (material->*member).has_value();
  };

  ImGui::PushID("tweaks");
  // Base colour.
  toggle("##base_color_on", has(&scene::MaterialOverride::base_color),
         [&](scene::MaterialOverride& o, bool on) {
           o.base_color = on ? std::optional(base.base_color) : std::nullopt;
         });
  ImGui::SameLine();
  if (has(&scene::MaterialOverride::base_color)) {
    glm::vec4 color = *material->base_color;
    EditFromWidget(doc, ImGui::ColorEdit4("Base colour", &color.x),
                   "Material colour", ChangeKind::kStructure,
                   [&](scene::SceneData& scene) {
                     override_of(scene).base_color = color;
                   });
  } else {
    ImGui::TextDisabled("Base colour");
  }

  const auto scalar =
      [&](const char* label, const char* on_id,
          std::optional<float> scene::MaterialOverride::* member,
          float base_value, float min, float max) {
        toggle(on_id, has(member), [&](scene::MaterialOverride& o, bool on) {
          o.*member = on ? std::optional(base_value) : std::nullopt;
        });
        ImGui::SameLine();
        if (has(member)) {
          float value = *(material->*member);
          EditFromWidget(doc, ImGui::SliderFloat(label, &value, min, max),
                         label, ChangeKind::kStructure,
                         [&](scene::SceneData& scene) {
                           override_of(scene).*member = value;
                         });
        } else {
          ImGui::TextDisabled("%s", label);
        }
      };
  scalar("Metallic", "##metallic_on", &scene::MaterialOverride::metallic,
         base.metallic, 0.0f, 1.0f);
  scalar("Roughness", "##roughness_on", &scene::MaterialOverride::roughness,
         base.roughness, 0.0f, 1.0f);

  toggle("##emissive_on", has(&scene::MaterialOverride::emissive),
         [&](scene::MaterialOverride& o, bool on) {
           o.emissive = on ? std::optional(base.emissive) : std::nullopt;
         });
  ImGui::SameLine();
  if (has(&scene::MaterialOverride::emissive)) {
    glm::vec3 emissive = *material->emissive;
    EditFromWidget(
        doc,
        ImGui::ColorEdit3("Emissive", &emissive.x,
                          ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float),
        "Emissive", ChangeKind::kStructure, [&](scene::SceneData& scene) {
          override_of(scene).emissive = glm::max(emissive, 0.0f);
        });
  } else {
    ImGui::TextDisabled("Emissive");
  }
  scalar("Emissive strength", "##emissive_strength_on",
         &scene::MaterialOverride::emissive_strength, base.emissive_strength,
         0.0f, 50.0f);

  toggle("##uv_scale_on", has(&scene::MaterialOverride::uv_scale),
         [&](scene::MaterialOverride& o, bool on) {
           o.uv_scale = on ? std::optional(base.uv_scale) : std::nullopt;
         });
  ImGui::SameLine();
  if (has(&scene::MaterialOverride::uv_scale)) {
    glm::vec2 uv_scale = *material->uv_scale;
    EditFromWidget(
        doc, ImGui::DragFloat2("UV scale", &uv_scale.x, 0.01f, 0.001f, 1000.0f),
        "UV scale", ChangeKind::kStructure, [&](scene::SceneData& scene) {
          override_of(scene).uv_scale = glm::max(uv_scale, 0.001f);
        });
  } else {
    ImGui::TextDisabled("UV scale");
  }
  ImGui::PopID();
  if (doc.history().edit_open()) {
    ImGui::TextDisabled("Material changes show when the drag ends.");
  }
}

void SceneInspectorPanel::DrawPhysicsOverride(SceneDocument& doc,
                                              scene::InstanceId id) {
  const scene::Instance* instance = scene::FindInstance(doc.state(), id);
  bool overridden = instance->physics.has_value();
  if (ImGui::Checkbox("Override physics", &overridden)) {
    doc.Edit("Physics override", ChangeKind::kStructure,
             [&](scene::SceneData& scene) {
               scene::Instance* edited = scene::FindInstance(scene, id);
               edited->physics = overridden
                                     ? std::optional(scene::PhysicsOverride{})
                                     : std::nullopt;
             });
    return;
  }
  if (!instance->physics.has_value()) {
    ImGui::TextDisabled("The prefab's physics settings apply.");
    return;
  }
  const scene::PhysicsOverride& physics = *instance->physics;
  const auto edit = [&](scene::SceneData& scene) -> scene::PhysicsOverride& {
    return *scene::FindInstance(scene, id)->physics;
  };

  bool disabled = physics.disabled;
  if (ImGui::Checkbox("No body", &disabled)) {
    doc.Edit("Physics: no body", ChangeKind::kStructure,
             [&](scene::SceneData& scene) { edit(scene).disabled = disabled; });
  }
  int motion = !physics.motion.has_value()
                   ? 0
                   : (*physics.motion == scene::MotionType::kStatic ? 1 : 2);
  if (ImGui::Combo("Motion", &motion, "From prefab\0Static\0Dynamic\0")) {
    doc.Edit("Physics: motion", ChangeKind::kStructure,
             [&](scene::SceneData& scene) {
               edit(scene).motion =
                   motion == 0
                       ? std::nullopt
                       : std::optional(motion == 1
                                           ? scene::MotionType::kStatic
                                           : scene::MotionType::kDynamic);
             });
  }
  const auto scalar = [&](const char* label,
                          std::optional<float> scene::PhysicsOverride::* member,
                          float initial, float min, float max) {
    ImGui::PushID(label);
    bool on = (physics.*member).has_value();
    if (ImGui::Checkbox("##on", &on)) {
      doc.Edit(label, ChangeKind::kStructure, [&](scene::SceneData& scene) {
        edit(scene).*member = on ? std::optional(initial) : std::nullopt;
      });
    }
    ImGui::SameLine();
    if ((physics.*member).has_value()) {
      float value = *(physics.*member);
      EditFromWidget(doc, ImGui::DragFloat(label, &value, 0.01f, min, max),
                     label, ChangeKind::kStructure,
                     [&](scene::SceneData& scene) {
                       edit(scene).*member = std::clamp(value, min, max);
                     });
    } else {
      ImGui::TextDisabled("%s (from prefab)", label);
    }
    ImGui::PopID();
  };
  scalar("Mass (kg)", &scene::PhysicsOverride::mass, 10.0f, 0.001f, 1.0e6f);
  scalar("Friction", &scene::PhysicsOverride::friction, 0.6f, 0.0f, 10.0f);
  scalar("Restitution", &scene::PhysicsOverride::restitution, 0.1f, 0.0f, 1.0f);
}

}  // namespace

std::unique_ptr<Panel> MakeSceneInspectorPanel(SceneModeState& state) {
  return std::make_unique<SceneInspectorPanel>(state);
}

}  // namespace blue::editor
