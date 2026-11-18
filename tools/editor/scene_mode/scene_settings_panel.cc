// Scene mode's Scene Settings panel (sun, environment, gravity, player
// start, point lights) and Problems panel (world build issues, override
// issues, save validation). The settings widgets are also what the
// inspector shows for those selection kinds.

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

#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>

#include "core/path.h"
#include "editor/dialogs.h"
#include "editor/drag_drop.h"
#include "editor/editor_context.h"
#include "editor/panel.h"
#include "editor/scene_mode/scene_mode.h"
#include "editor/selection.h"
#include "scene/mscn_serializer.h"
#include "scene/scene.h"
#include "scene/scene_data.h"

namespace blue::editor {
namespace {

std::array<char, 256> TextBuffer(std::string_view text) {
  std::array<char, 256> buffer{};
  const size_t length = std::min(text.size(), buffer.size() - 1);
  std::memcpy(buffer.data(), text.data(), length);
  return buffer;
}

// Settings that change no instance: the world is not refreshed for them
// (BuildView reads them from the document each frame).
constexpr ChangeKind kSetting = ChangeKind::kParameters;

}  // namespace

void DrawSunSettings(SceneModeState& state) {
  SceneDocument* doc = state.document();
  if (doc == nullptr) {
    return;
  }
  const scene::SunLight& sun = doc->state().sun;
  const scene::SunAngles angles = scene::SunAnglesFromDirection(sun.direction);
  float elevation = glm::degrees(angles.elevation);
  float azimuth = glm::degrees(angles.azimuth);
  const bool elevation_changed =
      ImGui::SliderFloat("Elevation", &elevation, -10.0f, 90.0f, "%.1f deg");
  EditFromWidget(*doc, elevation_changed, "Sun elevation", kSetting,
                 [&](scene::SceneData& scene) {
                   scene.sun.direction = scene::SunDirectionFromAngles(
                       {glm::radians(elevation), angles.azimuth});
                 });
  const bool azimuth_changed =
      ImGui::SliderFloat("Azimuth", &azimuth, -180.0f, 180.0f, "%.1f deg");
  EditFromWidget(*doc, azimuth_changed, "Sun azimuth", kSetting,
                 [&](scene::SceneData& scene) {
                   scene.sun.direction = scene::SunDirectionFromAngles(
                       {angles.elevation, glm::radians(azimuth)});
                 });
  glm::vec3 color = sun.color;
  EditFromWidget(*doc, ImGui::ColorEdit3("Colour##sun", &color.x), "Sun colour",
                 kSetting,
                 [&](scene::SceneData& scene) { scene.sun.color = color; });
  float intensity = sun.intensity;
  EditFromWidget(
      *doc, ImGui::DragFloat("Intensity##sun", &intensity, 0.05f, 0.0f, 100.0f),
      "Sun intensity", kSetting, [&](scene::SceneData& scene) {
        scene.sun.intensity = std::max(intensity, 0.0f);
      });
  bool shadows = sun.casts_shadows;
  if (ImGui::Checkbox("Casts shadows", &shadows)) {
    doc->Edit("Sun shadows", kSetting, [&](scene::SceneData& scene) {
      scene.sun.casts_shadows = shadows;
    });
  }
  if (ImGui::Button("Align sun to HDRI")) {
    if (auto aligned = state.AlignSunToHdri(); !aligned.ok()) {
      state.context().Status("Align sun: " + aligned.error());
    } else {
      state.context().Status("Sun aligned to the HDRI's brightest region");
    }
  }
}

void DrawEnvironmentSettings(SceneModeState& state) {
  SceneDocument* doc = state.document();
  if (doc == nullptr) {
    return;
  }
  const scene::Environment& environment = doc->state().environment;
  ImGui::TextWrapped("HDRI: %s", environment.hdri_path.empty()
                                     ? "(default sky)"
                                     : environment.hdri_path.c_str());
  if (const std::optional<std::string> dropped =
          AcceptAssetDrop(FileKind::kHdr)) {
    const std::string path = *dropped;
    doc->Edit("Set HDRI", kSetting, [&](scene::SceneData& scene) {
      scene.environment.hdri_path = path;
    });
  }
  if (ImGui::Button("Browse...##hdri")) {
    SceneModeState* captured = &state;
    state.context().dialogs().Open(
        FileKind::kHdr, state.context().asset_root(),
        [captured](std::string path) {
          captured->Edit("Set HDRI", kSetting, [&](scene::SceneData& scene) {
            scene.environment.hdri_path = path;
          });
        });
  }
  ImGui::SameLine();
  if (ImGui::Button("Default sky")) {
    doc->Edit("Default sky", kSetting, [](scene::SceneData& scene) {
      scene.environment.hdri_path.clear();
    });
  }
  float intensity = environment.intensity;
  EditFromWidget(
      *doc, ImGui::DragFloat("Intensity##env", &intensity, 0.01f, 0.0f, 20.0f),
      "Environment intensity", kSetting, [&](scene::SceneData& scene) {
        scene.environment.intensity = std::max(intensity, 0.0f);
      });
  float rotation = environment.rotation_radians;
  EditFromWidget(
      *doc, ImGui::SliderAngle("Rotation", &rotation, -180.0f, 180.0f),
      "Environment rotation", kSetting, [&](scene::SceneData& scene) {
        scene.environment.rotation_radians = rotation;
      });
  float exposure = environment.exposure_ev;
  EditFromWidget(
      *doc,
      ImGui::DragFloat("Exposure", &exposure, 0.05f, -10.0f, 10.0f, "%.2f EV"),
      "Exposure", kSetting, [&](scene::SceneData& scene) {
        scene.environment.exposure_ev = exposure;
      });
}

void DrawPlayerStartSettings(SceneModeState& state) {
  SceneDocument* doc = state.document();
  if (doc == nullptr) {
    return;
  }
  const scene::PlayerStart& start = doc->state().player_start;
  glm::vec3 position = start.position;
  EditFromWidget(*doc, ImGui::DragFloat3("Position##start", &position.x, 0.01f),
                 "Move player start", kSetting, [&](scene::SceneData& scene) {
                   scene.player_start.position = position;
                 });
  float yaw = start.yaw_radians;
  EditFromWidget(*doc, ImGui::SliderAngle("Yaw", &yaw, -180.0f, 180.0f),
                 "Player start yaw", kSetting, [&](scene::SceneData& scene) {
                   scene.player_start.yaw_radians = yaw;
                 });
  float pitch = start.pitch_radians;
  EditFromWidget(*doc, ImGui::SliderAngle("Pitch", &pitch, -89.0f, 89.0f),
                 "Player start pitch", kSetting, [&](scene::SceneData& scene) {
                   scene.player_start.pitch_radians = pitch;
                 });
  if (ImGui::SmallButton("Select for gizmo")) {
    doc->selection().Set({SelectionKind::kPlayerStart, -1});
  }
}

void DrawPointLightSettings(SceneModeState& state, size_t index) {
  SceneDocument* doc = state.document();
  if (doc == nullptr || index >= doc->state().point_lights.size()) {
    return;
  }
  const scene::ScenePointLight& light = doc->state().point_lights[index];
  ImGui::PushID(static_cast<int>(index));
  std::array<char, 256> name = TextBuffer(light.name);
  ImGui::InputText("Name##light", name.data(), name.size());
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    const std::string renamed(name.data());
    doc->Edit("Rename light", kSetting, [&](scene::SceneData& scene) {
      scene.point_lights[index].name = renamed;
    });
  }
  const auto edit = [&](scene::SceneData& scene) -> scene::ScenePointLight& {
    return scene.point_lights[index];
  };
  glm::vec3 position = light.position;
  EditFromWidget(*doc, ImGui::DragFloat3("Position##light", &position.x, 0.01f),
                 "Move light", kSetting, [&](scene::SceneData& scene) {
                   edit(scene).position = position;
                 });
  glm::vec3 color = light.color;
  EditFromWidget(*doc, ImGui::ColorEdit3("Colour##light", &color.x),
                 "Light colour", kSetting,
                 [&](scene::SceneData& scene) { edit(scene).color = color; });
  float intensity = light.intensity;
  EditFromWidget(
      *doc,
      ImGui::DragFloat("Intensity##light", &intensity, 0.1f, 0.0f, 10000.0f),
      "Light intensity", kSetting, [&](scene::SceneData& scene) {
        edit(scene).intensity = std::max(intensity, 0.0f);
      });
  float range = light.range;
  EditFromWidget(*doc,
                 ImGui::DragFloat("Range (m)", &range, 0.05f, 0.01f, 1000.0f),
                 "Light range", kSetting, [&](scene::SceneData& scene) {
                   edit(scene).range = std::max(range, 0.01f);
                 });
  if (ImGui::Button("Delete light")) {
    doc->Edit("Delete light", ChangeKind::kStructure,
              [&](scene::SceneData& scene) {
                scene.point_lights.erase(scene.point_lights.begin() +
                                         static_cast<std::ptrdiff_t>(index));
              });
    doc->selection().Clear();
  }
  ImGui::PopID();
}

