#include "physics/scene_physics.h"

#include <algorithm>

#include "core/log.h"

namespace blue::physics {

RebuildSummary ScenePhysics::Rebuild(const scene::Scene& scene,
                                     const MeshSource& mesh_source) {
  world_.RemoveAllBodies();
  world_.SetGravity(scene.gravity);
  bodies_.assign(scene.entities.size(), std::nullopt);
  entities_.clear();

  RebuildSummary summary;
  for (size_t i = 0; i < scene.entities.size(); ++i) {
    const scene::Entity& entity = scene.entities[i];
    if (!entity.body.has_value()) {
      continue;
    }

    const std::optional<MeshView> mesh = mesh_source(entity.mesh_path);
    if (!mesh.has_value()) {
      core::log::Warn("Physics: no mesh '{}' for entity '{}', body skipped",
                      entity.mesh_path, entity.name);
      ++summary.failed;
      continue;
    }

    const BodyDesc desc{
        .body = *entity.body,
        .pose = {.position = entity.transform.position,
                 .rotation = entity.transform.rotation},
        .scale = entity.transform.scale,
        .bounds = mesh->bounds,
        .mesh = mesh->mesh,
        .shape_key = entity.mesh_path,
    };
    const auto created = world_.CreateBody(desc);
    if (!created.ok()) {
      core::log::Warn("Physics: body for entity '{}' failed: {}", entity.name,
                      ToString(created.error()));
      ++summary.failed;
      continue;
    }
    bodies_[i] = created.value();
    entities_.emplace(static_cast<uint32_t>(created.value()), i);
    ++summary.created;
  }
  return summary;
}

void ScenePhysics::SyncToScene(scene::Scene& scene) const {
  const size_t count = std::min(bodies_.size(), scene.entities.size());
  for (size_t i = 0; i < count; ++i) {
    if (!bodies_[i].has_value() || !world_.IsDynamic(*bodies_[i])) {
      continue;
    }
    const Pose pose = world_.InterpolatedPose(*bodies_[i]);
    scene.entities[i].transform.position = pose.position;
    scene.entities[i].transform.rotation = pose.rotation;
  }
}

std::optional<size_t> ScenePhysics::EntityFor(BodyId body) const {
  const auto found = entities_.find(static_cast<uint32_t>(body));
  if (found == entities_.end()) {
    return std::nullopt;
  }
  return found->second;
}

std::optional<BodyId> ScenePhysics::BodyFor(size_t entity) const {
  return entity < bodies_.size() ? bodies_[entity] : std::nullopt;
}

}  // namespace blue::physics
