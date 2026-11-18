#include "editor/scene_mode/scene_mode.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include "core/log.h"
#include "editor/editor_app.h"
#include "editor/modes.h"
#include "editor/panel.h"
#include "editor/prefab_mode/prefab_edit.h"
#include "editor/scene_mode/scene_edit.h"
#include "editor/selection.h"
#include "editor/viewport/editor_camera.h"
#include "editor/viewport/viewport_panel.h"
#include "lighting/environment.h"
#include "scene/scene_data.h"
#include "world/world.h"

namespace blue::editor {
namespace {

// Farthest a viewport click or drop considers geometry, m.
constexpr float kPickDistance = 10000.0f;

// A Scene mode sync slower than this is logged: steady frames take well
// under a millisecond; rebuilds after an edit about 1.5 ms (Debug).
constexpr double kSlowSyncMs = 20.0;

double MillisecondsSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - start)
      .count();
}

// How far a duplicate is moved so it does not sit inside the original.
constexpr glm::vec3 kDuplicateOffset{1.0f, 0.0f, 0.0f};

}  // namespace

SceneModeState::SceneModeState(EditorContext& context) : context_(context) {}

SceneModeState::~SceneModeState() = default;

void SceneModeState::Sync() {
  const auto start = std::chrono::steady_clock::now();
  SyncDocument();
  const double elapsed_ms = MillisecondsSince(start);
  if (elapsed_ms > kSlowSyncMs) {
    core::log::Info("Scene mode: slow frame sync ({:.1f} ms)", elapsed_ms);
  }
}

void SceneModeState::SyncDocument() {
  SceneDocument* doc = document();
  if (doc == nullptr) {
    world_.reset();
    built_document_ = nullptr;
    problems_.clear();
    material_names_.clear();
    applied_environment_.reset();
    return;
  }
  if (world_ == nullptr) {
    world_ = std::make_unique<world::World>(context_.prefab_cache());
  }
  const DocumentRevision revision = doc->revision();
  const bool edit_open = doc->history().edit_open();
  // A replaced document may reuse the old one's address with the same
  // revision, so its path is compared too.
  if (doc != built_document_ || doc->path() != built_path_ ||
      rebuild_requested_ ||
      (revision.any != built_revision_.any && !edit_open)) {
    Rebuild(*doc);
  } else if (revision.any != built_revision_.any) {
    // A drag in progress: only the dragged (selected) instances move.
    for (const scene::InstanceId id : doc->selection().Instances()) {
      world_->SetInstanceTransform(doc->state(), id);
    }
  }

  if (context_.mode() == EditorMode::kScene) {
    SyncEnvironment(doc->state());
  } else {
    // The viewport restores the neutral sky in Prefab mode; apply ours again
    // when Scene mode comes back.
    applied_environment_.reset();
  }
}

void SceneModeState::Rebuild(const SceneDocument& doc) {
  const auto start = std::chrono::steady_clock::now();
  const scene::SceneData& scene = doc.state();
  problems_.clear();
  for (world::WorldIssue& issue : world_->Build(scene)) {
    problems_.push_back({issue.instance, std::move(issue.message)});
  }
  material_names_ = world_->MaterialNames(scene);
  for (scene::SceneIssue& issue :
       scene::ValidateAgainstPrefabs(scene, material_names_)) {
    problems_.push_back({issue.instance, std::move(issue.message)});
  }
  built_scene_lights_ = scene.point_lights.size();
  built_document_ = &doc;
  built_path_ = doc.path();
  built_revision_ = doc.revision();
  rebuild_requested_ = false;
  core::log::Info(
      "Scene mode: world rebuilt in {:.2f} ms ({} placements, "
      "{} problems)",
      MillisecondsSince(start), world_->placements().size(), problems_.size());

  // Undo or a delete may have removed selected objects.
  SceneDocument* mutable_doc = document();
  if (mutable_doc != nullptr) {
    mutable_doc->selection().PruneInstances(scene);
    mutable_doc->selection().PruneIndices(SelectionKind::kScenePointLight,
                                          scene.point_lights.size());
  }
}

