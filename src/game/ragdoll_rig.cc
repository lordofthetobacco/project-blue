#include "game/ragdoll_rig.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <limits>
#include <numbers>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <glm/common.hpp>
#include <glm/ext/quaternion_float.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "animation/hierarchy.h"
#include "animation/transform.h"
#include "asset/mesh_data.h"
#include "core/result.h"
#include "game/character_animator.h"
#include "physics/physics_world.h"
#include "physics/ragdoll.h"
#include "physics/shape_cooking.h"
#include "prefab/prefab.h"

namespace blue::game {
namespace {

using Result = core::Result<RagdollRig, std::string>;

// The pose conversions keep their node poses on the stack.
constexpr size_t kMaxNodes = 64;
constexpr float kPi = std::numbers::pi_v<float>;

constexpr glm::vec3 kXAxis{1.0f, 0.0f, 0.0f};
constexpr glm::vec3 kYAxis{0.0f, 1.0f, 0.0f};
// Figure space faces -Z; arms fold forwards, legs fold backwards.
constexpr glm::vec3 kForward{0.0f, 0.0f, -1.0f};
constexpr glm::vec3 kBackward{0.0f, 0.0f, 1.0f};

// The figure height the radii below are given for; they scale with the
// model's real height.
constexpr float kReferenceHeight = 1.8f;

// Capsule radii (m) at the reference height.
constexpr float kHeadRadius = 0.09f;
constexpr float kUpperArmRadius = 0.050f;
constexpr float kForearmRadius = 0.042f;
constexpr float kThighRadius = 0.070f;
constexpr float kShinRadius = 0.050f;
// The forearm capsule reaches this far past the wrist, over the hand (m at
// the reference height).
constexpr float kHandReach = 0.08f;
// Smallest cylinder part of a limb capsule (m).
constexpr float kMinHalfHeight = 0.01f;

// Box fits (the part's mesh bounds times a factor, per axis).
constexpr float kPelvisWidthFactor = 0.9f;
constexpr float kChestWidthFactor = 0.95f;
constexpr float kChestDepthFactor = 0.9f;
constexpr float kFootFactor = 0.95f;
// The pelvis box reaches this far below the hip joints and short of the chest
// joint; the chest box starts this far below its frame and ends short of the
// neck joint (m).
constexpr float kPelvisBelowHips = 0.03f;
constexpr float kPelvisBelowRibs = 0.02f;
constexpr float kChestBelowFrame = 0.02f;
constexpr float kChestBelowNeck = 0.02f;

// Joints overshoot their limits by about w * dt / 2, so the default 30 rad/s
// lets a cone overshoot by around 10 degrees; 15 rad/s halves that.
constexpr float kMaxAngularVelocity = 15.0f;  // rad/s

// Masses (kg), 71.5 in total.
constexpr float kPelvisMass = 12.0f;
constexpr float kChestMass = 20.0f;
constexpr float kHeadMass = 5.0f;
constexpr float kUpperArmMass = 2.5f;
constexpr float kForearmMass = 2.0f;
constexpr float kThighMass = 7.5f;
constexpr float kShinMass = 4.0f;
constexpr float kFootMass = 1.25f;

// Joint limits (degrees) and friction torques (N*m).
struct SwingTwistLimits {
  float cone_reference;
  float cone_side;
  float twist;  // +-
  float friction;
};
constexpr SwingTwistLimits kSpine{20.0f, 30.0f, 25.0f, 6.0f};
constexpr SwingTwistLimits kNeck{30.0f, 45.0f, 45.0f, 2.0f};
constexpr SwingTwistLimits kShoulder{110.0f, 110.0f, 175.0f, 2.0f};
constexpr SwingTwistLimits kHip{40.0f, 75.0f, 45.0f, 4.0f};
constexpr SwingTwistLimits kAnkle{30.0f, 60.0f, 35.0f, 1.0f};
constexpr float kElbowMin = -5.0f;
constexpr float kElbowMax = 155.0f;
constexpr float kElbowFriction = 1.5f;
constexpr float kKneeMin = -5.0f;
constexpr float kKneeMax = 155.0f;
constexpr float kKneeFriction = 2.0f;

// The shoulder cone is centred on the arm hanging down, slightly forwards and
// outwards (x is mirrored by side); the hip cone on the thigh bent forwards.
constexpr glm::vec3 kShoulderCone{0.35f, -0.80f, -0.50f};
constexpr float kHipConeFlexionDegrees = 35.0f;

struct PartInfo {
  Joint joint;  // The part's frame node.
  int parent;   // Part index, -1 for the pelvis.
  float mass;
};

// In RagdollPart order.
constexpr std::array<PartInfo, kRagdollPartCount> kParts{{
    {Joint::kRoot, -1, kPelvisMass},
    {Joint::kRibs, 0, kChestMass},
    {Joint::kNeck, 1, kHeadMass},
    {Joint::kLeftUpperArm, 1, kUpperArmMass},
    {Joint::kLeftForearm, 3, kForearmMass},
    {Joint::kRightUpperArm, 1, kUpperArmMass},
    {Joint::kRightForearm, 5, kForearmMass},
    {Joint::kLeftThigh, 0, kThighMass},
    {Joint::kLeftKnee, 7, kShinMass},
    {Joint::kLeftAnkle, 8, kFootMass},
    {Joint::kRightThigh, 0, kThighMass},
    {Joint::kRightKnee, 10, kShinMass},
    {Joint::kRightAnkle, 11, kFootMass},
}};

constexpr size_t Index(Joint joint) { return static_cast<size_t>(joint); }
constexpr size_t Index(RagdollPart part) { return static_cast<size_t>(part); }

bool Finite(glm::vec3 v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// An axis-aligned box that grows by points; empty until the first one.
struct Box {
  glm::vec3 min{std::numeric_limits<float>::max()};
  glm::vec3 max{std::numeric_limits<float>::lowest()};

  [[nodiscard]] bool empty() const {
    return min.x > max.x || min.y > max.y || min.z > max.z;
  }
  void Add(glm::vec3 point) {
    min = glm::min(min, point);
    max = glm::max(max, point);
  }
  [[nodiscard]] glm::vec3 center() const { return (min + max) * 0.5f; }
  [[nodiscard]] glm::vec3 half() const { return (max - min) * 0.5f; }
};

// Returns the unit part of `v` perpendicular to the unit vector `axis`.
glm::vec3 Perpendicular(glm::vec3 v, glm::vec3 axis) {
  const glm::vec3 flat = v - axis * glm::dot(axis, v);
  if (glm::length(flat) > 1.0e-4f) {
    return glm::normalize(flat);
  }
  const glm::vec3 other = std::abs(axis.x) < 0.9f ? kXAxis : kYAxis;
  return glm::normalize(glm::cross(axis, other));
}

// The rotation taking +Y to the unit vector `direction`.
glm::quat RotationFromY(glm::vec3 direction) {
  const float d = glm::dot(kYAxis, direction);
  if (d < -0.9999f) {
    return glm::angleAxis(kPi, kXAxis);
  }
  const glm::vec3 axis = glm::cross(kYAxis, direction);
  return glm::normalize(glm::quat(1.0f + d, axis.x, axis.y, axis.z));
}

physics::ShapeCookDesc BoxShape(glm::vec3 center, glm::vec3 half_extents) {
  physics::ShapeCookDesc shape;
  shape.kind = physics::CookedShapeKind::kBox;
  shape.center = center;
  shape.half_extents = half_extents;
  return shape;
}

physics::ShapeCookDesc CapsuleShape(glm::vec3 center, glm::quat rotation,
                                    float radius, float half_height) {
  physics::ShapeCookDesc shape;
  shape.kind = physics::CookedShapeKind::kCapsule;
  shape.center = center;
  shape.rotation = rotation;
  shape.radius = radius;
  shape.half_height = half_height;
  return shape;
}

// A capsule running from the part frame's origin along `bone` (the rest
// offset of the far joint), `extra` metres further.
physics::ShapeCookDesc LimbShape(glm::vec3 bone, float extra, float radius) {
  const float length = glm::length(bone) + extra;
  const glm::vec3 direction = glm::normalize(bone);
  return CapsuleShape(direction * (0.5f * length), RotationFromY(direction),
                      radius, std::max(0.5f * length - radius, kMinHalfHeight));
}

physics::RagdollJointDesc SwingTwistJoint(glm::vec3 anchor,
                                          physics::RagdollJointFrame parent,
                                          physics::RagdollJointFrame child,
                                          const SwingTwistLimits& limits) {
  physics::RagdollJointDesc joint;
  joint.kind = physics::RagdollJointKind::kSwingTwist;
  joint.anchor = anchor;
  joint.parent = parent;
  joint.child = child;
  joint.cone_reference_degrees = limits.cone_reference;
  joint.cone_side_degrees = limits.cone_side;
  joint.twist_min_degrees = -limits.twist;
  joint.twist_max_degrees = limits.twist;
  joint.friction_torque = limits.friction;
  return joint;
}

// A hinge between a bone of rest direction `upper` and one of `lower` (unit),
// folding about the axes the animator's PoseLimb() uses: cross(bone, fold)
// per bone. Every pose the animator makes then has a hinge angle >= 0.
physics::RagdollJointDesc HingeJoint(glm::vec3 anchor, glm::vec3 upper,
                                     glm::vec3 lower, glm::vec3 fold,
                                     float min_degrees, float max_degrees,
                                     float friction) {
  physics::RagdollJointDesc joint;
  joint.kind = physics::RagdollJointKind::kHinge;
  joint.anchor = anchor;
  joint.parent = {.primary = glm::normalize(glm::cross(upper, fold)),
                  .reference = upper};
  joint.child = {.primary = glm::normalize(glm::cross(lower, fold)),
                 .reference = lower};
  joint.hinge_min_degrees = min_degrees;
  joint.hinge_max_degrees = max_degrees;
  joint.friction_torque = friction;
  return joint;
}

physics::RagdollJointFrame SwingFrame(glm::vec3 primary, glm::vec3 about) {
  return {.primary = primary, .reference = Perpendicular(about, primary)};
}

// What MakeRagdollRig works from.
struct Source {
  const CharacterRig& rig;
  std::vector<Box> part_bounds;  // Per part, in its frame; mesh bounds.
  float scale = 1.0f;            // Model height / kReferenceHeight.

  [[nodiscard]] glm::vec3 position(Joint joint) const {
    return rig.rest_position[Index(joint)];
  }
  [[nodiscard]] glm::vec3 offset(Joint joint) const {
    return rig.rest_offset[Index(joint)];
  }
  // The rest direction of the bone that ends at `joint` (unit).
  [[nodiscard]] glm::vec3 bone(Joint joint) const {
    return glm::normalize(offset(joint));
  }
};

// The pelvis box: the Hip's footprint, from just under the hip joints to just
// under the chest joint.
physics::ShapeCookDesc PelvisShape(const Source& s, const Box& bounds) {
  const glm::vec3 root = s.position(Joint::kRoot);
  const float low = std::min(s.position(Joint::kLeftThigh).y,
                             s.position(Joint::kRightThigh).y) -
                    root.y - kPelvisBelowHips;
  const float high = s.position(Joint::kRibs).y - root.y - kPelvisBelowRibs;
  const glm::vec3 c = bounds.center();
  const glm::vec3 h = bounds.half() * kPelvisWidthFactor;
  return BoxShape({c.x, 0.5f * (low + high), c.z},
                  {h.x, 0.5f * (high - low), h.z});
}

// The chest box: the Ribs' footprint, from just under the frame to just under
// the neck joint.
physics::ShapeCookDesc ChestShape(const Source& s, const Box& bounds) {
  const float low = -kChestBelowFrame;
  const float high =
      s.position(Joint::kNeck).y - s.position(Joint::kRibs).y - kChestBelowNeck;
  const glm::vec3 c = bounds.center();
  const glm::vec3 h = bounds.half();
  return BoxShape(
      {c.x, 0.5f * (low + high), c.z},
      {h.x * kChestWidthFactor, 0.5f * (high - low), h.z * kChestDepthFactor});
}

// The head capsule along Y over the head mesh.
physics::ShapeCookDesc HeadShape(const Source& s, const Box& bounds) {
  const glm::vec3 h = bounds.half();
  const float radius = std::min(kHeadRadius * s.scale, std::min(h.x, h.z));
  return CapsuleShape(bounds.center(), glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
                      radius, std::max(h.y - radius, kMinHalfHeight));
}

physics::ShapeCookDesc FootShape(const Box& bounds) {
  return BoxShape(bounds.center(), bounds.half() * kFootFactor);
}

// The shape of `part`; the box parts use the mesh bounds of their nodes.
physics::ShapeCookDesc ShapeOf(const Source& s, RagdollPart part) {
  const Box& bounds = s.part_bounds[Index(part)];
  const float k = s.scale;
  switch (part) {
    case RagdollPart::kPelvis:
      return PelvisShape(s, bounds);
    case RagdollPart::kChest:
      return ChestShape(s, bounds);
    case RagdollPart::kHead:
      return HeadShape(s, bounds);
    case RagdollPart::kLeftUpperArm:
    case RagdollPart::kRightUpperArm: {
      const Joint forearm = part == RagdollPart::kLeftUpperArm
                                ? Joint::kLeftForearm
                                : Joint::kRightForearm;
      return LimbShape(s.offset(forearm), 0.0f, kUpperArmRadius * k);
    }
    case RagdollPart::kLeftForearm:
    case RagdollPart::kRightForearm: {
      const Joint wrist = part == RagdollPart::kLeftForearm
                              ? Joint::kLeftWrist
                              : Joint::kRightWrist;
      return LimbShape(s.offset(wrist), kHandReach * k, kForearmRadius * k);
    }
    case RagdollPart::kLeftThigh:
    case RagdollPart::kRightThigh: {
      const Joint knee = part == RagdollPart::kLeftThigh ? Joint::kLeftKnee
                                                         : Joint::kRightKnee;
      return LimbShape(s.offset(knee), 0.0f, kThighRadius * k);
    }
    case RagdollPart::kLeftShin:
    case RagdollPart::kRightShin: {
      const Joint ankle = part == RagdollPart::kLeftShin ? Joint::kLeftAnkle
                                                         : Joint::kRightAnkle;
      return LimbShape(s.offset(ankle), 0.0f, kShinRadius * k);
    }
    case RagdollPart::kLeftFoot:
    case RagdollPart::kRightFoot:
      return FootShape(bounds);
  }
  return {};
}

// The joint of `part` to its parent (unused for the pelvis).
physics::RagdollJointDesc JointOf(const Source& s, RagdollPart part) {
  const Joint joint = kParts[Index(part)].joint;
  const glm::vec3 anchor = s.position(joint);
  const physics::RagdollJointFrame up{.primary = kYAxis, .reference = kXAxis};
  switch (part) {
    case RagdollPart::kPelvis:
      return {};
    case RagdollPart::kChest:
      return SwingTwistJoint(anchor, up, up, kSpine);
    case RagdollPart::kHead:
      return SwingTwistJoint(anchor, up, up, kNeck);
    case RagdollPart::kLeftUpperArm:
    case RagdollPart::kRightUpperArm: {
      const bool left = part == RagdollPart::kLeftUpperArm;
      // The model's right is +X.
      const float side = left ? -1.0f : 1.0f;
      const glm::vec3 centre = glm::normalize(
          glm::vec3(side * kShoulderCone.x, kShoulderCone.y, kShoulderCone.z));
      const glm::vec3 t =
          s.bone(left ? Joint::kLeftForearm : Joint::kRightForearm);
      return SwingTwistJoint(anchor, SwingFrame(centre, kForward),
                             SwingFrame(t, kForward), kShoulder);
    }
    case RagdollPart::kLeftForearm:
    case RagdollPart::kRightForearm: {
      const bool left = part == RagdollPart::kLeftForearm;
      return HingeJoint(
          anchor, s.bone(left ? Joint::kLeftForearm : Joint::kRightForearm),
          s.bone(left ? Joint::kLeftWrist : Joint::kRightWrist), kForward,
          kElbowMin, kElbowMax, kElbowFriction);
    }
    case RagdollPart::kLeftThigh:
    case RagdollPart::kRightThigh: {
      const glm::vec3 t =
          s.bone(part == RagdollPart::kLeftThigh ? Joint::kLeftKnee
                                                 : Joint::kRightKnee);
      const glm::vec3 centre =
          glm::angleAxis(glm::radians(kHipConeFlexionDegrees), kXAxis) * t;
      return SwingTwistJoint(anchor, SwingFrame(centre, kXAxis),
                             SwingFrame(t, kXAxis), kHip);
    }
    case RagdollPart::kLeftShin:
    case RagdollPart::kRightShin: {
      const bool left = part == RagdollPart::kLeftShin;
      return HingeJoint(anchor,
                        s.bone(left ? Joint::kLeftKnee : Joint::kRightKnee),
                        s.bone(left ? Joint::kLeftAnkle : Joint::kRightAnkle),
                        kBackward, kKneeMin, kKneeMax, kKneeFriction);
    }
    case RagdollPart::kLeftFoot:
    case RagdollPart::kRightFoot: {
      const glm::vec3 t =
          s.bone(part == RagdollPart::kLeftFoot ? Joint::kLeftAnkle
                                                : Joint::kRightAnkle);
      const physics::RagdollJointFrame frame = SwingFrame(t, kXAxis);
      return SwingTwistJoint(anchor, frame, frame, kAnkle);
    }
  }
  return {};
}

// A node pose in figure space; the nodes the conversions walk are rigid
// (joint nodes have unit scale).
struct NodePose {
  glm::vec3 position{0.0f};
  glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

NodePose Compose(const NodePose& parent, glm::vec3 translation,
                 glm::quat rotation) {
  return {.position = parent.position + parent.rotation * translation,
          .rotation = parent.rotation * rotation};
}

// The number of nodes the conversions process.
size_t NodeCount(const RagdollRig& rig, size_t locals) {
  return std::min(
      {rig.node_parent.size(), rig.node_part.size(), locals, kMaxNodes});
}

}  // namespace

Result MakeRagdollRig(const CharacterRig& rig, const prefab::Prefab& prefab) {
  const std::vector<animation::Node>& nodes = prefab.hierarchy.nodes;
  if (nodes.size() != rig.node_count || nodes.size() != rig.rest.size()) {
    return Result::Error("the rig was not made from this prefab");
  }
  if (nodes.size() > kMaxNodes) {
    return Result::Error(std::format("the model has {} nodes, at most {}",
                                     nodes.size(), kMaxNodes));
  }
  const float height = prefab.bounds.size().y;
  if (!std::isfinite(height) || height < 0.1f) {
    return Result::Error("the model has no height");
  }

  RagdollRig out;
  out.node_part.assign(nodes.size(), -1);
  out.node_parent.reserve(nodes.size());
  for (const animation::Node& node : nodes) {
    out.node_parent.push_back(node.parent);
  }
  for (size_t p = 0; p < kParts.size(); ++p) {
    const Joint joint = kParts[p].joint;
    out.part_joint[p] = joint;
    out.part_node[p] = rig.nodes[Index(joint)];
    out.node_part[static_cast<size_t>(out.part_node[p])] = static_cast<int>(p);
  }

  // The mesh bounds of every part: its own nodes and those that ride on it.
  std::vector<glm::mat4> world(nodes.size());
  animation::ComputeWorldMatrices(prefab.hierarchy, rig.rest, world);
  Source source{.rig = rig,
                .part_bounds = std::vector<Box>(kParts.size()),
                .scale = height / kReferenceHeight};
  for (const prefab::Mesh& mesh : prefab.meshes) {
    if (mesh.node < 0 || static_cast<size_t>(mesh.node) >= nodes.size()) {
      continue;
    }
    int owner = -1;
    for (int n = mesh.node; n >= 0 && owner < 0;
         n = out.node_parent[static_cast<size_t>(n)]) {
      owner = out.node_part[static_cast<size_t>(n)];
    }
    if (owner < 0) {
      continue;
    }
    const glm::mat4 to_part = glm::inverse(world[static_cast<size_t>(
                                  out.part_node[static_cast<size_t>(owner)])]) *
                              world[static_cast<size_t>(mesh.node)];
    for (int corner = 0; corner < 8; ++corner) {
      const glm::vec3 point(
          (corner & 1) != 0 ? mesh.bounds.max.x : mesh.bounds.min.x,
          (corner & 2) != 0 ? mesh.bounds.max.y : mesh.bounds.min.y,
          (corner & 4) != 0 ? mesh.bounds.max.z : mesh.bounds.min.z);
      source.part_bounds[static_cast<size_t>(owner)].Add(
          glm::vec3(to_part * glm::vec4(point, 1.0f)));
    }
  }
  for (const RagdollPart part :
       {RagdollPart::kPelvis, RagdollPart::kChest, RagdollPart::kHead,
        RagdollPart::kLeftFoot, RagdollPart::kRightFoot}) {
    if (source.part_bounds[Index(part)].empty()) {
      return Result::Error(std::format(
          "no mesh bounds for ragdoll part {} (node '{}')",
          static_cast<int>(part),
          nodes[static_cast<size_t>(out.part_node[Index(part)])].name));
    }
  }
  for (const Joint joint :
       {Joint::kLeftForearm, Joint::kLeftWrist, Joint::kLeftKnee,
        Joint::kLeftAnkle, Joint::kRightForearm, Joint::kRightWrist,
        Joint::kRightKnee, Joint::kRightAnkle}) {
    if (!(glm::length(source.offset(joint)) > 1.0e-4f)) {
      return Result::Error(
          std::format("bone of '{}' has no length", JointName(joint)));
    }
  }

  out.desc.max_angular_velocity = kMaxAngularVelocity;
  out.desc.parts.reserve(kParts.size());
  for (size_t p = 0; p < kParts.size(); ++p) {
    const auto part = static_cast<RagdollPart>(p);
    physics::RagdollPartDesc desc;
    desc.parent = kParts[p].parent;
    desc.shape = ShapeOf(source, part);
    desc.rest_pose = {.position = source.position(kParts[p].joint)};
    desc.mass = kParts[p].mass;
    desc.joint = JointOf(source, part);
    if (!Finite(desc.shape.center) || !Finite(desc.shape.half_extents) ||
        !std::isfinite(desc.shape.radius) ||
        !std::isfinite(desc.shape.half_height) ||
        !Finite(desc.joint.parent.primary) ||
        !Finite(desc.joint.parent.reference) ||
        !Finite(desc.joint.child.primary) ||
        !Finite(desc.joint.child.reference)) {
      return Result::Error(
          std::format("ragdoll part {} came out non-finite", p));
    }
    out.desc.parts.push_back(desc);
  }
  // The thighs reach into the chest box when crouching.
  out.desc.no_collide = {
      {static_cast<int>(RagdollPart::kChest),
       static_cast<int>(RagdollPart::kLeftThigh)},
      {static_cast<int>(RagdollPart::kChest),
       static_cast<int>(RagdollPart::kRightThigh)},
  };
  return Result::Ok(std::move(out));
}

void PartPosesFromLocals(const RagdollRig& rig, const glm::mat4& root,
                         std::span<const animation::Transform> locals,
                         std::span<physics::Pose> out) {
  const size_t count = NodeCount(rig, locals.size());
  std::array<NodePose, kMaxNodes> world;
  for (size_t i = 0; i < count; ++i) {
    const int parent = rig.node_parent[i];
    world[i] = parent < 0 ? NodePose{.position = locals[i].translation,
                                     .rotation = locals[i].rotation}
                          : Compose(world[static_cast<size_t>(parent)],
                                    locals[i].translation, locals[i].rotation);
  }
  const glm::quat root_rotation =
      glm::normalize(glm::quat_cast(glm::mat3(root)));
  const size_t parts = std::min(out.size(), rig.part_node.size());
  for (size_t p = 0; p < parts; ++p) {
    const auto node = static_cast<size_t>(rig.part_node[p]);
    if (node >= count) {
      out[p] = {};
      continue;
    }
    out[p] = {
        .position = glm::vec3(root * glm::vec4(world[node].position, 1.0f)),
        .rotation = glm::normalize(root_rotation * world[node].rotation)};
  }
}

void LocalsFromPartPoses(const RagdollRig& rig,
                         std::span<const physics::Pose> poses,
                         std::span<animation::Transform> locals) {
  const size_t count = NodeCount(rig, locals.size());
  std::array<NodePose, kMaxNodes> world;
  for (size_t i = 0; i < count; ++i) {
    const int parent = rig.node_parent[i];
    const int part = rig.node_part[i];
    animation::Transform& local = locals[i];
    if (part >= 0 && static_cast<size_t>(part) < poses.size()) {
      const physics::Pose& pose = poses[static_cast<size_t>(part)];
      world[i] = {.position = pose.position,
                  .rotation = glm::normalize(pose.rotation)};
      if (parent < 0) {
        local.translation = world[i].position;
        local.rotation = world[i].rotation;
      } else {
        const NodePose& up = world[static_cast<size_t>(parent)];
        const glm::quat inverse = glm::conjugate(up.rotation);
        local.translation = inverse * (world[i].position - up.position);
        local.rotation = glm::normalize(inverse * world[i].rotation);
      }
      local.scale = glm::vec3(1.0f);
    } else {
      world[i] = parent < 0 ? NodePose{.position = local.translation,
                                       .rotation = local.rotation}
                            : Compose(world[static_cast<size_t>(parent)],
                                      local.translation, local.rotation);
    }
  }
}

}  // namespace blue::game
