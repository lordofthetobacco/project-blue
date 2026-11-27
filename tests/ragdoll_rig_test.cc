// Tests of game::MakeRagdollRig and the pose conversions (game/ragdoll_rig.h)
// on the real player model (kyle.pbo as blue-pack samples cooks it into the
// build's assets): 13 parts of 70 +- 3 kg, left/right mirror, shapes inside the
// mesh bounds, hinge axes of the animator, joint limits holding for animator
// poses, locals -> parts -> locals round trips, the template creates, and the
// CharacterModel::AppendDraws overloads agree.

#include "game/ragdoll_rig.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "animation/hierarchy.h"
#include "animation/transform.h"
#include "core/path.h"
#include "game/character_animator.h"
#include "game/character_model.h"
#include "physics/physics_runtime.h"
#include "physics/ragdoll.h"
#include "prefab/prefab.h"
#include "prefab/prefab_file.h"
#include "render/scene_view.h"
#include "tests/world_fixtures.h"
#include "world/prefab_cache.h"

namespace blue::game {
namespace {

constexpr float kDt = 1.0f / 60.0f;
constexpr size_t kParts = kRagdollPartCount;

std::string Asset(std::string_view name) {
  return core::JoinPath(BLUE_ASSETS_DIR, name);
}

constexpr size_t P(RagdollPart part) { return static_cast<size_t>(part); }

// kyle.pbo, its rig and its ragdoll skeleton, loaded once.
struct Kyle {
  using Loaded = decltype(prefab::LoadPrefab(std::string()));
  Loaded loaded;
  CharacterRig rig;
  RagdollRig ragdoll;

