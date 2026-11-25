#include "physics/character.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <memory>
#include <utility>

// Jolt.h must be the first Jolt header included.
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Geometry/Plane.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
// clang-format on

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include "core/log.h"
#include "physics/internal/jolt_math.h"
#include "physics/internal/layers.h"
#include "physics/internal/physics_world_impl.h"

namespace blue::physics {
namespace {

using internal::FromJolt;
using internal::ToJolt;

// A jump request outlives the press by this many steps (~133 ms), so a press
// just before landing still jumps.
constexpr int kJumpBufferSteps = 8;
// Steps after a launch during which the character does not count as grounded
// even if the contact has not been lost yet.
constexpr int kJumpCooldownSteps = 3;
// Upward speed relative to the ground above which the character is leaving
// it (a jump in progress) rather than standing on it.
constexpr float kLeavingGroundSpeed = 0.1f;

bool IsFiniteVec(glm::vec3 v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Returns whether the dimensions describe a buildable pair of capsules.
bool IsValidDesc(const CharacterDesc& desc) {
  return std::isfinite(desc.standing_height) &&
         std::isfinite(desc.crouch_height) && std::isfinite(desc.radius) &&
         desc.radius > 0.0f && desc.crouch_height > 2.0f * desc.radius &&
         desc.standing_height >= desc.crouch_height &&
         std::isfinite(desc.max_slope_degrees) && desc.mass > 0.0f &&
         desc.max_strength >= 0.0f && desc.step_up >= 0.0f &&
         desc.stick_to_floor >= 0.0f && std::isfinite(desc.air_control);
}

// Builds a capsule of total `height` whose bottom is at the local origin.
// Shape settings are reference counted and must live on the heap: the
// decorator keeps a reference to the inner settings.
JPH::ShapeRefC BuildCapsule(float height, float radius) {
  const float half_cylinder = 0.5f * height - radius;
  const JPH::Ref<JPH::ShapeSettings> capsule =
      new JPH::CapsuleShapeSettings(half_cylinder, radius);
  const JPH::Ref<JPH::ShapeSettings> raised =
      new JPH::RotatedTranslatedShapeSettings(
          JPH::Vec3(0.0f, 0.5f * height, 0.0f), JPH::Quat::sIdentity(),
          capsule);
  const JPH::ShapeSettings::ShapeResult result = raised->Create();
  return result.HasError() ? JPH::ShapeRefC() : result.Get();
}

GroundState FromJolt(JPH::CharacterBase::EGroundState state) {
  switch (state) {
    case JPH::CharacterBase::EGroundState::OnGround:
      return GroundState::kOnGround;
    case JPH::CharacterBase::EGroundState::OnSteepGround:
      return GroundState::kOnSteepGround;
    case JPH::CharacterBase::EGroundState::NotSupported:
      return GroundState::kNotSupported;
    case JPH::CharacterBase::EGroundState::InAir:
      return GroundState::kInAir;
  }
  return GroundState::kInAir;
}

}  // namespace

const char* ToString(GroundState state) {
  switch (state) {
    case GroundState::kOnGround:
      return "on ground";
    case GroundState::kOnSteepGround:
      return "on steep ground";
    case GroundState::kNotSupported:
      return "not supported";
    case GroundState::kInAir:
      return "in air";
  }
  return "unknown ground state";
}

// Owns the Jolt character and both capsules. Unless manually stepped it is
// registered with the world as a fixed-step participant for its whole
// lifetime.
class Character::Impl final : public internal::FixedStepParticipant {
 public:
  Impl(PhysicsWorld::Impl& world, const CharacterDesc& desc,
       JPH::ShapeRefC standing, JPH::ShapeRefC crouching, glm::vec3 feet)
      : world_(world),
        desc_(desc),
        standing_shape_(std::move(standing)),
        crouching_shape_(std::move(crouching)),
        previous_(feet),
        current_(feet) {
    const JPH::Ref<JPH::CharacterVirtualSettings> settings =
        new JPH::CharacterVirtualSettings();
    settings->mShape = standing_shape_;
    settings->mMaxSlopeAngle = JPH::DegreesToRadians(desc.max_slope_degrees);
    settings->mMass = desc.mass;
    settings->mMaxStrength = desc.max_strength;
    // Only contacts below the capsule's lower hemisphere center can support
    // the character, so touching a wall with the side never counts as ground.
    settings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -desc.radius);
    character_ =
        new JPH::CharacterVirtual(settings, JPH::RVec3(ToJolt(feet)),
                                  JPH::Quat::sIdentity(), &world_.system());
    extended_.mStickToFloorStepDown =
        JPH::Vec3(0.0f, -desc.stick_to_floor, 0.0f);
    extended_.mWalkStairsStepUp = JPH::Vec3(0.0f, desc.step_up, 0.0f);
    if (!desc.manual_step) {
      world_.AddParticipant(this);
    }
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  ~Impl() override { world_.RemoveParticipant(this); }

  void PreStep(float dt) override;