const asset::ImageDataF* SceneModeState::LoadHdri(const std::string& path,
                                                  std::string& error) {
  if (hdri_.has_value() && hdri_path_ == path) {
    return &*hdri_;
  }
  auto loaded = asset::LoadHdrFile(path);
  if (!loaded.ok()) {
    error = std::format("{}: {}", path, asset::ToString(loaded.error()));
    hdri_.reset();
    hdri_path_.clear();
    return nullptr;
  }
  hdri_ = std::move(loaded).value();
  hdri_path_ = path;
  return &*hdri_;
}

void SceneModeState::SyncEnvironment(const scene::SceneData& scene) {
  const scene::Environment& environment = scene.environment;
  render::Renderer& renderer = context_.renderer();
  if (!applied_environment_.has_value() ||
      applied_environment_->hdri_path != environment.hdri_path) {
    // A new image: load and filter it (waits for the GPU; only on a path
    // change or when Scene mode is entered).
    const auto start = std::chrono::steady_clock::now();
    if (environment.hdri_path.empty()) {
      renderer.ResetEnvironment();
      renderer.SetEnvironmentParams(environment.intensity,
                                    environment.rotation_radians);
    } else {
      std::string error;
      const asset::ImageDataF* image = LoadHdri(environment.hdri_path, error);
      if (image != nullptr) {
        renderer.SetEnvironment(*image, environment.intensity,
                                environment.rotation_radians);
      } else {
        context_.Status("Cannot load HDRI " + error + "; default sky shown");
        renderer.ResetEnvironment();
      }
    }
    applied_environment_ =
        AppliedEnvironment{environment.hdri_path, environment.intensity,
                           environment.rotation_radians};
    core::log::Info("Scene mode: environment applied in {:.1f} ms ({})",
                    MillisecondsSince(start),
                    environment.hdri_path.empty() ? std::string("default sky")
                                                  : environment.hdri_path);
    return;
  }
  if (applied_environment_->intensity != environment.intensity ||
      applied_environment_->rotation_radians != environment.rotation_radians) {
    renderer.SetEnvironmentParams(environment.intensity,
                                  environment.rotation_radians);
    applied_environment_->intensity = environment.intensity;
    applied_environment_->rotation_radians = environment.rotation_radians;
  }
}

std::vector<std::string> SceneModeState::PrefabMaterialNames(
    const std::string& path) {
  std::vector<std::string> names;
  auto id = context_.prefab_cache().Get(path);
  if (!id.ok()) {
    return names;
  }
  for (const prefab::Material& material :
       context_.prefab_cache().prefab(id.value()).materials) {
    names.push_back(material.name);
  }
  return names;
}

std::optional<prefab::Material> SceneModeState::FindPrefabMaterial(
    const std::string& path, std::string_view name) {
  auto id = context_.prefab_cache().Get(path);
  if (!id.ok()) {
    return std::nullopt;
  }
  for (const prefab::Material& material :
       context_.prefab_cache().prefab(id.value()).materials) {
    if (material.name == name) {
      return material;
    }
  }
  return std::nullopt;
}

glm::vec3 SceneModeState::DropPoint(std::optional<ImVec2> screen) {
  ViewportPanel& viewport = context_.viewport();
  const Ray ray =
      screen.has_value() ? viewport.RayAt(*screen) : viewport.CenterRay();
  std::optional<float> hit;
  if (world_ != nullptr) {
    if (const auto pick = world_->Pick(ray.origin, ray.direction, kPickDistance,
                                       world::PickMode::kTriangles)) {
      hit = pick->distance;
    }
  }
  return PlacementPoint(ray.origin, ray.direction, hit);
}

