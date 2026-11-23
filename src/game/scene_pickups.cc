#include "game/scene_pickups.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "core/log.h"
#include "scene/scene.h"

namespace blue::game {

interact::OrientedBox PlacedBox(const glm::mat4& matrix,
                                const asset::Bounds& bounds) {
  interact::OrientedBox box;
  box.center = glm::vec3(matrix * glm::vec4(bounds.center(), 1.0f));
  const glm::vec3 size = bounds.size();
  for (int i = 0; i < 3; ++i) {
    const glm::vec3 axis(matrix[i]);
    const float length = glm::length(axis);
    if (length > 0.0f) {
      box.axes[i] = axis / length;
    }
    box.half_extents[i] = 0.5f * size[i] * length;
  }
  return box;
}

void ScenePickups::Find(const scene::SceneData& scene,
                        const world::World& world,
                        const DefinitionOf& definition_of) {
  pickups_.clear();
  const std::span<const world::Placement> placements = world.placements();
  for (size_t p = 0; p < placements.size(); ++p) {
    const world::Placement& placement = placements[p];
    if (placement.parent != -1) {
      continue;  // A prefab nested in a prefab is part of its instance.
    }
    const std::optional<weapon::WeaponDefId> def =
        definition_of(placement.prefab);
    if (!def) {
      continue;
    }
    const scene::Instance* instance =
        scene::FindInstance(scene, placement.owner);
    if (instance == nullptr) {
      continue;
    }
    if (instance->parent != scene::kNoInstance) {
      core::log::Warn(
          "Weapon instance '{}' is a child of another instance: not a "
          "pick-up",
          instance->name);
      continue;
    }
    pickups_.push_back(
        {.instance = instance->id,
         .def = *def,
         .placement = p,
         .bounds = world.cache().prefab(placement.prefab).bounds});
  }
}

void ScenePickups::AppendCandidates(
    const world::World& world, std::vector<interact::Interactable>& out) const {
  const std::span<const world::Placement> placements = world.placements();
  for (const ScenePickup& pickup : pickups_) {
    if (pickup.taken) {
      continue;
    }
    out.push_back({.id = pickup.instance,
                   .kind = interact::InteractableKind::kSceneWeapon,
                   .shape = PlacedBox(placements[pickup.placement].world,
                                      pickup.bounds)});
  }
}

std::optional<weapon::WeaponItem> ScenePickups::Take(
    scene::InstanceId instance, world::World& world,
    world::WorldPhysics& physics, const weapon::WeaponLibrary& library) {
  const auto pickup =
      std::ranges::find(pickups_, instance, &ScenePickup::instance);
  if (pickup == pickups_.end() || pickup->taken) {
    return std::nullopt;
  }
  pickup->taken = true;
  (void)world.HideInstance(instance);
  (void)physics.RemoveHiddenBodies(world);
  return weapon::MakeFreshWeapon(pickup->def, library.def(pickup->def));
}

}  // namespace blue::game
