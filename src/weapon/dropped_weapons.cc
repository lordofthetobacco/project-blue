#include "weapon/dropped_weapons.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <glm/common.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "core/log.h"
#include "core/result.h"
#include "physics/physics_world.h"
#include "physics/shape_cooking.h"
#include "scene/scene.h"

namespace blue::weapon {
namespace {

// Half extents below this are padded, so a flat part never makes a
// degenerate box.
constexpr float kMinHalfExtent = 0.005f;  // m
constexpr float kFriction = 0.6f;
constexpr float kRestitution = 0.1f;

}  // namespace

DroppedWeapons::~DroppedWeapons() { Clear(); }

bool DroppedWeapons::SetShape(WeaponDefId def, const asset::Bounds& bounds) {
  const physics::ShapeCookDesc desc{
      .kind = physics::CookedShapeKind::kBox,
      .center = bounds.center(),
      .half_extents = glm::max(0.5f * bounds.size(), glm::vec3(kMinHalfExtent)),
  };
  core::Result<std::vector<std::byte>, physics::PhysicsError> cooked =
      physics::CookShape(desc);
  if (!cooked.ok()) {
    core::log::Error("Dropped weapon shape: {}",
                     physics::ToString(cooked.error()));
    return false;
  }
  core::Result<physics::CookedShape, physics::PhysicsError> shape =
      physics::CookedShape::Restore(cooked.value());
  if (!shape.ok()) {
    core::log::Error("Dropped weapon shape: {}",
                     physics::ToString(shape.error()));
    return false;
  }
  const auto index = static_cast<size_t>(def);
  if (index >= shapes_.size()) {
    shapes_.resize(index + 1);
  }
  shapes_[index] = {.shape = std::move(shape).value(), .bounds = bounds};
  return true;
}

asset::Bounds DroppedWeapons::bounds(WeaponDefId def) const {
  const auto index = static_cast<size_t>(def);
  return index < shapes_.size() ? shapes_[index].bounds : asset::Bounds{};
}

uint32_t DroppedWeapons::Add(WeaponItem weapon, const ThrowPlan& plan) {
  DroppedWeapon& item = items_.emplace_back();
  item.id = next_id_++;
  item.weapon = std::move(weapon);
  item.pose = {.position = plan.position, .rotation = plan.rotation};
  item.scale = plan.scale;
  CreateBody(item, plan.linear_velocity, plan.angular_velocity);
  return item.id;
}

std::optional<WeaponItem> DroppedWeapons::Take(uint32_t id) {
  const auto found = std::ranges::find(items_, id, &DroppedWeapon::id);
  if (found == items_.end()) {
    return std::nullopt;
  }
  if (found->body.has_value()) {
    world_.RemoveBody(*found->body);
  }
  std::optional<WeaponItem> weapon = std::move(found->weapon);
  items_.erase(found);
  return weapon;
}

void DroppedWeapons::Clear() {
  for (const DroppedWeapon& item : items_) {
    if (item.body.has_value()) {
      world_.RemoveBody(*item.body);
    }
  }
  items_.clear();
}

void DroppedWeapons::Sync() {
  for (DroppedWeapon& item : items_) {
    if (item.body.has_value()) {
      item.pose = world_.InterpolatedPose(*item.body);
    }
  }
}

void DroppedWeapons::RecreateBodies() {
  for (DroppedWeapon& item : items_) {
    if (item.body.has_value()) {
      world_.RemoveBody(*item.body);  // Ignored when already gone.
    }
    CreateBody(item, glm::vec3(0.0f), glm::vec3(0.0f));
  }
}

const DroppedWeapon* DroppedWeapons::Find(uint32_t id) const {
  const auto found = std::ranges::find(items_, id, &DroppedWeapon::id);
  return found == items_.end() ? nullptr : &*found;
}

const DroppedWeapon* DroppedWeapons::FindByBody(physics::BodyId body) const {
  const auto found = std::ranges::find_if(
      items_, [body](const DroppedWeapon& item) { return item.body == body; });
  return found == items_.end() ? nullptr : &*found;
}

glm::mat4 DroppedWeapons::Matrix(const DroppedWeapon& item) {
  glm::mat4 matrix = glm::mat4_cast(item.pose.rotation);
  for (int i = 0; i < 3; ++i) {
    matrix[i] *= item.scale[i];
  }
  matrix[3] = glm::vec4(item.pose.position, 1.0f);
  return matrix;
}

void DroppedWeapons::CreateBody(DroppedWeapon& item, glm::vec3 linear_velocity,
                                glm::vec3 angular_velocity) {
  item.body.reset();
  const FirearmSpec& spec = item.weapon.firearm.spec();
  const auto def = static_cast<size_t>(item.weapon.def);
  if (def >= shapes_.size() || !shapes_[def].shape.has_value()) {
    core::log::Error("No shape for a dropped {}; it stays in the air",
                     spec.display_name);
    return;
  }
  const physics::CookedShape& shape = *shapes_[def].shape;
  const core::Result<physics::BodyId, physics::PhysicsError> body =
      world_.CreateBody(shape, {.motion = scene::MotionType::kDynamic,
                                .pose = item.pose,
                                .scale = item.scale,
                                .mass = spec.mass,
                                .friction = kFriction,
                                .restitution = kRestitution,
                                .linear_velocity = linear_velocity,
                                .angular_velocity = angular_velocity,
                                .blocks_characters = false});
  if (!body.ok()) {
    core::log::Error("Cannot create the body of a dropped {}: {}",
                     spec.display_name, physics::ToString(body.error()));
    return;
  }
  item.body = body.value();
}

}  // namespace blue::weapon
