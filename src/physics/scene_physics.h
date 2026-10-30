#ifndef BLUE_PHYSICS_SCENE_PHYSICS_H_
#define BLUE_PHYSICS_SCENE_PHYSICS_H_

#include <cstddef>
#include <functional>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "asset/mesh_data.h"
#include "physics/physics_world.h"
#include "scene/scene.h"

namespace blue::physics {

// CPU-side description of an entity's mesh. `mesh` is borrowed: it must stay
// valid until the MeshSource caller (ScenePhysics::Rebuild) returns.
struct MeshView {
  asset::Bounds bounds;
  const asset::MeshData* mesh = nullptr;
};

// Resolves an entity's mesh_path to its CPU mesh, or nullopt if unavailable.
// Called synchronously from Rebuild only; it is never stored.
using MeshSource = std::function<std::optional<MeshView>(std::string_view)>;

struct RebuildSummary {
  int created = 0;  // Entities that now have a body.
  int failed = 0;   // Entities with a body description that got none.
};

// Glue between a scene::Scene and a PhysicsWorld: owns the mapping entity
// index <-> BodyId and moves poses from the simulation back into the scene.
// Borrows `world`, which must outlive it and should not be given bodies by
// anyone else. Neither copyable nor movable. Not thread-safe.
class ScenePhysics {
 public:
  explicit ScenePhysics(PhysicsWorld& world) : world_(world) {}
  ScenePhysics(const ScenePhysics&) = delete;
  ScenePhysics& operator=(const ScenePhysics&) = delete;

  // Removes every body of the world, applies `scene.gravity` and creates one
  // body per entity that has a `body`, from its transform and the mesh
  // returned by `mesh_source` (the mesh path is the shape key). Failures are
  // logged with the entity name and skipped. Entity indices refer to `scene`
  // as passed here: call Rebuild again after entities are added or removed.
  RebuildSummary Rebuild(const scene::Scene& scene,
                         const MeshSource& mesh_source);

  // Writes the interpolated pose of every dynamic entity's body into the
  // entity's position and rotation. Scale and static entities are untouched.
  // `scene` must have the entity count it had at the last Rebuild.
  void SyncToScene(scene::Scene& scene) const;

  [[nodiscard]] std::optional<size_t> EntityFor(BodyId body) const;
  [[nodiscard]] std::optional<BodyId> BodyFor(size_t entity) const;

 private:
  PhysicsWorld& world_;
  std::vector<std::optional<BodyId>> bodies_;      // Indexed by entity.
  std::unordered_map<uint32_t, size_t> entities_;  // BodyId value -> entity.
};

}  // namespace blue::physics

#endif  // BLUE_PHYSICS_SCENE_PHYSICS_H_