  void SetDesiredVelocity(glm::vec3 horizontal) {
    if (IsFiniteVec(horizontal)) {
      desired_ = glm::vec3(horizontal.x, 0.0f, horizontal.z);
    }
  }

  void RequestJump(float vertical_speed) {
    if (!(vertical_speed > 0.0f) || !std::isfinite(vertical_speed)) {
      return;
    }
    jump_speed_ = vertical_speed;
    jump_pending_ = true;
    jump_age_ = 0;
  }

  bool SetCrouching(bool crouching) {
    if (crouching == crouching_) {
      return true;
    }
    const JPH::Shape* shape =
        crouching ? crouching_shape_.GetPtr() : standing_shape_.GetPtr();
    // Shrinking always fits; growing may only enter penetration a solver
    // step can recover from.
    const float max_penetration =
        crouching
            ? FLT_MAX
            : 1.5f * world_.system().GetPhysicsSettings().mPenetrationSlop;
    JPH::TempAllocator& allocator = world_.temp_allocator();
    if (character_->SetShape(shape, max_penetration, BroadPhaseFilter(),
                             ObjectFilter(), body_filter_, shape_filter_,
                             allocator)) {
      crouching_ = crouching;
    }
    return crouching_ == crouching;
  }

  void Teleport(glm::vec3 feet) {
    if (!IsFiniteVec(feet)) {
      return;
    }
    character_->SetPosition(JPH::RVec3(ToJolt(feet)));
    character_->SetLinearVelocity(JPH::Vec3::sZero());
    previous_ = current_ = feet;
    jump_pending_ = false;
    jump_cooldown_ = 0;
    RefreshContacts();
  }

  void RefreshContacts() {
    character_->RefreshContacts(BroadPhaseFilter(), ObjectFilter(),
                                body_filter_, shape_filter_,
                                world_.temp_allocator());
  }

  void Step() {
    if (!desc_.manual_step) {
      core::log::Warn(
          "Character::Step ignored: the world steps this character");
      return;
    }
    PreStep(kFixedStepSeconds);
  }

  CharacterState SaveState() const {
    return CharacterState{
        .position = FromJolt(JPH::Vec3(character_->GetPosition())),
        .velocity = velocity(),
        .jump_pending = jump_pending_,
        .jump_age = jump_age_,
        .jump_cooldown = jump_cooldown_,
        .crouching = crouching_,
        .jump_speed = jump_speed_,
    };
  }

  void RestoreState(const CharacterState& state);

  glm::vec3 position() const {
    if (previous_ == current_) {
      return current_;  // Exact after a teleport or while standing still.
    }
    return glm::mix(previous_, current_, world_.interpolation_alpha());
  }
  glm::vec3 tick_position() const { return current_; }
  glm::vec3 velocity() const {
    return FromJolt(character_->GetLinearVelocity());
  }
  GroundState ground_state() const {
    return FromJolt(character_->GetGroundState());
  }
  bool IsOnGround() const {
    return ground_state() == GroundState::kOnGround && jump_cooldown_ == 0;
  }
  bool crouching() const { return crouching_; }
  float height() const {
    return crouching_ ? desc_.crouch_height : desc_.standing_height;
  }
  const CharacterDesc& desc() const { return desc_; }

 private:
  // The character collides with whatever a moving body collides with, except
  // the bodies that do not block characters.
  JPH::DefaultBroadPhaseLayerFilter BroadPhaseFilter() const {
    return world_.system().GetDefaultBroadPhaseLayerFilter(
        internal::layers::kMoving);
  }
  const internal::CharacterObjectFilter& ObjectFilter() const {
    return object_filter_;
  }

  PhysicsWorld::Impl& world_;
  CharacterDesc desc_;
  JPH::ShapeRefC standing_shape_;
  JPH::ShapeRefC crouching_shape_;
  JPH::Ref<JPH::CharacterVirtual> character_;
  JPH::CharacterVirtual::ExtendedUpdateSettings extended_;
  internal::CharacterObjectFilter object_filter_;
  JPH::BodyFilter body_filter_;
  JPH::ShapeFilter shape_filter_;

