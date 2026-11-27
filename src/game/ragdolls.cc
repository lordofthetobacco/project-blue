#include "game/ragdolls.h"

#include <memory>
#include <span>
#include <vector>

#include "game/character_animator.h"
#include "physics/physics_world.h"
#include "prefab/prefab.h"

namespace blue::game {

struct Ragdolls::Impl {
  Impl(physics::PhysicsWorld& world_in, const RagdollTuning& tuning_in)
      : world(world_in), tuning(tuning_in) {}

  physics::PhysicsWorld& world;
  RagdollTuning tuning;
  std::vector<RagdollView> views;
};

Ragdolls::Ragdolls(physics::PhysicsWorld& world, const RagdollTuning& tuning)
    : impl_(std::make_unique<Impl>(world, tuning)) {}

Ragdolls::~Ragdolls() = default;

bool Ragdolls::SetModel(const CharacterRig& /*rig*/,
                        const prefab::Prefab& /*prefab*/) {
  return false;
}

void Ragdolls::ClearModel() {}

bool Ragdolls::has_model() const { return false; }

RagdollId Ragdolls::Spawn(const RagdollSpawn& /*spawn*/) { return 0; }

void Ragdolls::Update(float /*dt*/) {}

bool Ragdolls::Remove(RagdollId /*id*/) { return false; }

void Ragdolls::Clear() {}

std::span<const RagdollView> Ragdolls::views() const { return impl_->views; }

const RagdollView* Ragdolls::Find(RagdollId /*id*/) const { return nullptr; }

RagdollId Ragdolls::FindByBody(physics::BodyId /*body*/) const { return 0; }

}  // namespace blue::game