namespace {

class SceneSettingsPanel final : public Panel {
 public:
  explicit SceneSettingsPanel(SceneModeState& state) : state_(state) {}

  void Draw(EditorContext& /*context*/) override {
    SceneDocument* doc = state_.document();
    if (doc == nullptr) {
      ImGui::TextDisabled("No scene open.");
      return;
    }
    if (ImGui::CollapsingHeader("Sun", ImGuiTreeNodeFlags_DefaultOpen)) {
      DrawSunSettings(state_);
    }
    if (ImGui::CollapsingHeader("Environment",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
      DrawEnvironmentSettings(state_);
    }
    if (ImGui::CollapsingHeader("Physics")) {
      glm::vec3 gravity = doc->state().gravity;
      EditFromWidget(*doc,
                     ImGui::DragFloat3("Gravity (m/s2)", &gravity.x, 0.01f),
                     "Gravity", kSetting,
                     [&](scene::SceneData& scene) { scene.gravity = gravity; });
    }
    if (ImGui::CollapsingHeader("Player start")) {
      DrawPlayerStartSettings(state_);
    }
    if (ImGui::CollapsingHeader("Point lights")) {
      const auto& lights = doc->state().point_lights;
      for (size_t i = 0; i < lights.size(); ++i) {
        const SelectionItem item{SelectionKind::kScenePointLight,
                                 static_cast<int64_t>(i)};
        const std::string label = std::format(
            "{}##pl{}", lights[i].name.empty() ? "(unnamed)" : lights[i].name,
            i);
        if (ImGui::Selectable(label.c_str(), doc->selection().Contains(item))) {
          doc->selection().Set(item);
        }
      }
      if (ImGui::Button("Add point light")) {
        state_.AddPointLight(state_.DropPoint(std::nullopt) +
                             glm::vec3(0.0f, 2.0f, 0.0f));
      }
    }
  }

 private:
  SceneModeState& state_;
};

class ProblemsPanel final : public Panel {
 public:
  explicit ProblemsPanel(SceneModeState& state) : state_(state) {}

