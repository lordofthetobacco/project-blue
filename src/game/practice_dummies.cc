#include "game/practice_dummies.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>

#include <glm/vec3.hpp>

#include "core/log.h"
#include "game/character_animator.h"
#include "game/ragdolls.h"
#include "physics/physics_world.h"
#include "physics/shape_cooking.h"
#include "scene/scene.h"
#include "sim/damage.h"

namespace blue::game {
namespace {

// Added to the dummy's index to tag its ragdoll; clear of player ids (+ 1).
constexpr uint64_t kDummyOwnerBase = uint64_t{1} << 32;

RagdollZone ZoneOf(float height_fraction) {
  switch (sim::ZoneOfHeight(height_fraction)) {
    case sim::HitZone::kHead:
      return RagdollZone::kHead;
    case sim::HitZone::kTorso:
      return RagdollZone::kTorso;
    case sim::HitZone::kLegs:
      return RagdollZone::kLegs;
  }
  return RagdollZone::kAny;
}

}  // namespace

PracticeDummies::PracticeDummies(physics::PhysicsWorld& world,
                                 const PracticeDummyTuning& tuning)
    : world_(world), tuning_(tuning) {}

PracticeDummies::~PracticeDummies() { Clear(); }

int PracticeDummies::Reset(const CharacterRig& rig,
                           std::span<const DummyPlacement> placements) {
  Clear();
  if (!proxy_shape_) {
    // A capsule along Y: the cylinder part is the height minus both caps.
    const float half_height =
        std::max(tuning_.height * 0.5f - tuning_.radius, 0.01f);
    auto bytes = physics::CookShape({.kind = physics::CookedShapeKind::kCapsule,
                                     .radius = tuning_.radius,
                                     .half_height = half_height});
    if (!bytes.ok()) {
      core::log::Warn("Practice dummy proxy: {}",
                      physics::ToString(bytes.error()));
      return 0;
    }
    auto restored = physics::CookedShape::Restore(bytes.value());
    if (!restored.ok()) {
      core::log::Warn("Practice dummy proxy: {}",
                      physics::ToString(restored.error()));
      return 0;
    }
    proxy_shape_.emplace(std::move(restored).value());
  }
  dummies_.reserve(placements.size());
  for (const DummyPlacement& placement : placements) {
    PracticeDummy dummy{.placement = placement};
    if (!MakeProxy(dummy)) {
      continue;
    }
    dummy.animator.emplace(rig);
    Animate(dummy, 0.0f);
    dummies_.push_back(std::move(dummy));
  }
  return static_cast<int>(dummies_.size());
}

void PracticeDummies::Clear() {
  for (const PracticeDummy& dummy : dummies_) {
    if (dummy.proxy) {
      world_.RemoveBody(*dummy.proxy);
    }
  }
  dummies_.clear();
}

bool PracticeDummies::MakeProxy(PracticeDummy& dummy) {
  if (!proxy_shape_) {
    return false;
  }
  const auto created = world_.CreateBody(
      *proxy_shape_,
      {.motion = scene::MotionType::kStatic,
       .pose = {.position = dummy.placement.feet +
                            glm::vec3(0.0f, tuning_.height * 0.5f, 0.0f)}});
  if (!created.ok()) {
    core::log::Warn("Practice dummy proxy: {}",
                    physics::ToString(created.error()));
    return false;
  }
  dummy.proxy = created.value();
  dummy.standing = true;
  dummy.down_seconds = 0.0f;
  return true;
}

void PracticeDummies::Animate(PracticeDummy& dummy, float dt) {
  dummy.animator->Update({.dt = dt,
                          .feet = dummy.placement.feet,
                          .yaw = dummy.placement.yaw,
                          .weapon = dummy.placement.hold});
}

bool PracticeDummies::IsProxy(physics::BodyId body) const {
  return std::any_of(dummies_.begin(), dummies_.end(),
                     [body](const PracticeDummy& dummy) {
                       return dummy.proxy && *dummy.proxy == body;
                     });
}

std::optional<RagdollSpawn> PracticeDummies::Hit(physics::BodyId body,
                                                 glm::vec3 point,
                                                 glm::vec3 velocity,
                                                 float mass) {
  for (size_t i = 0; i < dummies_.size(); ++i) {
    PracticeDummy& dummy = dummies_[i];
    if (!dummy.proxy || *dummy.proxy != body) {
      continue;
    }
    world_.RemoveBody(*dummy.proxy);
    dummy.proxy.reset();
    dummy.standing = false;
    dummy.down_seconds = 0.0f;
    const float fraction = (point.y - dummy.placement.feet.y) / tuning_.height;
    return RagdollSpawn{.root = dummy.animator->root(),
                        .locals = dummy.animator->locals(),
                        .velocity = glm::vec3(0.0f),
                        .hit = RagdollHit{.point = point,
                                          .velocity = velocity,
                                          .mass = mass,
                                          .zone = ZoneOf(fraction)},
                        .owner = kDummyOwnerBase + i};
  }
  return std::nullopt;
}

void PracticeDummies::Update(float dt) {
  for (PracticeDummy& dummy : dummies_) {
    if (dummy.standing) {
      Animate(dummy, dt);
      continue;
    }
    dummy.down_seconds += dt;
    if (dummy.down_seconds >= tuning_.respawn_seconds && MakeProxy(dummy)) {
      dummy.animator->Reset();
      Animate(dummy, 0.0f);
    }
  }
}

}  // namespace blue::game
