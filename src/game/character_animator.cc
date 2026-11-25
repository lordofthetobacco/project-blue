#include "game/character_animator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/common.hpp>
#include <glm/ext/quaternion_common.hpp>
#include <glm/ext/quaternion_float.hpp>
#include <glm/ext/quaternion_geometric.hpp>
#include <glm/ext/quaternion_trigonometric.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "animation/hierarchy.h"
#include "animation/transform.h"
#include "core/result.h"
#include "prefab/prefab.h"

namespace blue::game {
namespace {

constexpr float kPi = std::numbers::pi_v<float>;
constexpr glm::vec3 kUp{0.0f, 1.0f, 0.0f};
constexpr glm::vec3 kForward{0.0f, 0.0f, -1.0f};  // figure space
constexpr glm::vec3 kXAxis{1.0f, 0.0f, 0.0f};

// Longest step Update() integrates; a longer dt (a stall) is cut to it.
constexpr float kMaxStep = 0.1f;  // s
// Inputs beyond these are treated as these.
constexpr float kMaxSpeed = 30.0f;  // m/s
constexpr float kMaxPitch = 1.5f;   // rad

// Exponential blends: the time (s) to cover 63 % of a change.
constexpr float kSpeedTau = 0.1f;
constexpr float kDirectionTau = 0.06f;
constexpr float kWeaponTau = 0.035f;
// State blends: eased ramps over this long (s).
constexpr float kStanceSeconds = 0.18f;  // crouch
constexpr float kAirSeconds = 0.12f;
constexpr float kAimSeconds = 0.15f;
constexpr float kArmedSeconds = 0.15f;
constexpr float kWalkSeconds = 0.2f;
// Landing settle: the pelvis dips and comes back over this long.
constexpr float kLandingSeconds = 0.15f;
constexpr float kLandingDip = 0.06f;  // m

// Gait. A cycle is two steps; its length (m) grows with speed, the share
// of it a foot stands on the ground shrinks (walk -> sprint).
constexpr float kCycleBase = 0.7f;      // m
constexpr float kCyclePerSpeed = 0.2f;  // m per m/s
constexpr float kCycleMin = 0.9f;       // m
constexpr float kCycleMax = 2.1f;       // m
constexpr float kWalkStance = 0.6f;
constexpr float kSprintStance = 0.34f;
constexpr float kRunFrom = 2.5f;      // m/s, the sprint blend starts
constexpr float kRunTo = 6.5f;        // m/s, ... and is complete
constexpr float kWalkFrom = 0.05f;    // m/s, the gait blends in from here
constexpr float kWalkTo = 0.6f;       // m/s, ... to here
constexpr float kStepLift = 0.07f;    // m, swing height of a walk
constexpr float kRunLift = 0.08f;     // m, added at a sprint
constexpr float kStanceWidth = 0.8f;  // of the rest ankle spacing
// Moving sideways the steps are shorter (a side-step shuffle) and the
// stance wider, so the feet do not cross: at a pure strafe the cycle is
// (1 - kStrafeShorten) as long and the stance (1 + kStrafeWiden) as wide.
constexpr float kStrafeShorten = 0.5f;
constexpr float kStrafeWiden = 0.6f;
constexpr float kLegReach = 0.995f;   // of the leg length, at most
constexpr float kStandBend = 0.015f;  // m, knees never locked
// Crouch: pelvis height as a share of the rest one, feet staggered.
constexpr float kCrouchPelvis = 0.52f;
constexpr float kCrouchPelvisBack = 0.06f;  // m
constexpr float kCrouchLeftFoot = -0.08f;   // m along z
constexpr float kCrouchRightFoot = 0.12f;   // m along z
// Airborne: feet tucked up, one forward, one back.
constexpr float kAirTuck = 0.2f;  // m
constexpr float kAirLeftFoot = -0.1f;
constexpr float kAirRightFoot = 0.12f;
constexpr float kAirToe = 0.3f;  // rad, toes hang

// Upper body (rad unless noted). Leaning forward: into a crouch, and with
// speed when moving forwards (about 5 degrees at a 2 m/s walk, 11 at a
// 7 m/s sprint). The pelvis tilts and turns with most of the upper body's
// lean, pitch and twist (kPelvisLeanShare), so the waist (where the mesh
// splits between hip and ribs) bends little.
constexpr float kCrouchLean = 0.45f;
// The walking lean is measured from the vertical (the chest, ribs joint to
// neck), not from the model's rest pose, which leans back a little.
constexpr float kWalkLean = 0.087f;  // reached at kWalkLeanSpeed
constexpr float kWalkLeanSpeed = 2.0f;
constexpr float kSprintLean = 0.14f;  // added up to kRunTo
constexpr float kPelvisLeanShare = 0.8f;
constexpr float kSpinePitchShare = 0.25f;
constexpr float kMaxSpinePitch = 0.35f;  // 20 degrees; the arms do the rest
constexpr float kHeadPitchShare = 0.8f;
constexpr float kMaxHeadPitch = 1.0f;
constexpr float kPelvisTwist = 0.1f;
constexpr float kPelvisSway = 0.015f;  // m
constexpr float kWalkBob = 0.012f;     // m
constexpr float kRunBob = 0.02f;       // m
constexpr float kRifleTwist = -0.5f;   // the torso turns right, bladed
constexpr float kBreath = 0.012f;
constexpr float kBreathHz = 0.25f;

// Weapon placement, relative to the middle of the shoulder joints
// (character space, metres). The weapon always points along the look
// (aimed) or along the look dipped by kReadyDip (low ready).
// Rifle: the butt in the shoulder pocket, in front of the shoulder joints
// and the face, high when aimed.
constexpr glm::vec3 kRifleAimStock{0.07f, 0.12f, -0.17f};
constexpr glm::vec3 kRifleReadyStock{0.1f, -0.02f, -0.13f};
// Pistol: the sight this far along the weapon's direction from a pivot
// between the shoulders, raised to the eye line when aimed and lowered as
// the look rises (the arms are short).
constexpr float kPistolAimPivot = 0.27f;
constexpr float kPistolPivotDropUp = 0.15f;
constexpr float kPistolAimReach = 0.4f;
constexpr glm::vec3 kPistolReadyPivot{0.02f, 0.0f, 0.0f};
constexpr float kPistolReadyReach = 0.32f;
// Unarmed hands: below the shoulder joint, swinging opposite the legs.
constexpr glm::vec3 kHandDown{0.06f, -0.5f, 0.03f};  // right side; x mirrored
constexpr float kArmSwing = 0.12f;                   // m
// Elbow poles (right side; x mirrored), character space: down and out,
// and back as well when unarmed (hanging arms). Not turned with the spine:
// a leaning pole can line up with the reach and flip the elbow.
constexpr glm::vec3 kElbowPole{0.6f, -1.0f, 0.0f};
constexpr float kElbowBackUnarmed = 0.5f;
constexpr float kKneeOut = 0.15f;

// Weapon defaults (metres, the weapon's frame).
constexpr float kRifleSupportForward = 0.08f;
constexpr float kRifleSupportUp = 0.05f;
constexpr float kPistolSupportSide = 0.035f;
constexpr float kPistolSupportForward = 0.005f;
constexpr float kPistolSupportDown = 0.01f;
constexpr float kStockUp = 0.05f;
constexpr float kStockInset = 0.01f;

struct JointInfo {
  std::string_view name;
  int parent;  // Joint index; -1 for the root
};

constexpr std::array<JointInfo, kJointCount> kJoints{{
    {"Root", -1},
    {"Hip", 0},
    {"Ribs", 0},
    {"Neck", 2},
    {"Head", 3},
    {"Left_Shoulder", 2},
    {"Left_Upper_Arm", 5},
    {"Left_Forearm", 6},
    {"Left_Wrist", 7},
    {"Left_Thigh", 1},
    {"Left_Knee", 9},
    {"Left_Ankle", 10},
    {"Left_Toe", 11},
    {"Right_Shoulder", 2},
    {"Right_Upper_Arm", 13},
    {"Right_Forearm", 14},
    {"Right_Wrist", 15},
    {"Right_Thigh", 1},
    {"Right_Knee", 17},
    {"Right_Ankle", 18},
    {"Right_Toe", 19},
}};

// Limb joints of a side: offsets from Joint::kLeftShoulder.
enum class Limb : int {
  kShoulder = 0,
  kUpperArm = 1,
  kForearm = 2,
  kWrist = 3,
  kThigh = 4,
  kKnee = 5,
  kAnkle = 6,
  kToe = 7,
};
constexpr int kSideStride = 8;

constexpr Joint LimbJoint(Side side, Limb limb) {
  return static_cast<Joint>(static_cast<int>(Joint::kLeftShoulder) +
                            static_cast<int>(side) * kSideStride +
                            static_cast<int>(limb));
}

constexpr size_t Index(Joint joint) { return static_cast<size_t>(joint); }

constexpr float SideSign(Side side) {
  return side == Side::kRight ? 1.0f : -1.0f;
}

constexpr std::array<Side, 2> kSides{Side::kLeft, Side::kRight};

bool Finite(glm::vec3 v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool Finite(const glm::quat& q) {
  return std::isfinite(q.w) && std::isfinite(q.x) && std::isfinite(q.y) &&
         std::isfinite(q.z);
}

float FiniteOr(float value, float fallback) {
  return std::isfinite(value) ? value : fallback;
}

glm::vec3 FiniteOr(glm::vec3 value, glm::vec3 fallback) {
  return Finite(value) ? value : fallback;
}

// The share of a change an exponential blend with time constant `tau`
// covers in `dt`.
float Alpha(float dt, float tau) { return 1.0f - std::exp(-dt / tau); }

// Moves `value` towards `target` by at most `step`.
float MoveTowards(float value, float target, float step) {
  return value < target ? std::min(value + step, target)
                        : std::max(value - step, target);
}

// Eases a linear ramp in [0, 1].
float Ease(float ramp) { return ramp * ramp * (3.0f - 2.0f * ramp); }

float SmoothStep(float edge0, float edge1, float x) {
  const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

// Returns a unit vector perpendicular to `v` (unit).
glm::vec3 AnyPerpendicular(glm::vec3 v) {
  const glm::vec3 other =
      std::abs(v.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : kUp;
  return glm::normalize(glm::cross(v, other));
}

glm::vec3 SafeNormalize(glm::vec3 v, glm::vec3 fallback) {
  const float length = glm::length(v);
  return length > 1.0e-6f && std::isfinite(length) ? v / length : fallback;
}

// Orthonormal basis with `a` as its first axis and `b` (made perpendicular)
// as its second.
glm::mat3 Basis(glm::vec3 a, glm::vec3 b) {
  a = SafeNormalize(a, kUp);
  b = SafeNormalize(b - a * glm::dot(a, b), AnyPerpendicular(a));
  return glm::mat3(a, b, glm::cross(a, b));
}

// The rotation taking `from_a` to `to_a` and `from_b` (as far as it is
// perpendicular to from_a) to `to_b`.
glm::quat FrameRotation(glm::vec3 from_a, glm::vec3 from_b, glm::vec3 to_a,
                        glm::vec3 to_b) {
  const glm::mat3 rotation =
      Basis(to_a, to_b) * glm::transpose(Basis(from_a, from_b));
  return glm::normalize(glm::quat_cast(rotation));
}

// A palm frame: +Z along the fingers, +Y the palm normal.
glm::quat PalmFrame(glm::vec3 fingers, glm::vec3 palm) {
  const glm::mat3 basis = Basis(fingers, palm);  // fingers, palm, f x p
  // Columns x = palm x fingers, y = palm, z = fingers.
  return glm::normalize(
      glm::quat_cast(glm::mat3(-basis[2], basis[1], basis[0])));
}

glm::quat RotationX(float angle) { return glm::angleAxis(angle, kXAxis); }
glm::quat RotationY(float angle) { return glm::angleAxis(angle, kUp); }

float CycleLength(float speed) {
  return std::clamp(kCycleBase + kCyclePerSpeed * speed, kCycleMin, kCycleMax);
}

float StanceFraction(float speed) {
  return glm::mix(kWalkStance, kSprintStance,
                  SmoothStep(kRunFrom, kRunTo, speed));
}

// Rest matrix of socket `index` in prefab space.
glm::mat4 SocketRest(const prefab::Prefab& prefab, int index) {
  const prefab::Socket& socket = prefab.sockets[static_cast<size_t>(index)];
  glm::mat4 matrix = animation::ToMatrix(socket.local);
  int node = socket.node;
  const std::vector<animation::Node>& nodes = prefab.hierarchy.nodes;
  for (size_t guard = 0;
       node >= 0 && static_cast<size_t>(node) < nodes.size() &&
       guard < nodes.size();
       ++guard) {
    matrix =
        animation::ToMatrix(nodes[static_cast<size_t>(node)].rest) * matrix;
    node = nodes[static_cast<size_t>(node)].parent;
  }
  return matrix;
}

}  // namespace

std::string_view JointName(Joint joint) { return kJoints[Index(joint)].name; }

core::Result<CharacterRig, std::string> MakeCharacterRig(
    const animation::Hierarchy& hierarchy,
    std::span<const prefab::Socket> sockets) {
  using Result = core::Result<CharacterRig, std::string>;
  if (!animation::IsValid(hierarchy)) {
    return Result::Error("the hierarchy lists a child before its parent");
  }
  CharacterRig rig;
  rig.node_count = hierarchy.nodes.size();
  rig.rest.reserve(rig.node_count);
  for (const animation::Node& node : hierarchy.nodes) {
    rig.rest.push_back(node.rest);
  }
  for (size_t i = 0; i < kJoints.size(); ++i) {
    const JointInfo& info = kJoints[i];
    const int node = animation::FindNode(hierarchy, info.name);
    if (node < 0) {
      return Result::Error(std::format("no node '{}'", info.name));
    }
    const animation::Node& data = hierarchy.nodes[static_cast<size_t>(node)];
    const int expected =
        info.parent < 0 ? -1 : rig.nodes[static_cast<size_t>(info.parent)];
    if (data.parent != expected) {
      return Result::Error(
          std::format("node '{}' is not parented as its bone", info.name));
    }
    if (std::abs(data.rest.rotation.w) < 0.9999f ||
        glm::length(data.rest.scale - glm::vec3(1.0f)) > 1.0e-4f ||
        !Finite(data.rest.translation)) {
      return Result::Error(
          std::format("node '{}' is rotated or scaled at rest", info.name));
    }
    rig.nodes[i] = node;
    rig.rest_offset[i] = data.rest.translation;
    rig.rest_position[i] =
        info.parent < 0 ? data.rest.translation
                        : rig.rest_position[static_cast<size_t>(info.parent)] +
                              data.rest.translation;
  }

  std::vector<glm::mat4> world(rig.node_count);
  animation::ComputeWorldMatrices(hierarchy, rig.rest, world);
  const auto relative = [&](std::string_view socket_name,
                            Joint joint) -> std::optional<glm::mat4> {
    for (const prefab::Socket& socket : sockets) {
      if (socket.name != socket_name) {
        continue;
      }
      const glm::mat4 node =
          socket.node >= 0 && static_cast<size_t>(socket.node) < world.size()
              ? world[static_cast<size_t>(socket.node)]
              : glm::mat4(1.0f);
      return glm::inverse(world[static_cast<size_t>(rig.nodes[Index(joint)])]) *
             node * animation::ToMatrix(socket.local);
    }
    return std::nullopt;
  };
  for (const Side side : kSides) {
    const std::optional<glm::mat4> palm =
        relative(side == Side::kLeft ? "hand_l" : "hand_r",
                 LimbJoint(side, Limb::kWrist));
    if (!palm) {
      return Result::Error(std::format(
          "no socket '{}'", side == Side::kLeft ? "hand_l" : "hand_r"));
    }
    rig.palm[static_cast<size_t>(side)] = {
        .translation = glm::vec3((*palm)[3]),
        .rotation = glm::normalize(glm::quat_cast(glm::mat3(*palm)))};
  }
  if (const std::optional<glm::mat4> head = relative("head", Joint::kHead)) {
    rig.head_top = glm::vec3((*head)[3]);
  }
  return Result::Ok(std::move(rig));
}

WeaponHold MakeWeaponHold(HeldWeapon kind, const prefab::Prefab& prefab) {
  WeaponHold hold;
  hold.kind = kind;
  if (kind == HeldWeapon::kNone) {
    return hold;
  }
  const asset::Bounds& bounds = prefab.bounds;
  const int sight = prefab::FindSocket(prefab, "sight");
  if (sight >= 0) {
    const glm::mat4 matrix = SocketRest(prefab, sight);
    hold.forward = SafeNormalize(glm::mat3(matrix) * glm::vec3(0, 0, 1),
                                 glm::vec3(0.0f, 0.0f, 1.0f));
    hold.sight = glm::vec3(matrix[3]);
  } else {
    hold.sight = glm::vec3(bounds.center().x, bounds.max.y, bounds.center().z);
  }
  hold.up = SafeNormalize(kUp - hold.forward * glm::dot(kUp, hold.forward),
                          AnyPerpendicular(hold.forward));
  const int grip = prefab::FindSocket(prefab, "grip");
  hold.grip = grip >= 0 ? glm::vec3(SocketRest(prefab, grip)[3])
                        : bounds.center() - hold.up * (0.3f * bounds.size().y);
  const glm::vec3 left = glm::cross(hold.up, hold.forward);
  if (kind == HeldWeapon::kRifle) {
    hold.support = hold.grip + hold.forward * kRifleSupportForward +
                   hold.up * kRifleSupportUp;
    float rear = glm::dot(bounds.min, hold.forward);
    for (int corner = 0; corner < 8; ++corner) {
      const glm::vec3 point((corner & 1) != 0 ? bounds.max.x : bounds.min.x,
                            (corner & 2) != 0 ? bounds.max.y : bounds.min.y,
                            (corner & 4) != 0 ? bounds.max.z : bounds.min.z);
      rear = std::min(rear, glm::dot(point, hold.forward));
    }
    hold.stock = hold.grip +
                 hold.forward *
                     (rear + kStockInset - glm::dot(hold.grip, hold.forward)) +
                 hold.up * kStockUp;
  } else {
    hold.support = hold.grip + left * kPistolSupportSide +
                   hold.forward * kPistolSupportForward -
                   hold.up * kPistolSupportDown;
    hold.stock = hold.grip;
  }
  return hold;
}

TwoBoneIk SolveTwoBoneIk(glm::vec3 root, glm::vec3 target, float upper,
                         float lower, glm::vec3 pole) {
  upper = std::max(FiniteOr(upper, 0.0f), 0.0f);
  lower = std::max(FiniteOr(lower, 0.0f), 0.0f);
  root = FiniteOr(root, glm::vec3(0.0f));
  target = FiniteOr(target, root);
  const glm::vec3 to_target = target - root;
  const float distance = glm::length(to_target);
  const glm::vec3 direction =
      SafeNormalize(to_target, glm::vec3(0.0f, -1.0f, 0.0f));
  const float reach =
      std::clamp(distance, std::abs(upper - lower), upper + lower);
  glm::vec3 bend = FiniteOr(pole, glm::vec3(0.0f));
  bend = SafeNormalize(bend - direction * glm::dot(bend, direction),
                       AnyPerpendicular(direction));
  float cosine = 1.0f;
  if (upper > 1.0e-6f && reach > 1.0e-6f) {
    cosine = (upper * upper + reach * reach - lower * lower) /
             (2.0f * upper * reach);
  }
  cosine = std::clamp(cosine, -1.0f, 1.0f);
  const float sine = std::sqrt(std::max(0.0f, 1.0f - cosine * cosine));
  return {.middle = root + upper * (direction * cosine + bend * sine),
          .end = root + direction * reach};
}

glm::mat4 CharacterRoot(glm::vec3 feet, float yaw) {
  return glm::rotate(glm::translate(glm::mat4(1.0f), feet), yaw, kUp);
}

CharacterAnimator::CharacterAnimator(CharacterRig rig)
    : rig_(std::move(rig)), locals_(rig_.rest) {
  const glm::vec3 chest = rig_.rest_offset[Index(Joint::kNeck)];
  rest_lean_ = std::atan2(chest.z, std::max(chest.y, 1.0e-3f));
  for (size_t i = 0; i < kJoints.size(); ++i) {
    position_[i] = rig_.rest_position[i];
    rotation_[i] = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
  }
}

glm::vec3 CharacterAnimator::palm_position(Side side) const {
  const Joint wrist = LimbJoint(side, Limb::kWrist);
  return position_[Index(wrist)] +
         rotation_[Index(wrist)] *
             rig_.palm[static_cast<size_t>(side)].translation;
}

glm::vec3 CharacterAnimator::head_top() const {
  return position_[Index(Joint::kHead)] +
         rotation_[Index(Joint::kHead)] * rig_.head_top;
}

void CharacterAnimator::Update(const CharacterInput& raw) {
  CharacterInput input = raw;
  float dt = FiniteOr(input.dt, 0.0f);
  dt = std::clamp(dt, 0.0f, kMaxStep);
  input.feet = FiniteOr(input.feet, glm::vec3(0.0f));
  input.velocity = FiniteOr(input.velocity, glm::vec3(0.0f));
  if (glm::length(input.velocity) > kMaxSpeed) {
    input.velocity = glm::normalize(input.velocity) * kMaxSpeed;
  }
  input.yaw = std::remainder(FiniteOr(input.yaw, 0.0f), 2.0f * kPi);
  input.pitch = std::clamp(FiniteOr(input.pitch, 0.0f), -kMaxPitch, kMaxPitch);
  const WeaponHold& hold = input.weapon;
  if (!Finite(hold.forward) || !Finite(hold.up) || !Finite(hold.grip) ||
      !Finite(hold.support) || !Finite(hold.sight) || !Finite(hold.stock) ||
      glm::length(hold.forward) < 0.5f) {
    input.weapon = WeaponHold{.kind = hold.kind};
  }

  root_ = CharacterRoot(input.feet, input.yaw);
  Blend(input, dt);
  PoseLegs();
  PoseSpine();
  PoseWeapon(input, dt);
  PoseArms();
  const bool finite = WriteLocals();
  snap_ = !input.alive || !finite;
}

void CharacterAnimator::Blend(const CharacterInput& input, float dt) {
  const auto alpha = [&](float tau) { return snap_ ? 1.0f : Alpha(dt, tau); };
  pitch_ = input.pitch;
  // Horizontal velocity in character space.
  const glm::vec3 flat(input.velocity.x, 0.0f, input.velocity.z);
  const glm::vec3 local = glm::inverse(RotationY(input.yaw)) * flat;
  const float speed = glm::length(glm::vec2(local.x, local.z));
  speed_ += (speed - speed_) * alpha(kSpeedTau);
  if (speed > 0.1f) {
    const glm::vec2 wanted = glm::vec2(local.x, local.z) / speed;
    const glm::vec2 mixed = glm::mix(direction_, wanted, alpha(kDirectionTau));
    const float length = glm::length(mixed);
    direction_ = length > 1.0e-3f ? mixed / length : wanted;
  }
  // Ramps towards each state, eased; a snap jumps.
  const auto ramp = [&](float& value, bool on, float seconds) {
    const float target = on ? 1.0f : 0.0f;
    value = snap_ ? target : MoveTowards(value, target, dt / seconds);
    return Ease(value);
  };
  const bool was_airborne = air_ > 0.3f;
  crouch_ = ramp(crouch_ramp_, input.crouching, kStanceSeconds);
  air_ = ramp(air_ramp_, !input.on_ground, kAirSeconds);
  if (input.on_ground && was_airborne && !snap_) {
    landing_ = 1.0f;
  } else {
    landing_ = std::max(0.0f, landing_ - dt / kLandingSeconds);
  }
  const bool armed = input.weapon.kind != HeldWeapon::kNone;
  if (armed) {
    kind_ = input.weapon.kind;
    hold_ = input.weapon;
  }
  aim_ = ramp(aim_ramp_, armed && input.aiming, kAimSeconds);
  armed_ = ramp(armed_ramp_, armed, kArmedSeconds);
  const float moving = SmoothStep(kWalkFrom, kWalkTo, speed_);
  walk_ramp_ =
      snap_ ? moving : MoveTowards(walk_ramp_, moving, dt / kWalkSeconds);
  walk_ = Ease(walk_ramp_) * (1.0f - air_);
  if (air_ < 0.5f) {
    phase_ = std::fmod(phase_ + speed * dt / Cycle(), 1.0f);
  }
  breath_ = std::fmod(breath_ + dt, 1.0f / kBreathHz);
}

void CharacterAnimator::PlaceChild(Joint joint) {
  const int parent = kJoints[Index(joint)].parent;
  const auto p = static_cast<size_t>(parent);
  position_[Index(joint)] =
      position_[p] + rotation_[p] * rig_.rest_offset[Index(joint)];
}

void CharacterAnimator::PoseLimb(Joint upper, Joint lower, Joint end,
                                 glm::vec3 target, glm::vec3 pole,
                                 glm::vec3 fold) {
  const glm::vec3 root = position_[Index(upper)];
  const glm::vec3 upper_rest = rig_.rest_offset[Index(lower)];
  const glm::vec3 lower_rest = rig_.rest_offset[Index(end)];
  const TwoBoneIk ik = SolveTwoBoneIk(root, target, glm::length(upper_rest),
                                      glm::length(lower_rest), pole);
  const glm::vec3 r1 = SafeNormalize(upper_rest, glm::vec3(0, -1, 0));
  const glm::vec3 r2 = SafeNormalize(lower_rest, r1);
  const glm::vec3 d1 = SafeNormalize(ik.middle - root, r1);
  const glm::vec3 d2 = SafeNormalize(ik.end - ik.middle, d1);
  glm::vec3 hinge = glm::cross(d1, d2);
  if (glm::length(hinge) < 1.0e-4f) {
    hinge = glm::cross(d1, fold);
  }
  hinge = SafeNormalize(hinge, AnyPerpendicular(d1));
  const glm::vec3 h1 =
      SafeNormalize(glm::cross(r1, fold), AnyPerpendicular(r1));
  const glm::vec3 h2 =
      SafeNormalize(glm::cross(r2, fold), AnyPerpendicular(r2));
  rotation_[Index(upper)] = FrameRotation(r1, h1, d1, hinge);
  PlaceChild(lower);
  rotation_[Index(lower)] = FrameRotation(r2, h2, d2, hinge);
  PlaceChild(end);
}

void CharacterAnimator::PoseLegs() {
  const float cycle = Cycle();
  const float stance = StanceFraction(speed_);
  const float amplitude = 0.5f * stance * cycle;
  const float walk = walk_;
  const float run = SmoothStep(kRunFrom, kRunTo, speed_);
  const float lift = kStepLift + kRunLift * run;
  const glm::vec3 along(direction_.x, 0.0f, direction_.y);

  for (const Side side : kSides) {
    const size_t s = static_cast<size_t>(side);
    const glm::vec3 rest_ankle =
        rig_.rest_position[Index(LimbJoint(side, Limb::kAnkle))];
    const float width = glm::mix(kStanceWidth, 1.0f, crouch_) *
                        (1.0f + kStrafeWiden * std::abs(direction_.x) * walk);
    glm::vec3 base(rest_ankle.x * width, rest_ankle.y, rest_ankle.z);
    base.z +=
        crouch_ * (side == Side::kLeft ? kCrouchLeftFoot : kCrouchRightFoot);
    const float u =
        std::fmod(phase_ + (side == Side::kRight ? 0.5f : 0.0f), 1.0f);
    float offset = 0.0f;
    float up = 0.0f;
    if (u < stance) {
      offset = amplitude - 2.0f * amplitude * u / stance;
    } else {
      const float t = (u - stance) / (1.0f - stance);
      offset = -amplitude + 2.0f * amplitude * t * t * (3.0f - 2.0f * t);
      up = lift * std::sin(kPi * t);
    }
    const glm::vec3 ground = base + along * (offset * walk) + kUp * (up * walk);
    const glm::vec3 air =
        base + glm::vec3(0.0f, kAirTuck,
                         side == Side::kLeft ? kAirLeftFoot : kAirRightFoot);
    glm::vec3 target = glm::mix(ground, air, air_);
    target.y = std::max(target.y, rest_ankle.y);
    foot_target_[s] = target;
    planted_[s] = u < stance && walk > 0.999f && air_ < 1.0e-3f;
  }

  // The pelvis: lowered by crouch, bob and landing, never so high that a
  // foot target is out of reach.
  const glm::vec3 rest_root = rig_.rest_position[Index(Joint::kRoot)];
  const float twist = kPelvisTwist * walk * std::sin(2.0f * kPi * phase_);
  rotation_[Index(Joint::kRoot)] =
      RotationY(twist) * RotationY(kPelvisLeanShare * TorsoTwist()) *
      RotationX(kPelvisLeanShare * (SpinePitch() - Lean()));
  float height =
      glm::mix(rest_root.y - kStandBend, rest_root.y * kCrouchPelvis, crouch_);
  height -= walk * (kWalkBob + kRunBob * run) *
            (0.5f - 0.5f * std::cos(4.0f * kPi * phase_));
  if (landing_ > 0.0f) {
    height -= kLandingDip * std::sin(kPi * (1.0f - landing_));
  }
  glm::vec3 pelvis(walk * kPelvisSway * std::sin(2.0f * kPi * phase_), height,
                   rest_root.z + crouch_ * kCrouchPelvisBack);
  for (const Side side : kSides) {
    const glm::vec3 hip_offset =
        rotation_[Index(Joint::kRoot)] *
        (rig_.rest_offset[Index(Joint::kHip)] +
         rig_.rest_offset[Index(LimbJoint(side, Limb::kThigh))]);
    const float leg =
        kLegReach *
        (glm::length(rig_.rest_offset[Index(LimbJoint(side, Limb::kKnee))]) +
         glm::length(rig_.rest_offset[Index(LimbJoint(side, Limb::kAnkle))]));
    const glm::vec3 to_foot =
        foot_target_[static_cast<size_t>(side)] - (pelvis + hip_offset);
    const float flat = glm::length(glm::vec2(to_foot.x, to_foot.z));
    const float drop = std::sqrt(std::max(leg * leg - flat * flat, 0.0f));
    pelvis.y = std::min(pelvis.y, foot_target_[static_cast<size_t>(side)].y +
                                      drop - hip_offset.y);
  }
  position_[Index(Joint::kRoot)] = pelvis;
  rotation_[Index(Joint::kHip)] = rotation_[Index(Joint::kRoot)];
  PlaceChild(Joint::kHip);

  for (const Side side : kSides) {
    const Joint thigh = LimbJoint(side, Limb::kThigh);
    PlaceChild(thigh);
    const glm::vec3 pole(SideSign(side) * kKneeOut, 0.0f, -1.0f);
    PoseLimb(thigh, LimbJoint(side, Limb::kKnee), LimbJoint(side, Limb::kAnkle),
             foot_target_[static_cast<size_t>(side)], pole,
             glm::vec3(0.0f, 0.0f, 1.0f));
    const glm::quat foot = RotationX(-kAirToe * air_);
    rotation_[Index(LimbJoint(side, Limb::kAnkle))] = foot;
    PlaceChild(LimbJoint(side, Limb::kToe));
    rotation_[Index(LimbJoint(side, Limb::kToe))] = foot;
  }
}

float CharacterAnimator::Cycle() const {
  return CycleLength(speed_) * (1.0f - kStrafeShorten * std::abs(direction_.x));
}

float CharacterAnimator::Lean() const {
  // Leaning into the walk only when walking forwards.
  const float forwards = std::max(0.0f, -direction_.y) * walk_;
  return kCrouchLean * crouch_ +
         forwards * ((rest_lean_ + kWalkLean) *
                         SmoothStep(0.0f, kWalkLeanSpeed, speed_) +
                     kSprintLean * SmoothStep(kWalkLeanSpeed, kRunTo, speed_));
}

float CharacterAnimator::SpinePitch() const {
  return std::clamp(kSpinePitchShare * pitch_, -kMaxSpinePitch, kMaxSpinePitch);
}

float CharacterAnimator::TorsoTwist() const {
  return kind_ == HeldWeapon::kRifle ? kRifleTwist * armed_ : 0.0f;
}

void CharacterAnimator::PoseSpine() {
  const float breath =
      kBreath * (1.0f - walk_) * std::sin(2.0f * kPi * kBreathHz * breath_);
  const glm::quat ribs =
      RotationY(TorsoTwist()) * RotationX(SpinePitch() - Lean() + breath);
  rotation_[Index(Joint::kRibs)] = ribs;
  PlaceChild(Joint::kRibs);
  // The head looks along the pitch, level however the body leans.
  const float head_pitch =
      std::clamp(kHeadPitchShare * pitch_, -kMaxHeadPitch, kMaxHeadPitch);
  const glm::quat head = RotationX(head_pitch);
  rotation_[Index(Joint::kNeck)] = glm::slerp(ribs, head, 0.5f);
  PlaceChild(Joint::kNeck);
  rotation_[Index(Joint::kHead)] = head;
  PlaceChild(Joint::kHead);
  for (const Side side : kSides) {
    const Joint shoulder = LimbJoint(side, Limb::kShoulder);
    rotation_[Index(shoulder)] = ribs;
    PlaceChild(shoulder);
    PlaceChild(LimbJoint(side, Limb::kUpperArm));
  }
}

void CharacterAnimator::PoseWeapon(const CharacterInput& input, float dt) {
  const glm::vec3 shoulders =
      0.5f * (position_[Index(LimbJoint(Side::kLeft, Limb::kUpperArm))] +
              position_[Index(LimbJoint(Side::kRight, Limb::kUpperArm))]);
  const glm::quat look = RotationX(pitch_);
  const glm::quat ready =
      RotationX(std::max(pitch_ - kLowReadyDip, kLowReadyMinPitch));
  // The weapon's own frame turned onto the figure's forward and up.
  const glm::quat upright =
      FrameRotation(hold_.forward, hold_.up, kForward, kUp);
  const bool rifle = kind_ == HeldWeapon::kRifle;
  // Places anchor point `anchor` (prefab space) at `at` with the weapon
  // turned by `turn`; returns the grip frame.
  const auto place = [&](glm::vec3 anchor, glm::vec3 at, glm::quat turn) {
    const glm::quat rotation = turn * upright;
    const glm::vec3 origin = at - rotation * anchor;
    return Frame{.position = origin + rotation * hold_.grip,
                 .rotation = rotation};
  };
  Frame aimed;
  Frame lowered;
  if (rifle) {
    aimed = place(hold_.stock, shoulders + kRifleAimStock, look);
    lowered = place(hold_.stock, shoulders + kRifleReadyStock, ready);
  } else {
    const float pivot =
        kPistolAimPivot - kPistolPivotDropUp * std::max(0.0f, std::sin(pitch_));
    aimed = place(hold_.sight,
                  shoulders + glm::vec3(0.0f, pivot, 0.0f) +
                      look * kForward * kPistolAimReach,
                  look);
    lowered = place(
        hold_.sight,
        shoulders + kPistolReadyPivot + ready * kForward * kPistolReadyReach,
        ready);
  }
  const Frame wanted{
      .position = glm::mix(lowered.position, aimed.position, aim_),
      .rotation = glm::slerp(lowered.rotation, aimed.rotation, aim_)};
  const float alpha = snap_ ? 1.0f : Alpha(dt, kWeaponTau);
  grip_.position = glm::mix(grip_.position, wanted.position, alpha);
  grip_.rotation =
      glm::normalize(glm::slerp(grip_.rotation, wanted.rotation, alpha));

  const glm::quat rotation = grip_.rotation;
  const glm::vec3 origin = grip_.position - rotation * hold_.grip;
  if (input.weapon.kind != HeldWeapon::kNone) {
    weapon_ =
        glm::translate(glm::mat4(1.0f), origin) * glm::mat4_cast(rotation);
  } else {
    weapon_.reset();
  }

  // The hands on the weapon (the frames of the N15 figures' grips) ...
  const glm::vec3 forward = rotation * hold_.forward;
  const glm::vec3 up = rotation * hold_.up;
  const glm::vec3 right = glm::cross(forward, up);
  std::array<Frame, 2> armed;
  const size_t l = static_cast<size_t>(Side::kLeft);
  const size_t r = static_cast<size_t>(Side::kRight);
  armed[r].position = origin + rotation * hold_.grip;
  armed[l].position = origin + rotation * hold_.support;
  if (rifle) {
    armed[r].rotation = PalmFrame(0.75f * forward - 0.66f * up, -right);
    armed[l].rotation = PalmFrame(right, up);
  } else {
    armed[r].rotation = PalmFrame(forward - 0.4f * up, -right);
    armed[l].rotation = PalmFrame(0.8f * forward - 0.8f * up, right);
  }
  // ... or hanging by the sides, swinging opposite the legs.
  const float walk = walk_;
  const float swing = kArmSwing * walk * std::sin(2.0f * kPi * phase_);
  for (const Side side : kSides) {
    const size_t s = static_cast<size_t>(side);
    const float sign = SideSign(side);
    const glm::vec3 shoulder =
        position_[Index(LimbJoint(side, Limb::kUpperArm))];
    const Frame loose{
        .position = shoulder + glm::vec3(sign * kHandDown.x, kHandDown.y,
                                         kHandDown.z + sign * swing),
        .rotation = PalmFrame(glm::vec3(0.0f, -1.0f, -0.15f),
                              glm::vec3(-sign, 0.0f, 0.0f))};
    palm_target_[s] = {
        .position = glm::mix(loose.position, armed[s].position, armed_),
        .rotation = glm::normalize(
            glm::slerp(loose.rotation, armed[s].rotation, armed_))};
  }
}

void CharacterAnimator::PoseArms() {
  for (const Side side : kSides) {
    const size_t s = static_cast<size_t>(side);
    const animation::Transform& palm = rig_.palm[s];
    const glm::quat hand =
        glm::normalize(palm_target_[s].rotation * glm::inverse(palm.rotation));
    const glm::vec3 wrist = palm_target_[s].position - hand * palm.translation;
    const glm::vec3 pole(SideSign(side) * kElbowPole.x, kElbowPole.y,
                         kElbowPole.z + kElbowBackUnarmed * (1.0f - armed_));
    PoseLimb(LimbJoint(side, Limb::kUpperArm), LimbJoint(side, Limb::kForearm),
             LimbJoint(side, Limb::kWrist), wrist, pole, kForward);
    rotation_[Index(LimbJoint(side, Limb::kWrist))] = hand;
  }
}

bool CharacterAnimator::WriteLocals() {
  std::copy(rig_.rest.begin(), rig_.rest.end(), locals_.begin());
  bool finite = true;
  for (size_t i = 0; i < kJoints.size(); ++i) {
    animation::Transform& local = locals_[static_cast<size_t>(rig_.nodes[i])];
    const int parent = kJoints[i].parent;
    if (parent < 0) {
      local.translation = position_[i];
      local.rotation = rotation_[i];
    } else {
      local.rotation = glm::normalize(
          glm::inverse(rotation_[static_cast<size_t>(parent)]) * rotation_[i]);
    }
    finite = finite && Finite(local.translation) && Finite(local.rotation);
  }
  if (!finite) {
    // Never hand out NaN: fall back to the rest pose and start over.
    std::copy(rig_.rest.begin(), rig_.rest.end(), locals_.begin());
    for (size_t i = 0; i < kJoints.size(); ++i) {
      position_[i] = rig_.rest_position[i];
      rotation_[i] = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
    weapon_.reset();
    grip_ = Frame{};
  }
  return finite;
}

}  // namespace blue::game