scene::InstanceId SceneModeState::PlaceInstance(const std::string& prefab_path,
                                                const glm::vec3& position) {
  SceneDocument* doc = document();
  if (doc == nullptr) {
    return scene::kNoInstance;
  }
  scene::InstanceId placed = scene::kNoInstance;
  doc->Edit("Add instance", ChangeKind::kStructure,
            [&](scene::SceneData& scene) {
              placed = PlacePrefab(scene, prefab_path, position);
            });
  if (placed != scene::kNoInstance) {
    doc->selection().Set(InstanceItem(placed));
    context_.Status(std::format("Placed {} at ({:.2f}, {:.2f}, {:.2f})",
                                scene::FindInstance(doc->state(), placed)->name,
                                position.x, position.y, position.z));
  }
  return placed;
}

bool SceneModeState::DuplicateSelection() {
  SceneDocument* doc = document();
  if (doc == nullptr || doc->selection().Instances().empty()) {
    return false;
  }
  const std::vector<scene::InstanceId> ids = doc->selection().Instances();
  std::vector<scene::InstanceId> copies;
  doc->Edit("Duplicate", ChangeKind::kStructure, [&](scene::SceneData& scene) {
    copies = DuplicateInstances(scene, ids, kDuplicateOffset);
  });
  doc->selection().Clear();
  for (const scene::InstanceId copy : copies) {
    doc->selection().Add(InstanceItem(copy));
  }
  context_.Status(std::format("Duplicated {} instance(s)", copies.size()));
  return !copies.empty();
}

bool SceneModeState::DeleteSelection() {
  SceneDocument* doc = document();
  if (doc == nullptr) {
    return false;
  }
  // Point lights selected in the outliner are deleted too (highest index
  // first so the others stay valid).
  std::vector<size_t> lights;
  for (const SelectionItem& item : doc->selection().items()) {
    if (item.kind == SelectionKind::kScenePointLight && item.index >= 0) {
      lights.push_back(static_cast<size_t>(item.index));
    }
  }
  const std::vector<scene::InstanceId> ids = doc->selection().Instances();
  if (ids.empty() && lights.empty()) {
    return false;
  }
  std::ranges::sort(lights, std::greater<>());
  int removed = 0;
  doc->Edit("Delete", ChangeKind::kStructure, [&](scene::SceneData& scene) {
    removed = DeleteInstances(scene, ids);
    for (const size_t light : lights) {
      if (light < scene.point_lights.size()) {
        scene.point_lights.erase(scene.point_lights.begin() +
                                 static_cast<std::ptrdiff_t>(light));
        ++removed;
      }
    }
  });
  doc->selection().Clear();
  context_.Status(std::format("Deleted {} object(s)", removed));
  return removed > 0;
}

bool SceneModeState::GroupSelection() {
  SceneDocument* doc = document();
  if (doc == nullptr || doc->selection().Instances().empty()) {
    return false;
  }
  const std::vector<scene::InstanceId> ids = doc->selection().Instances();
  scene::InstanceId group = scene::kNoInstance;
  doc->Edit("Group", ChangeKind::kStructure, [&](scene::SceneData& scene) {
    std::vector<std::string> names;
    for (const scene::Instance& instance : scene.instances) {
      names.push_back(instance.name);
    }
    group = scene::GroupInstances(scene, ids, MakeUniqueName("group", names));
  });
  if (group == scene::kNoInstance) {
    return false;
  }
  doc->selection().Set(InstanceItem(group));
  context_.Status("Grouped " + std::to_string(ids.size()) + " instance(s)");
  return true;
}

bool SceneModeState::UngroupSelection() {
  SceneDocument* doc = document();
  if (doc == nullptr) {
    return false;
  }
  const int64_t primary =
      doc->selection().PrimaryIndex(SelectionKind::kInstance);
  if (primary < 0) {
    return false;
  }
  const auto group = static_cast<scene::InstanceId>(primary);
  const std::vector<scene::InstanceId> children =
      scene::ChildrenOf(doc->state(), group);
  if (children.empty()) {
    context_.Status("Ungroup: the selected instance has no children");
    return false;
  }
  doc->Edit("Ungroup", ChangeKind::kStructure, [&](scene::SceneData& scene) {
    scene::UngroupInstance(scene, group);
  });
  doc->selection().Clear();
  for (const scene::InstanceId child : children) {
    doc->selection().Add(InstanceItem(child));
  }
  context_.Status("Ungrouped " + std::to_string(children.size()) +
                  " instance(s)");
  return true;
}