  void Draw(EditorContext& /*context*/) override {
    SceneDocument* doc = state_.document();
    if (doc == nullptr) {
      ImGui::TextDisabled("No scene open.");
      return;
    }
    if (ImGui::SmallButton("Revalidate")) {
      state_.RequestRebuild();
    }
    ImGui::SetItemTooltip(
        "Reloads nothing; rebuilds the world from the "
        "cached prefabs and checks the overrides");
    // What a save would refuse (structural errors); checked once per
    // document revision (about 1 ms in Debug, too much for every frame).
    if (doc != checked_document_ || doc->revision() != checked_revision_) {
      const std::string path =
          doc->path().empty()
              ? core::JoinPath(state_.context().asset_root(), "untitled.mscn")
              : doc->path();
      unsavable_ = scene::CheckMscnSerializable(doc->state(), path);
      checked_document_ = doc;
      checked_revision_ = doc->revision();
    }
    const std::optional<std::string>& unsavable = unsavable_;
    if (unsavable.has_value()) {
      ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "Cannot save: %s",
                         unsavable->c_str());
    }
    const auto problems = state_.problems();
    if (problems.empty() && !unsavable.has_value()) {
      ImGui::TextDisabled("No problems.");
      return;
    }
    for (size_t i = 0; i < problems.size(); ++i) {
      const SceneModeState::Problem& problem = problems[i];
      const scene::Instance* instance =
          scene::FindInstance(doc->state(), problem.instance);
      const std::string label = std::format(
          "{}{}##problem{}",
          instance != nullptr ? "[" + instance->name + "] " : std::string(),
          problem.message, i);
      if (ImGui::Selectable(label.c_str()) && instance != nullptr) {
        doc->selection().Set(InstanceItem(problem.instance));
      }
    }
  }

 private:
  SceneModeState& state_;
  // The save check's result and the document state it belongs to.
  const SceneDocument* checked_document_ = nullptr;
  DocumentRevision checked_revision_;
  std::optional<std::string> unsavable_;
};

}  // namespace

std::unique_ptr<Panel> MakeSceneSettingsPanel(SceneModeState& state) {
  return std::make_unique<SceneSettingsPanel>(state);
}

std::unique_ptr<Panel> MakeProblemsPanel(SceneModeState& state) {
  return std::make_unique<ProblemsPanel>(state);
}

}  // namespace blue::editor