  glm::vec3 desired_{0.0f};
  glm::vec3 previous_;  // Feet after the step before the last one.
  glm::vec3 current_;   // Feet after the last step.
  float jump_speed_ = 0.0f;
  bool jump_pending_ = false;
  int jump_age_ = 0;       // Steps since the pending request was made.
  int jump_cooldown_ = 0;  // Steps left after a launch, see IsOnGround().
  bool crouching_ = false;
};

void Character::Impl::PreStep(float dt) {
  previous_ = current_;

  JPH::PhysicsSystem& system = world_.system();
  const JPH::Vec3 gravity = system.GetGravity();
  const JPH::Vec3 current = character_->GetLinearVelocity();
  const JPH::Vec3 ground = character_->GetGroundVelocity();
  const JPH::CharacterBase::EGroundState state = character_->GetGroundState();

  JPH::Vec3 desired = ToJolt(desired_);
  if (state == JPH::CharacterBase::EGroundState::OnSteepGround) {
    desired = character_->CancelVelocityTowardsSteepSlopes(desired);
  }

  const bool leaving_ground =
      current.GetY() - ground.GetY() > kLeavingGroundSpeed;
  JPH::Vec3 velocity;
  if (character_->IsSupported() && !leaving_ground) {
    velocity = ground + desired;
    if (state == JPH::CharacterBase::EGroundState::OnGround && jump_pending_ &&
        jump_cooldown_ == 0) {
      velocity += JPH::Vec3(0.0f, jump_speed_, 0.0f);
      jump_pending_ = false;
      jump_cooldown_ = kJumpCooldownSteps;
    }
  } else {
    const JPH::Vec3 horizontal(current.GetX(), 0.0f, current.GetZ());
    const float control = std::clamp(desc_.air_control, 0.0f, 1.0f);
    velocity = JPH::Vec3(0.0f, current.GetY(), 0.0f) +
               horizontal * (1.0f - control) + desired * control;
  }
  velocity += gravity * dt;
  character_->SetLinearVelocity(velocity);

  character_->ExtendedUpdate(dt, gravity, extended_, BroadPhaseFilter(),
                             ObjectFilter(), body_filter_, shape_filter_,
                             world_.temp_allocator());
  current_ = FromJolt(JPH::Vec3(character_->GetPosition()));

  if (jump_pending_ && ++jump_age_ > kJumpBufferSteps) {
    jump_pending_ = false;
  }
  if (jump_cooldown_ > 0) {
    --jump_cooldown_;
  }
}

void Character::Impl::RestoreState(const CharacterState& state) {
  if (!IsFiniteVec(state.position) || !IsFiniteVec(state.velocity)) {
    return;
  }
  // An unlimited penetration depth always switches the shape: shrinking
  // always fits, and a rewind to standing must not depend on what moved into
  // the head room since.
  const JPH::Shape* shape =
      state.crouching ? crouching_shape_.GetPtr() : standing_shape_.GetPtr();
  (void)character_->SetShape(shape, FLT_MAX, BroadPhaseFilter(), ObjectFilter(),
                             body_filter_, shape_filter_,
                             world_.temp_allocator());
  crouching_ = state.crouching;
  character_->SetPosition(JPH::RVec3(ToJolt(state.position)));
  character_->SetLinearVelocity(ToJolt(state.velocity));
  previous_ = current_ = state.position;
  jump_pending_ = state.jump_pending;
  jump_age_ = state.jump_age;
  jump_cooldown_ = state.jump_cooldown;
  jump_speed_ = state.jump_speed;
  RefreshContacts();
}

core::Result<std::unique_ptr<Character>, PhysicsError> Character::Create(
    PhysicsWorld& world, const CharacterDesc& desc, glm::vec3 feet_position) {
  using Result = core::Result<std::unique_ptr<Character>, PhysicsError>;
  if (!IsValidDesc(desc) || !IsFiniteVec(feet_position)) {
    return Result::Error(PhysicsError::kInvalidShape);
  }
  JPH::ShapeRefC standing = BuildCapsule(desc.standing_height, desc.radius);
  JPH::ShapeRefC crouching = BuildCapsule(desc.crouch_height, desc.radius);
  if (standing == nullptr || crouching == nullptr) {
    return Result::Error(PhysicsError::kShapeBuildFailed);
  }
  auto impl = std::make_unique<Impl>(*world.impl_, desc, std::move(standing),
                                     std::move(crouching), feet_position);
  impl->RefreshContacts();
  return Result::Ok(std::unique_ptr<Character>(new Character(std::move(impl))));
}

Character::Character(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Character::~Character() = default;

void Character::SetDesiredVelocity(glm::vec3 horizontal) {
  impl_->SetDesiredVelocity(horizontal);
}

void Character::RequestJump(float vertical_speed) {
  impl_->RequestJump(vertical_speed);
}

bool Character::SetCrouching(bool crouching) {
  return impl_->SetCrouching(crouching);
}

void Character::Teleport(glm::vec3 feet_position) {
  impl_->Teleport(feet_position);
}

void Character::RefreshContacts() { impl_->RefreshContacts(); }

void Character::Step() { impl_->Step(); }

CharacterState Character::SaveState() const { return impl_->SaveState(); }

void Character::RestoreState(const CharacterState& state) {
  impl_->RestoreState(state);
}

glm::vec3 Character::position() const { return impl_->position(); }

glm::vec3 Character::tick_position() const { return impl_->tick_position(); }

glm::vec3 Character::velocity() const { return impl_->velocity(); }

GroundState Character::ground_state() const { return impl_->ground_state(); }

bool Character::IsOnGround() const { return impl_->IsOnGround(); }

bool Character::crouching() const { return impl_->crouching(); }

float Character::height() const { return impl_->height(); }

const CharacterDesc& Character::desc() const { return impl_->desc(); }

}  // namespace blue::physics
