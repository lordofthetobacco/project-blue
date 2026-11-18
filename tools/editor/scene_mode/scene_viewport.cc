// Scene mode's viewport client: the world's draws and lights, the gizmo on
// the primary selection (instances, point lights, player start), picking,
// prefab drops and the debug draws of the scene.

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>
#include <glm/vec3.hpp>

#include "asset/mesh_data.h"
#include "editor/dialogs.h"
#include "editor/drag_drop.h"
#include "editor/scene_mode/scene_mode.h"
#include "editor/selection.h"
#include "editor/viewport/debug_draw.h"
#include "editor/viewport/editor_camera.h"
#include "editor/viewport/gizmo.h"
#include "editor/viewport/viewport_panel.h"
#include "render/scene_view.h"
#include "scene/scene_data.h"
#include "world/world.h"

namespace blue::editor {
namespace {

constexpr float kPickDistance = 10000.0f;
// A click within this distance (m) of a light or the player start selects it.
constexpr float kIconPickRadius = 0.35f;
// Gizmo ids of the non-instance objects (instances use their id).
constexpr int kPointLightGizmoBase = 1 << 28;
constexpr int kPlayerStartGizmo = (1 << 28) - 1;

// Returns the distance along `ray` to its closest approach to `point` if
// that approach is within `radius`, else nullopt.
std::optional<float> RayPointDistance(const Ray& ray, const glm::vec3& point,
                                      float radius) {
  const float along = glm::dot(point - ray.origin, ray.direction);
  if (along < 0.0f) {
    return std::nullopt;
  }
  const glm::vec3 closest = ray.origin + ray.direction * along;
  if (glm::length(point - closest) > radius) {
    return std::nullopt;
  }
  return along;
}

void Grow(std::optional<asset::Bounds>& total, const asset::Bounds& bounds) {
  if (!total.has_value()) {
    total = bounds;
    return;
  }
  total->min = glm::min(total->min, bounds.min);
  total->max = glm::max(total->max, bounds.max);
}

asset::Bounds PointBounds(const glm::vec3& point, float radius) {
  return {point - glm::vec3(radius), point + glm::vec3(radius)};
}

// Returns the yaw (radians about +Y, 0 = -Z, positive turns left) of the
// forward axis (-Z) of `matrix`.
float YawOf(const glm::mat4& matrix) {
  const glm::vec3 forward = -glm::vec3(matrix[2]);
  return std::atan2(-forward.x, -forward.z);
}

class SceneViewportClient final : public ViewportClient {
 public:
  explicit SceneViewportClient(SceneModeState& state) : state_(state) {}

  void DrawOverlay(EditorContext& context, ViewportPanel& viewport) override;
  void Pick(EditorContext& context, ViewportPanel& viewport, const Ray& ray,
            bool additive) override;
  std::optional<asset::Bounds> FocusBounds(EditorContext& context) override;
  void BuildView(EditorContext& context, ViewportPanel& viewport,
                 render::SceneView& view) override;

 private:
  // Shows the gizmo for the primary selection and applies its edits.
  void DrawGizmo(SceneDocument& doc, ViewportPanel& viewport);
  // Accepts prefabs dropped onto the viewport image.
  void AcceptDrop(ViewportPanel& viewport);
  void AddDebugDraws(SceneDocument& doc, ViewportPanel& viewport);

