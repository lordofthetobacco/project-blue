#include "physics/ragdoll.h"

#include <span>
#include <utility>

#include "core/result.h"
#include "physics/internal/ragdoll_data.h"

namespace blue::physics {

core::Result<RagdollTemplate, PhysicsError> RagdollTemplate::Create(
    const RagdollDesc& /*desc*/) {
  return core::Result<RagdollTemplate, PhysicsError>::Error(
      PhysicsError::kInvalidShape);
}

int RagdollTemplate::part_count() const {
  return data_ ? static_cast<int>(data_->parents.size()) : 0;
}

int RagdollTemplate::parent(int part) const {
  if (!data_ || part < 0 || part >= part_count()) {
    return -1;
  }
  return data_->parents[static_cast<size_t>(part)];
}

Pose RagdollTemplate::rest_pose(int part) const {
  if (!data_ || part < 0 || part >= part_count()) {
    return Pose{};
  }
  return data_->rest_poses[static_cast<size_t>(part)];
}

const CookedShape& RagdollTemplate::shape(int part) const {
  return data_->shapes[static_cast<size_t>(part)];
}

core::Result<Ragdoll, PhysicsError> Ragdoll::Create(
    PhysicsWorld& /*world*/, const RagdollTemplate& /*tmpl*/,
    const RagdollSpawnDesc& /*spawn*/) {
  return core::Result<Ragdoll, PhysicsError>::Error(
      PhysicsError::kInvalidShape);
}

Ragdoll::Ragdoll(Ragdoll&& other) noexcept
    : world_(std::exchange(other.world_, nullptr)),
      key_(std::exchange(other.key_, 0)),
      bodies_(std::move(other.bodies_)) {
  other.bodies_.clear();
}

Ragdoll& Ragdoll::operator=(Ragdoll&& other) noexcept {
  if (this != &other) {
    world_ = std::exchange(other.world_, nullptr);
    key_ = std::exchange(other.key_, 0);
    bodies_ = std::move(other.bodies_);
    other.bodies_.clear();
  }
  return *this;
}

Ragdoll::~Ragdoll() = default;

bool Ragdoll::valid() const { return world_ != nullptr; }

std::span<const BodyId> Ragdoll::bodies() const {
  return valid() ? std::span<const BodyId>(bodies_) : std::span<const BodyId>();
}

bool Ragdoll::IsActive() const { return false; }

}  // namespace blue::physics