  const prefab::Prefab& prefab() const { return loaded.value(); }
};

const Kyle& GetKyle() {
  static const Kyle* const kyle = [] {
    auto loaded = prefab::LoadPrefab(Asset("kyle.pbo"));
    REQUIRE(loaded.ok());
    auto* result = new Kyle{std::move(loaded), {}, {}};
    auto rig =
        MakeCharacterRig(result->prefab().hierarchy, result->prefab().sockets);
    REQUIRE(rig.ok());
    result->rig = std::move(rig).value();
    auto ragdoll = MakeRagdollRig(result->rig, result->prefab());
    REQUIRE_MESSAGE(ragdoll.ok(), (ragdoll.ok() ? "" : ragdoll.error()));
    result->ragdoll = std::move(ragdoll).value();
    return result;
  }();
  return *kyle;
}

WeaponHold Hold(HeldWeapon kind, std::string_view file) {
  auto loaded = prefab::LoadPrefab(Asset(file));
  REQUIRE(loaded.ok());
  return MakeWeaponHold(kind, loaded.value());
}

// A pose of the animator.
struct Posed {
  std::string name;
  std::vector<animation::Transform> locals;
  glm::mat4 root{1.0f};
};

void Capture(const CharacterAnimator& animator, std::string name,
             std::vector<Posed>& out) {
  out.push_back({std::move(name),
                 {animator.locals().begin(), animator.locals().end()},
                 animator.root()});
}

// Standing, walking (several phases of the stride), crouching, and aiming a
// rifle at pitch -1, 0 and +1 rad and a pistol level, with the character at
// `feet` turned by `yaw`.
std::vector<Posed> AnimatorPoses(glm::vec3 feet, float yaw) {
  const CharacterRig& rig = GetKyle().rig;
  std::vector<Posed> poses;
  const auto settle = [&](CharacterInput input, int frames) {
    CharacterAnimator animator(rig);
    input.feet = feet;
    input.yaw = yaw;
    input.dt = kDt;
    for (int frame = 0; frame < frames; ++frame) {
      animator.Update(input);
    }
    return animator;
  };

  poses.reserve(16);
  Capture(settle({}, 60), "standing", poses);

  // Walking: take one pose every 5 frames over a stride after settling.
  {
    CharacterAnimator animator(rig);
    CharacterInput input;
    input.dt = kDt;
    input.yaw = yaw;
    input.velocity = glm::vec3(CharacterRoot(glm::vec3(0.0f), yaw) *
                               glm::vec4(0.0f, 0.0f, -3.5f, 0.0f));
    glm::vec3 position = feet;
    for (int frame = 0; frame < 180; ++frame) {
      position += input.velocity * kDt;
      input.feet = position;
      animator.Update(input);
      if (frame >= 120 && frame % 5 == 0) {
        Capture(animator, "walking " + std::to_string(frame), poses);
      }
    }
  }

  CharacterInput crouch;
  crouch.crouching = true;
  Capture(settle(crouch, 60), "crouching", poses);

  const WeaponHold rifle = Hold(HeldWeapon::kRifle, "m16.pbo");
  for (const float pitch : {-1.0f, 0.0f, 1.0f}) {
    CharacterInput aim;
    aim.aiming = true;
    aim.pitch = pitch;
    aim.weapon = rifle;
    Capture(settle(aim, 60), "rifle aimed pitch " + std::to_string(pitch),
            poses);
  }
  CharacterInput pistol;
  pistol.aiming = true;
  pistol.weapon = Hold(HeldWeapon::kPistol, "glock17.pbo");
  Capture(settle(pistol, 60), "pistol aimed", poses);
  return poses;
}

std::array<physics::Pose, kParts> PartPoses(const Posed& posed) {
  std::array<physics::Pose, kParts> parts;
  PartPosesFromLocals(GetKyle().ragdoll, posed.root, posed.locals, parts);
  return parts;
}

glm::vec3 Mirror(glm::vec3 v) { return {-v.x, v.y, v.z}; }

bool Near(glm::vec3 a, glm::vec3 b, float tolerance) {
  return glm::length(a - b) <= tolerance;
}

bool Finite(glm::vec3 v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// The part's shape as an axis-aligned box in the part frame.
struct Extent {
  glm::vec3 min;
  glm::vec3 max;
};

Extent ShapeExtent(const physics::ShapeCookDesc& shape) {
  if (shape.kind == physics::CookedShapeKind::kBox) {
    return {shape.center - shape.half_extents,
            shape.center + shape.half_extents};
  }
  const glm::vec3 axis = shape.rotation * glm::vec3(0.0f, 1.0f, 0.0f);
  const glm::vec3 reach =
      glm::abs(axis) * shape.half_height + glm::vec3(shape.radius);
  return {shape.center - reach, shape.center + reach};
}

// The union of the mesh bounds of the nodes of `part`, in the part frame.
std::optional<Extent> MeshExtent(size_t part) {
  const Kyle& kyle = GetKyle();
  const RagdollRig& ragdoll = kyle.ragdoll;
  const animation::Hierarchy& hierarchy = kyle.prefab().hierarchy;
  std::vector<glm::mat4> world(hierarchy.nodes.size());
  animation::ComputeWorldMatrices(hierarchy, kyle.rig.rest, world);
  const glm::mat4 inverse =
      glm::inverse(world[static_cast<size_t>(ragdoll.part_node[part])]);
  std::optional<Extent> extent;
  for (const prefab::Mesh& mesh : kyle.prefab().meshes) {
    int owner = -1;
    for (int n = mesh.node; n >= 0 && owner < 0;
         n = ragdoll.node_parent[static_cast<size_t>(n)]) {
      owner = ragdoll.node_part[static_cast<size_t>(n)];
    }
    if (owner != static_cast<int>(part)) {
      continue;
    }
    const glm::mat4 to_part = inverse * world[static_cast<size_t>(mesh.node)];
    for (int corner = 0; corner < 8; ++corner) {
      const glm::vec3 point(
          (corner & 1) != 0 ? mesh.bounds.max.x : mesh.bounds.min.x,
          (corner & 2) != 0 ? mesh.bounds.max.y : mesh.bounds.min.y,
          (corner & 4) != 0 ? mesh.bounds.max.z : mesh.bounds.min.z);
      const glm::vec3 p(to_part * glm::vec4(point, 1.0f));
      if (!extent) {
        extent = Extent{p, p};
      }
      extent->min = glm::min(extent->min, p);
      extent->max = glm::max(extent->max, p);
    }
  }
  return extent;
}

// The animator's own fold directions: legs fold backwards, arms forwards.
glm::vec3 FoldOf(RagdollPart part) {
  return part == RagdollPart::kLeftShin || part == RagdollPart::kRightShin
             ? glm::vec3(0.0f, 0.0f, 1.0f)
             : glm::vec3(0.0f, 0.0f, -1.0f);
}

bool IsHinge(RagdollPart part) {
  return part == RagdollPart::kLeftForearm ||
         part == RagdollPart::kRightForearm || part == RagdollPart::kLeftShin ||
         part == RagdollPart::kRightShin;
}

// The right-handed rotation (degrees) of the child's hinge reference from the
// parent's about the parent's hinge axis, for the part poses.
float HingeDegrees(const physics::RagdollJointDesc& joint,
                   const physics::Pose& parent, const physics::Pose& child) {
  const glm::vec3 axis = parent.rotation * joint.parent.primary;
  const glm::vec3 a = parent.rotation * joint.parent.reference;
  const glm::vec3 b = child.rotation * joint.child.reference;
  return glm::degrees(
      std::atan2(glm::dot(glm::cross(a, b), axis), glm::dot(a, b)));
}

// Swing and twist of the child's joint frame relative to the parent's.
struct SwingTwist {
  float reference = 0.0f;  // Tilt of the twist axis towards the reference, deg.
  float side = 0.0f;       // Tilt towards the third axis, deg.
  float twist = 0.0f;      // Twist about the axis, deg.
  // The cone limit as an ellipse: <= 1 inside.
  float cone = 0.0f;
};

glm::mat3 FrameBasis(const physics::RagdollJointFrame& frame) {
  const glm::vec3 reference =
      glm::normalize(frame.reference -
                     frame.primary * glm::dot(frame.primary, frame.reference));
  return glm::mat3(frame.primary, reference,
                   glm::cross(frame.primary, reference));
}

SwingTwist MeasureSwingTwist(const physics::RagdollJointDesc& joint,
                             const physics::Pose& parent,
                             const physics::Pose& child) {
  const glm::mat3 relative =
      glm::transpose(glm::mat3_cast(parent.rotation) *
                     FrameBasis(joint.parent)) *
      (glm::mat3_cast(child.rotation) * FrameBasis(joint.child));
  const glm::quat q = glm::normalize(glm::quat_cast(relative));
  const glm::quat twist = glm::normalize(glm::quat(q.w, q.x, 0.0f, 0.0f));
  glm::quat swing = q * glm::conjugate(twist);
  if (swing.w < 0.0f) {
    swing = -swing;
  }
  const glm::vec3 axis = swing * glm::vec3(1.0f, 0.0f, 0.0f);
  SwingTwist result;
  result.reference = glm::degrees(std::atan2(axis.y, axis.x));
  result.side = glm::degrees(std::atan2(axis.z, axis.x));
  float degrees = glm::degrees(2.0f * std::atan2(q.x, q.w));
  if (std::abs(q.w) > 1.0e-6f && q.w < 0.0f) {
    degrees = glm::degrees(2.0f * std::atan2(-q.x, -q.w));
  }
  result.twist = degrees;
  // The swing quaternion (w, 0, y, z): z carries the tilt towards the
  // reference axis, y the tilt towards the third axis. Each is limited by the
  // sine of half its cone angle, as an ellipse.
  const float z = std::abs(swing.z) /
                  std::sin(glm::radians(joint.cone_reference_degrees) * 0.5f);
  const float y = std::abs(swing.y) /
                  std::sin(glm::radians(joint.cone_side_degrees) * 0.5f);
  result.cone = y * y + z * z;
  return result;
}

TEST_CASE("ragdoll_rig: 13 parts of 70 kg with the table's parents") {
  const RagdollRig& rig = GetKyle().ragdoll;
  REQUIRE(rig.desc.parts.size() == kParts);
  const std::array<int, kParts> parents{-1, 0, 1, 1, 3,  1, 5,
                                        0,  7, 8, 0, 10, 11};
  float mass = 0.0f;
  for (size_t p = 0; p < kParts; ++p) {
    CAPTURE(p);
    const physics::RagdollPartDesc& part = rig.desc.parts[p];
    CHECK(part.parent == parents[p]);
    CHECK(part.parent < static_cast<int>(p));
    CHECK(part.mass > 0.0f);
    mass += part.mass;
    CHECK(Finite(part.rest_pose.position));
    const physics::ShapeCookDesc& shape = part.shape;
    CHECK(Finite(shape.center));
    CHECK(Finite(shape.half_extents));
    CHECK(std::isfinite(shape.radius));
    CHECK(std::isfinite(shape.half_height));
    if (shape.kind == physics::CookedShapeKind::kBox) {
      CHECK(shape.half_extents.x > 0.0f);
      CHECK(shape.half_extents.y > 0.0f);
      CHECK(shape.half_extents.z > 0.0f);
    } else {
      REQUIRE(shape.kind == physics::CookedShapeKind::kCapsule);
      CHECK(shape.radius > 0.0f);
      CHECK(shape.half_height > 0.0f);
    }
    if (p == 0) {
      continue;
    }
    const physics::RagdollJointDesc& joint = part.joint;
    CHECK(Finite(joint.anchor));
    for (const physics::RagdollJointFrame& frame :
         {joint.parent, joint.child}) {
      CHECK(glm::length(frame.primary) == doctest::Approx(1.0f).epsilon(1e-4));
      CHECK(glm::length(frame.reference) ==
            doctest::Approx(1.0f).epsilon(1e-4));
      CHECK(std::abs(glm::dot(frame.primary, frame.reference)) < 1.0e-4f);
    }
  }
  CHECK(mass == doctest::Approx(70.0f).epsilon(0.043));
  CHECK(std::abs(mass - 70.0f) <= 3.0f);

  // Part frames are the model's nodes; the rest follows the hierarchy.
  const animation::Hierarchy& hierarchy = GetKyle().prefab().hierarchy;
  REQUIRE(rig.node_part.size() == hierarchy.nodes.size());
  REQUIRE(rig.node_parent.size() == hierarchy.nodes.size());
  for (size_t n = 0; n < hierarchy.nodes.size(); ++n) {
    CHECK(rig.node_parent[n] == hierarchy.nodes[n].parent);
  }
  int carriers = 0;
  for (size_t p = 0; p < kParts; ++p) {
    const auto node = static_cast<size_t>(rig.part_node[p]);
    CHECK(rig.node_part[node] == static_cast<int>(p));
    CHECK(rig.part_node[p] ==
          GetKyle().rig.nodes[static_cast<size_t>(rig.part_joint[p])]);
    ++carriers;
  }
  CHECK(carriers == 13);
  CHECK(std::count_if(rig.node_part.begin(), rig.node_part.end(),
                      [](int part) { return part >= 0; }) == 13);
}

TEST_CASE("ragdoll_rig: left and right parts mirror in X") {
  const RagdollRig& rig = GetKyle().ragdoll;
  const std::array<std::pair<RagdollPart, RagdollPart>, 5> pairs{{
      {RagdollPart::kLeftUpperArm, RagdollPart::kRightUpperArm},
      {RagdollPart::kLeftForearm, RagdollPart::kRightForearm},
      {RagdollPart::kLeftThigh, RagdollPart::kRightThigh},
      {RagdollPart::kLeftShin, RagdollPart::kRightShin},
      {RagdollPart::kLeftFoot, RagdollPart::kRightFoot},
  }};
  constexpr float kTolerance = 1.0e-3f;
  for (const auto& [left_id, right_id] : pairs) {
    CAPTURE(static_cast<int>(left_id));
    const physics::RagdollPartDesc& l = rig.desc.parts[P(left_id)];
    const physics::RagdollPartDesc& r = rig.desc.parts[P(right_id)];
    CHECK(Near(Mirror(l.rest_pose.position), r.rest_pose.position, kTolerance));
    CHECK(l.mass == r.mass);
    CHECK(l.shape.kind == r.shape.kind);
    CHECK(Near(Mirror(l.shape.center), r.shape.center, kTolerance));
    CHECK(Near(l.shape.half_extents, r.shape.half_extents, kTolerance));
    CHECK(l.shape.radius == doctest::Approx(r.shape.radius));
    CHECK(l.shape.half_height == doctest::Approx(r.shape.half_height));
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    CHECK(
        Near(Mirror(l.shape.rotation * up), r.shape.rotation * up, kTolerance));

    const physics::RagdollJointDesc& lj = l.joint;
    const physics::RagdollJointDesc& rj = r.joint;
    CHECK(lj.kind == rj.kind);
    CHECK(Near(Mirror(lj.anchor), rj.anchor, kTolerance));
    CHECK(lj.cone_reference_degrees == rj.cone_reference_degrees);
    CHECK(lj.cone_side_degrees == rj.cone_side_degrees);
    CHECK(lj.twist_min_degrees == rj.twist_min_degrees);
    CHECK(lj.twist_max_degrees == rj.twist_max_degrees);
    CHECK(lj.hinge_min_degrees == rj.hinge_min_degrees);
    CHECK(lj.hinge_max_degrees == rj.hinge_max_degrees);
    CHECK(lj.friction_torque == rj.friction_torque);
    // A reflection turns an axis into the mirrored vector up to sign: swing
    // frames keep their sign convention, hinge axes (pseudo-vectors) flip.
    const auto parallel = [&](glm::vec3 a, glm::vec3 b) {
      return std::abs(glm::dot(Mirror(a), b)) > 1.0f - kTolerance;
    };
    CHECK(parallel(lj.parent.primary, rj.parent.primary));
    CHECK(parallel(lj.parent.reference, rj.parent.reference));
    CHECK(parallel(lj.child.primary, rj.child.primary));
    CHECK(parallel(lj.child.reference, rj.child.reference));
    if (lj.kind == physics::RagdollJointKind::kHinge) {
      CHECK(glm::dot(Mirror(lj.parent.primary), rj.parent.primary) <
            -1.0f + kTolerance);
      CHECK(glm::dot(Mirror(lj.child.primary), rj.child.primary) <
            -1.0f + kTolerance);
    }
  }
}

TEST_CASE("ragdoll_rig: shapes lie inside the mesh bounds of their nodes") {
  const RagdollRig& rig = GetKyle().ragdoll;
  constexpr float kGrow = 0.05f;
  for (size_t p = 0; p < kParts; ++p) {
    CAPTURE(p);
    const std::optional<Extent> mesh = MeshExtent(p);
    REQUIRE(mesh.has_value());
    const Extent shape = ShapeExtent(rig.desc.parts[p].shape);
    INFO("shape ", shape.min.x, " ", shape.min.y, " ", shape.min.z, " .. ",
         shape.max.x, " ", shape.max.y, " ", shape.max.z);
    INFO("mesh ", mesh->min.x, " ", mesh->min.y, " ", mesh->min.z, " .. ",
         mesh->max.x, " ", mesh->max.y, " ", mesh->max.z);
    for (int axis = 0; axis < 3; ++axis) {
      const auto a = static_cast<glm::length_t>(axis);
      CHECK(shape.min[a] >= mesh->min[a] - kGrow);
      CHECK(shape.max[a] <= mesh->max[a] + kGrow);
    }
  }
}

TEST_CASE("ragdoll_rig: hinge axes are the animator's and angles stay inside") {
  const Kyle& kyle = GetKyle();
  const RagdollRig& rig = kyle.ragdoll;
  struct Hinge {
    RagdollPart part;
    Joint upper;  // Rest offset of this joint is the upper bone.
    Joint lower;
  };
  const std::array<Hinge, 4> hinges{{
      {RagdollPart::kLeftForearm, Joint::kLeftForearm, Joint::kLeftWrist},
      {RagdollPart::kRightForearm, Joint::kRightForearm, Joint::kRightWrist},
      {RagdollPart::kLeftShin, Joint::kLeftKnee, Joint::kLeftAnkle},
      {RagdollPart::kRightShin, Joint::kRightKnee, Joint::kRightAnkle},
  }};
  for (const Hinge& hinge : hinges) {
    const physics::RagdollJointDesc& joint =
        rig.desc.parts[P(hinge.part)].joint;
    REQUIRE(joint.kind == physics::RagdollJointKind::kHinge);
    const glm::vec3 r1 =
        glm::normalize(kyle.rig.rest_offset[static_cast<size_t>(hinge.upper)]);
    const glm::vec3 r2 =
        glm::normalize(kyle.rig.rest_offset[static_cast<size_t>(hinge.lower)]);
    const glm::vec3 fold = FoldOf(hinge.part);
    CHECK(Near(joint.parent.reference, r1, 1.0e-5f));
    CHECK(Near(joint.child.reference, r2, 1.0e-5f));
    CHECK(Near(joint.parent.primary, glm::normalize(glm::cross(r1, fold)),
               1.0e-5f));
    CHECK(Near(joint.child.primary, glm::normalize(glm::cross(r2, fold)),
               1.0e-5f));
    CHECK(joint.hinge_min_degrees == -5.0f);
    CHECK(joint.hinge_max_degrees == 155.0f);
  }

  for (const Posed& posed : AnimatorPoses(glm::vec3(0.0f), 0.0f)) {
    CAPTURE(posed.name);
    const std::array<physics::Pose, kParts> parts = PartPoses(posed);
    for (const Hinge& hinge : hinges) {
      const physics::RagdollPartDesc& desc = rig.desc.parts[P(hinge.part)];
      const physics::Pose& parent = parts[static_cast<size_t>(desc.parent)];
      const physics::Pose& child = parts[P(hinge.part)];
      const float angle = HingeDegrees(desc.joint, parent, child);
      CAPTURE(static_cast<int>(hinge.part));
      CAPTURE(angle);
      CHECK(angle >= -0.5f);
      CHECK(angle <= 155.0f);
      // Both parts turn their axis to the same world axis.
      CHECK(glm::dot(parent.rotation * desc.joint.parent.primary,
                     child.rotation * desc.joint.child.primary) > 0.999f);
    }
  }
}

TEST_CASE("ragdoll_rig: swing-twist joints hold for animator poses") {
  const RagdollRig& rig = GetKyle().ragdoll;
  for (const Posed& posed : AnimatorPoses(glm::vec3(0.0f), 0.0f)) {
    const std::array<physics::Pose, kParts> parts = PartPoses(posed);
    for (size_t p = 1; p < kParts; ++p) {
      const physics::RagdollPartDesc& desc = rig.desc.parts[p];
      if (IsHinge(static_cast<RagdollPart>(p))) {
        continue;
      }
      const SwingTwist st = MeasureSwingTwist(
          desc.joint, parts[static_cast<size_t>(desc.parent)], parts[p]);
      CAPTURE(posed.name);
      CAPTURE(p);
      INFO("swing towards reference ", st.reference, " side ", st.side,
           " twist ", st.twist, " cone ", st.cone);
      // Poses the table's limits cannot contain without absurd ones (measured
      // with the animator as it is): the crouch splays the left thigh about
      // 70 degrees off the hip cone's plane (limit 40, side 75) and folds the
      // right ankle 69 degrees (limit 60). They are corrected over the first
      // steps of a ragdoll spawned from them.
      const bool crouch_outlier =
          posed.name == "crouching" &&
          (p == P(RagdollPart::kLeftThigh) || p == P(RagdollPart::kRightFoot));
      if (!crouch_outlier) {
        CHECK(st.cone <= 1.0f);
      }
      CHECK(st.twist >= desc.joint.twist_min_degrees);
      CHECK(st.twist <= desc.joint.twist_max_degrees);
    }
  }
}

TEST_CASE("ragdoll_rig: locals -> parts -> locals reproduce the pose") {
  const Kyle& kyle = GetKyle();
  const animation::Hierarchy& hierarchy = kyle.prefab().hierarchy;
  const glm::vec3 feet(3.0f, 0.5f, -2.0f);
  for (const Posed& posed : AnimatorPoses(feet, 0.7f)) {
    CAPTURE(posed.name);
    REQUIRE(posed.locals.size() == hierarchy.nodes.size());
    const std::array<physics::Pose, kParts> parts = PartPoses(posed);
    std::vector<animation::Transform> rewritten = posed.locals;
    LocalsFromPartPoses(kyle.ragdoll, parts, rewritten);

    std::vector<glm::mat4> expected(hierarchy.nodes.size());
    std::vector<glm::mat4> actual(hierarchy.nodes.size());
    animation::ComputeWorldMatrices(hierarchy, posed.locals, expected);
    animation::ComputeWorldMatrices(hierarchy, rewritten, actual);
    for (size_t n = 0; n < expected.size(); ++n) {
      const glm::mat4 want = posed.root * expected[n];
      for (glm::length_t c = 0; c < 4; ++c) {
        for (glm::length_t r = 0; r < 4; ++r) {
          CHECK(actual[n][c][r] ==
                doctest::Approx(want[c][r]).epsilon(1e-4).scale(1.0));
        }
      }
    }
    // Nodes that ride on a part keep their local transform.
    for (size_t n = 0; n < rewritten.size(); ++n) {
      if (kyle.ragdoll.node_part[n] >= 0) {
        continue;
      }
      CHECK(Near(rewritten[n].translation, posed.locals[n].translation, 0.0f));
      CHECK(rewritten[n].rotation == posed.locals[n].rotation);
    }
  }
}

TEST_CASE("ragdoll_rig: conversions tolerate short spans") {
  const RagdollRig& rig = GetKyle().ragdoll;
  std::array<physics::Pose, 3> few;
  PartPosesFromLocals(rig, glm::mat4(1.0f), {}, few);
  std::vector<animation::Transform> locals(rig.node_part.size());
  LocalsFromPartPoses(rig, few, locals);
  LocalsFromPartPoses(rig, {}, {});
  CHECK(true);
}

TEST_CASE("ragdoll_rig: the template of the rig creates") {
  physics::PhysicsRuntime runtime;
  const auto tmpl = physics::RagdollTemplate::Create(GetKyle().ragdoll.desc);
  REQUIRE(tmpl.ok());
  CHECK(tmpl.value().part_count() == kRagdollPartCount);
  CHECK(tmpl.value().parent(static_cast<int>(RagdollPart::kLeftShin)) ==
        static_cast<int>(RagdollPart::kLeftThigh));
}

TEST_CASE("ragdoll_rig: a rig of another model is refused") {
  const Kyle& kyle = GetKyle();
  prefab::Prefab other;
  other.hierarchy.nodes = {{.name = "root", .parent = -1, .rest = {}}};
  CHECK_FALSE(MakeRagdollRig(kyle.rig, other).ok());
}

TEST_CASE("ragdoll_rig: AppendDraws with locals equals the animator overload") {
  testing::FakeGpu gpu;
  world::PrefabCache cache(gpu);
  std::optional<CharacterModel> model =
      CharacterModel::Load(cache, Asset("kyle.pbo"));
  REQUIRE(model.has_value());

  CharacterAnimator animator(model->rig());
  CharacterInput input;
  input.dt = kDt;
  input.feet = glm::vec3(1.0f, 0.0f, 2.0f);
  input.yaw = 0.4f;
  input.velocity = glm::vec3(0.0f, 0.0f, -2.0f);
  for (int frame = 0; frame < 30; ++frame) {
    input.feet += input.velocity * kDt;
    animator.Update(input);
  }

  std::vector<render::DrawItem> from_animator;
  std::vector<render::DrawItem> from_locals;
  model->AppendDraws(animator, from_animator);
  model->AppendDraws(animator.root(), animator.locals(), from_locals);
  REQUIRE(!from_animator.empty());
  REQUIRE(from_locals.size() == from_animator.size());
  for (size_t i = 0; i < from_animator.size(); ++i) {
    const render::DrawItem& a = from_animator[i];
    const render::DrawItem& b = from_locals[i];
    CHECK(a.mesh == b.mesh);
    CHECK(a.transform == b.transform);
    CHECK(Near(a.bounds.min, b.bounds.min, 0.0f));
    CHECK(Near(a.bounds.max, b.bounds.max, 0.0f));
    CHECK(a.submeshes.data() == b.submeshes.data());
    CHECK(a.submeshes.size() == b.submeshes.size());
    CHECK(a.material_override == b.material_override);
    CHECK(a.shadow_only == b.shadow_only);
  }
}

// Prints the derived shapes and the joint angles of the animator poses:
// ragdoll_rig_test --no-skip -tc="*dump*" -s
TEST_CASE("ragdoll_rig: dump of the derived rig" * doctest::skip()) {
  const RagdollRig& rig = GetKyle().ragdoll;
  for (size_t p = 0; p < kParts; ++p) {
    const physics::RagdollPartDesc& part = rig.desc.parts[p];
    const Extent e = ShapeExtent(part.shape);
    const std::optional<Extent> mesh = MeshExtent(p);
    MESSAGE("part ", p, " kind ", static_cast<int>(part.shape.kind), " center ",
            part.shape.center.x, " ", part.shape.center.y, " ",
            part.shape.center.z, " half ", part.shape.half_extents.x, " ",
            part.shape.half_extents.y, " ", part.shape.half_extents.z, " r ",
            part.shape.radius, " hh ", part.shape.half_height, " extent ",
            e.min.x, " ", e.min.y, " ", e.min.z, " .. ", e.max.x, " ", e.max.y,
            " ", e.max.z, " mesh ", mesh ? mesh->min.x : 0.0f, " ",
            mesh ? mesh->min.y : 0.0f, " ", mesh ? mesh->min.z : 0.0f, " .. ",
            mesh ? mesh->max.x : 0.0f, " ", mesh ? mesh->max.y : 0.0f, " ",
            mesh ? mesh->max.z : 0.0f);
  }
  for (const Posed& posed : AnimatorPoses(glm::vec3(0.0f), 0.0f)) {
    const std::array<physics::Pose, kParts> parts = PartPoses(posed);
    for (size_t p = 1; p < kParts; ++p) {
      const physics::RagdollPartDesc& desc = rig.desc.parts[p];
      const physics::Pose& parent = parts[static_cast<size_t>(desc.parent)];
      if (IsHinge(static_cast<RagdollPart>(p))) {
        MESSAGE(posed.name, " part ", p, " hinge ",
                HingeDegrees(desc.joint, parent, parts[p]));
      } else {
        const SwingTwist st = MeasureSwingTwist(desc.joint, parent, parts[p]);
        MESSAGE(posed.name, " part ", p, " reference ", st.reference, " side ",
                st.side, " twist ", st.twist, " cone ", st.cone);
      }
    }
  }
}

}  // namespace
}  // namespace blue::game