  SceneModeState& state_;
  // Frame storage handed to the SceneView (kept between frames, so warm
  // frames do not allocate).
  std::vector<render::DrawItem> draws_;
  std::vector<uint32_t> selected_draws_;
  std::vector<render::PointLight> lights_;
  std::vector<scene::InstanceId> selected_ids_;
};

void SceneViewportClient::DrawOverlay(EditorContext& /*context*/,
                                      ViewportPanel& viewport) {
  SceneDocument* doc = state_.document();
  if (doc == nullptr) {
    return;
  }
  AcceptDrop(viewport);
  DrawGizmo(*doc, viewport);
  if (viewport.toggles().lights) {
    const scene::SceneData& scene = doc->state();
    for (const scene::ScenePointLight& light : scene.point_lights) {
      viewport.AddLabel(light.position + glm::vec3(0.0f, 0.25f, 0.0f),
                        light.name.empty() ? "light" : light.name);
    }
    viewport.AddLabel(scene.player_start.position + glm::vec3(0.0f, 2.0f, 0.0f),
                      "player start");
  }
}

void SceneViewportClient::DrawGizmo(SceneDocument& doc,
                                    ViewportPanel& viewport) {
  const std::optional<SelectionItem> primary = doc.selection().primary();
  if (!primary.has_value()) {
    return;
  }
  const scene::SceneData& scene = doc.state();
  switch (primary->kind) {
    case SelectionKind::kInstance: {
      const auto id = static_cast<scene::InstanceId>(primary->index);
      const scene::Instance* instance = scene::FindInstance(scene, id);
      const std::optional<glm::mat4> world = scene::WorldMatrix(scene, id);
      if (instance == nullptr || !world.has_value()) {
        return;
      }
      std::optional<glm::mat4> parent;
      if (instance->parent != scene::kNoInstance) {
        parent = scene::WorldMatrix(scene, instance->parent);
      }
      glm::mat4 matrix = *world;
      const GizmoEvent event = viewport.Manipulate(matrix, static_cast<int>(id),
                                                   parent ? &*parent : nullptr);
      if (event == GizmoEvent::kNone) {
        return;
      }
      if (event != GizmoEvent::kEnd || matrix != *world) {
        const glm::mat4 parent_world = parent.value_or(glm::mat4(1.0f));
        scene::FindInstance(
            doc.BeginEdit("Move instance", ChangeKind::kParameters), id)
            ->transform =
            scene::TransformFromMatrix(glm::inverse(parent_world) * matrix);
        // Show the move this frame (Sync() ran before the viewport drew).
        if (world::World* runtime = state_.world()) {
          runtime->SetInstanceTransform(doc.state(), id);
        }
      }
      if (event == GizmoEvent::kEnd) {
        doc.CommitEdit();
      }
      return;
    }
    case SelectionKind::kScenePointLight: {
      const auto index = static_cast<size_t>(primary->index);
      if (primary->index < 0 || index >= scene.point_lights.size()) {
        return;
      }
      glm::mat4 matrix =
          glm::translate(glm::mat4(1.0f), scene.point_lights[index].position);
      const GizmoEvent event = viewport.Manipulate(
          matrix, kPointLightGizmoBase + static_cast<int>(index));
      if (event == GizmoEvent::kNone) {
        return;
      }
      // Lights are not part of the world's per-instance refresh: the light
      // list is patched per frame in BuildView(), so kParameters suffices.
      doc.BeginEdit("Move light", ChangeKind::kParameters)
          .point_lights[index]
          .position = glm::vec3(matrix[3]);
      if (event == GizmoEvent::kEnd) {
        doc.CommitEdit();
      }
      return;
    }
    case SelectionKind::kPlayerStart: {
      const scene::PlayerStart& start = scene.player_start;
      glm::mat4 matrix =
          glm::rotate(glm::translate(glm::mat4(1.0f), start.position),
                      start.yaw_radians, glm::vec3(0.0f, 1.0f, 0.0f));
      const GizmoEvent event = viewport.Manipulate(matrix, kPlayerStartGizmo);
      if (event == GizmoEvent::kNone) {
        return;
      }
      scene::PlayerStart& edited =
          doc.BeginEdit("Move player start", ChangeKind::kParameters)
              .player_start;
      edited.position = glm::vec3(matrix[3]);
      edited.yaw_radians = YawOf(matrix);
      if (event == GizmoEvent::kEnd) {
        doc.CommitEdit();
      }
      return;
    }
    default:
      return;
  }
}

void SceneViewportClient::AcceptDrop(ViewportPanel& viewport) {
  const ImVec2 min = viewport.image_min();
  const ImVec2 size = viewport.image_size();
  const ImRect rect(min, ImVec2(min.x + size.x, min.y + size.y));
  if (!ImGui::BeginDragDropTargetCustom(rect,
                                        ImGui::GetID("scene_prefab_drop"))) {
    return;
  }
  // Only .pbo files are accepted; others do not highlight the target.
  const ImGuiPayload* peek = ImGui::GetDragDropPayload();
  if (peek != nullptr && peek->IsDataType(kAssetPathPayload) &&
      MatchesFileKind(static_cast<const char*>(peek->Data),
                      FileKind::kPrefab)) {
    if (const ImGuiPayload* payload =
            ImGui::AcceptDragDropPayload(kAssetPathPayload)) {
      const std::string path(static_cast<const char*>(payload->Data));
      state_.PlaceInstance(path, state_.DropPoint(ImGui::GetMousePos()));
    }
  }
  ImGui::EndDragDropTarget();
}

void SceneViewportClient::Pick(EditorContext& /*context*/,
                               ViewportPanel& /*viewport*/, const Ray& ray,
                               bool additive) {
  SceneDocument* doc = state_.document();
  if (doc == nullptr) {
    return;
  }
  const scene::SceneData& scene = doc->state();
  std::optional<SelectionItem> picked;
  float nearest = kPickDistance;
  if (world::World* world = state_.world()) {
    if (const auto hit = world->Pick(ray.origin, ray.direction, kPickDistance,
                                     world::PickMode::kTriangles)) {
      picked = InstanceItem(hit->instance);
      nearest = hit->distance;
    }
  }
  // Icons (lights, player start) in front of the geometry win.
  for (size_t i = 0; i < scene.point_lights.size(); ++i) {
    const std::optional<float> distance =
        RayPointDistance(ray, scene.point_lights[i].position, kIconPickRadius);
    if (distance.has_value() && *distance < nearest) {
      nearest = *distance;
      picked = SelectionItem{SelectionKind::kScenePointLight,
                             static_cast<int64_t>(i)};
    }
  }
  const std::optional<float> start_distance = RayPointDistance(
      ray, scene.player_start.position + glm::vec3(0.0f, 0.9f, 0.0f),
      kIconPickRadius * 2.0f);
  if (start_distance.has_value() && *start_distance < nearest) {
    picked = SelectionItem{SelectionKind::kPlayerStart, -1};
  }

  Selection& selection = doc->selection();
  if (!picked.has_value()) {
    if (!additive) {
      selection.Clear();
    }
    return;
  }
  if (additive) {
    selection.Toggle(*picked);
  } else {
    selection.Set(*picked);
  }
}

std::optional<asset::Bounds> SceneViewportClient::FocusBounds(
    EditorContext& /*context*/) {
  SceneDocument* doc = state_.document();
  world::World* world = state_.world();
  if (doc == nullptr || world == nullptr) {
    return std::nullopt;
  }
  const scene::SceneData& scene = doc->state();
  std::optional<asset::Bounds> total;
  const auto add_item = [&](const SelectionItem& item) {
    switch (item.kind) {
      case SelectionKind::kInstance: {
        const auto id = static_cast<scene::InstanceId>(item.index);
        if (auto bounds = world->InstanceBounds(id)) {
          Grow(total, *bounds);
        } else if (auto matrix = scene::WorldMatrix(scene, id)) {
          Grow(total, PointBounds(glm::vec3((*matrix)[3]), 0.5f));
        }
        break;
      }
      case SelectionKind::kScenePointLight:
        if (item.index >= 0 &&
            static_cast<size_t>(item.index) < scene.point_lights.size()) {
          Grow(total,
               PointBounds(
                   scene.point_lights[static_cast<size_t>(item.index)].position,
                   1.0f));
        }
        break;
      case SelectionKind::kPlayerStart:
        Grow(total, PointBounds(scene.player_start.position, 1.0f));
        break;
      default:
        break;
    }
  };
  if (!doc->selection().empty()) {
    for (const SelectionItem& item : doc->selection().items()) {
      add_item(item);
    }
    return total;
  }
  for (const scene::Instance& instance : scene.instances) {
    add_item(InstanceItem(instance.id));
  }
  return total;
}

void SceneViewportClient::AddDebugDraws(SceneDocument& doc,
                                        ViewportPanel& viewport) {
  const scene::SceneData& scene = doc.state();
  render::DebugLines& lines = viewport.lines();
  const ViewportToggles& toggles = viewport.toggles();
  const Selection& selection = doc.selection();
  if (toggles.lights) {
    for (size_t i = 0; i < scene.point_lights.size(); ++i) {
      const scene::ScenePointLight& light = scene.point_lights[i];
      const bool selected = selection.Contains(
          {SelectionKind::kScenePointLight, static_cast<int64_t>(i)});
      AddPointLight(lines, light.position, light.range, selected);
    }
    const glm::vec3 anchor =
        viewport.camera().target() + glm::vec3(0.0f, 1.0f, 0.0f);
    if (selection.Contains({SelectionKind::kSun, -1})) {
      AddSunArrow(lines, anchor, scene.sun.direction);
    }
  }
  AddPlayerStart(lines, scene.player_start.position,
                 scene.player_start.yaw_radians);

  world::World* world = state_.world();
  if (world == nullptr) {
    return;
  }
  if (toggles.bounds) {
    for (const scene::InstanceId id : selected_ids_) {
      if (const auto bounds = world->InstanceBounds(id)) {
        AddBounds(lines, *bounds, glm::mat4(1.0f));
      }
    }
  }
  if (toggles.colliders) {
    const world::PrefabCache& cache = state_.context().prefab_cache();
    for (const world::Placement& placement : world->placements()) {
      const prefab::Prefab& prefab = cache.prefab(placement.prefab);
      AddCollider(lines, prefab.physics, placement.world, prefab.bounds);
    }
  }
}

void SceneViewportClient::BuildView(EditorContext& /*context*/,
                                    ViewportPanel& viewport,
                                    render::SceneView& view) {
  draws_.clear();
  selected_draws_.clear();
  lights_.clear();
  SceneDocument* doc = state_.document();
  world::World* world = state_.world();
  if (doc == nullptr || world == nullptr) {
    return;
  }
  const scene::SceneData& scene = doc->state();
  selected_ids_ = doc->selection().Instances();

  world->AppendDraws(draws_);
  world->AppendSelectedDraws(selected_ids_, 0, selected_draws_);

  // Scene lights come first in the world's list; take their current values
  // so light edits show while dragging, before the next rebuild.
  const std::span<const render::PointLight> built = world->point_lights();
  lights_.assign(built.begin(), built.end());
  if (state_.built_scene_light_count() == scene.point_lights.size()) {
    for (size_t i = 0; i < scene.point_lights.size() && i < lights_.size();
         ++i) {
      const scene::ScenePointLight& light = scene.point_lights[i];
      lights_[i] = {light.position, light.color, light.intensity, light.range};
    }
  }

  view.draws = draws_;
  view.selected_draws = selected_draws_;
  view.point_lights = lights_;
  view.sun = scene.sun;
  view.shadows_enabled = viewport.toggles().shadows && scene.sun.casts_shadows;
  view.post.exposure_ev = scene.environment.exposure_ev;
  AddDebugDraws(*doc, viewport);
}

}  // namespace

std::unique_ptr<ViewportClient> MakeSceneViewportClient(SceneModeState& state) {
  return std::make_unique<SceneViewportClient>(state);
}

}  // namespace blue::editor