std::optional<size_t> SceneModeState::AddPointLight(const glm::vec3& position) {
  SceneDocument* doc = document();
  if (doc == nullptr) {
    return std::nullopt;
  }
  size_t index = 0;
  doc->Edit(
      "Add point light", ChangeKind::kStructure, [&](scene::SceneData& scene) {
        scene::ScenePointLight light;
        light.name = std::format("light {}", scene.point_lights.size() + 1);
        light.position = position;
        scene.point_lights.push_back(std::move(light));
        index = scene.point_lights.size() - 1;
      });
  doc->selection().Set(
      {SelectionKind::kScenePointLight, static_cast<int64_t>(index)});
  return index;
}

bool SceneModeState::StepSelection(int step) {
  SceneDocument* doc = document();
  if (doc == nullptr) {
    return false;
  }
  const int64_t primary =
      doc->selection().PrimaryIndex(SelectionKind::kInstance);
  const scene::InstanceId next =
      StepInstance(doc->state(),
                   primary < 0 ? scene::kNoInstance
                               : static_cast<scene::InstanceId>(primary),
                   step);
  if (next == scene::kNoInstance) {
    return false;
  }
  doc->selection().Set(InstanceItem(next));
  context_.Status("Selected " + scene::FindInstance(doc->state(), next)->name);
  return true;
}

core::Result<void, std::string> SceneModeState::AlignSunToHdri() {
  using Status = core::Result<void, std::string>;
  SceneDocument* doc = document();
  if (doc == nullptr) {
    return Status::Error("no scene document");
  }
  const scene::Environment& environment = doc->state().environment;
  if (environment.hdri_path.empty()) {
    return Status::Error("the scene uses the default sky (no HDRI)");
  }
  std::string error;
  const asset::ImageDataF* image = LoadHdri(environment.hdri_path, error);
  if (image == nullptr) {
    return Status::Error(error);
  }
  // The renderer turns the panorama by the rotation about +Y; the light
  // travels away from the brightest direction.
  const glm::vec3 towards_light = glm::angleAxis(environment.rotation_radians,
                                                 glm::vec3(0.0f, 1.0f, 0.0f)) *
                                  lighting::BrightestDirection(*image);
  doc->Edit("Align sun to HDRI", ChangeKind::kStructure,
            [&](scene::SceneData& scene) {
              scene.sun.direction = -glm::normalize(towards_light);
            });
  return Status::Ok();
}

void RegisterSceneMode(EditorApp& app) {
  EditorContext& context = app.context();
  // The Outliner panel owns the state and the viewport client (the shell
  // keeps panels until exit); the other panels, the viewport and the
  // command handlers borrow them.
  auto owned_state = std::make_unique<SceneModeState>(context);
  SceneModeState& state = *owned_state;
  std::unique_ptr<ViewportClient> client = MakeSceneViewportClient(state);
  context.viewport().SetClient(EditorMode::kScene, client.get());
  app.AddPanel(
      {.name = "Outliner", .modes = kSceneModeOnly, .slot = DockSlot::kLeft},
      MakeOutlinerPanel(std::move(owned_state), std::move(client)));
  app.AddPanel({.name = "Scene Inspector",
                .modes = kSceneModeOnly,
                .slot = DockSlot::kRight},
               MakeSceneInspectorPanel(state));
  app.AddPanel({.name = "Scene Settings",
                .modes = kSceneModeOnly,
                .slot = DockSlot::kRightBottom},
               MakeSceneSettingsPanel(state));
  app.AddPanel(
      {.name = "Problems", .modes = kSceneModeOnly, .slot = DockSlot::kBottom},
      MakeProblemsPanel(state));
  RegisterSceneCommands(state);
}

}  // namespace blue::editor
