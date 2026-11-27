// Tests of physics::Ragdoll (src/physics/ragdoll.h): a chain falls and rests
// with finite poses, hinge and cone limits hold, a spawn outside the limits
// recovers without a kick, default ray casts and characters ignore the parts
// while RayFilter::kEverything hits them, RemoveAllBodies with a live ragdoll
// is safe, parts interpolate. All rigs are built by hand; no assets.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>

#include "asset/mesh_data.h"
#include "core/result.h"
#include "physics/character.h"
#include "physics/physics_runtime.h"
#include "physics/physics_world.h"
#include "physics/ragdoll.h"
#include "physics/shape_cooking.h"
#include "scene/scene.h"
#include "weapon/projectile_system.h"

namespace blue::physics {
namespace {

constexpr float kGravity = -9.81f;
const glm::vec3 kX(1.0f, 0.0f, 0.0f);
const glm::vec3 kY(0.0f, 1.0f, 0.0f);
const glm::vec3 kZ(0.0f, 0.0f, 1.0f);

// A world on the calling thread (one PhysicsRuntime at a time).
struct Fixture {
  PhysicsRuntime runtime;
  PhysicsWorld world{runtime, PhysicsWorldOptions{.worker_threads = 0}};

  explicit Fixture(bool gravity = true, bool ground = true) {
    world.SetGravity(glm::vec3(0.0f, gravity ? kGravity : 0.0f, 0.0f));
    if (ground) {
      AddStaticBox(asset::Bounds{glm::vec3(-50.0f, 0.0f, -50.0f),
                                 glm::vec3(50.0f, 0.0f, 50.0f)},
                   glm::vec3(0.0f));
    }
  }

  BodyId AddStaticBox(asset::Bounds bounds, glm::vec3 position) {
    BodyDesc desc;
    desc.body.shape = scene::ShapeType::kBox;
    desc.body.motion = scene::MotionType::kStatic;
    desc.pose.position = position;
    desc.bounds = bounds;
    const core::Result<BodyId, PhysicsError> body = world.CreateBody(desc);
    REQUIRE(body.ok());
    return body.value();
  }

  void Step(int count = 1) {
    for (int i = 0; i < count; ++i) {
      world.StepOnce();
    }
  }
};

ShapeCookDesc Capsule(float radius, float half_height,
                      glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f)) {
  ShapeCookDesc desc;
  desc.kind = CookedShapeKind::kCapsule;
  desc.radius = radius;
  desc.half_height = half_height;
  desc.rotation = rotation;
  return desc;
}

ShapeCookDesc Sphere(float radius) {
  ShapeCookDesc desc;
  desc.kind = CookedShapeKind::kSphere;
  desc.radius = radius;
  return desc;
}

ShapeCookDesc Box(glm::vec3 half_extents) {
  ShapeCookDesc desc;
  desc.kind = CookedShapeKind::kBox;
  desc.half_extents = half_extents;
  return desc;
}

// Both frames: primary `axis`, reference `reference`.
RagdollJointDesc SwingTwistJoint(glm::vec3 anchor, glm::vec3 axis,
                                 glm::vec3 reference, float cone_reference,
                                 float cone_side, float twist_min,
                                 float twist_max, float friction = 0.0f) {
  RagdollJointDesc joint;
  joint.kind = RagdollJointKind::kSwingTwist;
  joint.anchor = anchor;
  joint.parent = {axis, reference};
  joint.child = {axis, reference};
  joint.cone_reference_degrees = cone_reference;
  joint.cone_side_degrees = cone_side;
  joint.twist_min_degrees = twist_min;
  joint.twist_max_degrees = twist_max;
  joint.friction_torque = friction;
  return joint;
}

RagdollPartDesc Part(int parent, ShapeCookDesc shape, glm::vec3 position,
                     float mass, RagdollJointDesc joint = {}) {
  RagdollPartDesc part;
  part.parent = parent;
  part.shape = shape;
  part.rest_pose.position = position;
  part.mass = mass;
  part.joint = joint;
  return part;
}

// `count` capsules (0.4 m long, 5 cm radius) stacked along +Y from the
// origin, joined by swing-twist joints (60 degree cones, +-45 degrees twist).
RagdollDesc ChainDesc(int count) {
  RagdollDesc desc;
  for (int i = 0; i < count; ++i) {
    const glm::vec3 center(0.0f, 0.4f * static_cast<float>(i) + 0.2f, 0.0f);
    desc.parts.push_back(Part(
        i == 0 ? -1 : i - 1, Capsule(0.05f, 0.15f), center, 2.0f,
        SwingTwistJoint(glm::vec3(0.0f, 0.4f * static_cast<float>(i), 0.0f), kY,
                        kX, 60.0f, 60.0f, -45.0f, 45.0f, 0.2f)));
  }
  return desc;
}

// A parent capsule on the -X side and a child on the +X side of the anchor at
// the origin, both along X; one hinge about +Z.
RagdollDesc HingeDesc(float min_degrees, float max_degrees) {
  const glm::quat along_x = glm::angleAxis(glm::radians(-90.0f), kZ);
  RagdollDesc desc;
  desc.stabilize = false;
  desc.parts.push_back(Part(-1, Capsule(0.05f, 0.15f, along_x),
                            glm::vec3(-0.2f, 0.0f, 0.0f), 100.0f));
  RagdollJointDesc joint;
  joint.kind = RagdollJointKind::kHinge;
  joint.parent = {kZ, kX};
  joint.child = {kZ, kX};
  joint.hinge_min_degrees = min_degrees;
  joint.hinge_max_degrees = max_degrees;
  desc.parts.push_back(Part(0, Capsule(0.05f, 0.15f, along_x),
                            glm::vec3(0.2f, 0.0f, 0.0f), 1.0f, joint));
  return desc;
}

// A parent along +Y below the anchor and a child above it, one swing-twist
// joint with the given limits.
RagdollDesc ConeDesc(float cone_reference, float cone_side, float twist_min,
                     float twist_max) {
  RagdollDesc desc;
  desc.stabilize = false;
  desc.parts.push_back(
      Part(-1, Capsule(0.05f, 0.15f), glm::vec3(0.0f, -0.2f, 0.0f), 100.0f));
  desc.parts.push_back(
      Part(0, Capsule(0.05f, 0.15f), glm::vec3(0.0f, 0.2f, 0.0f), 1.0f,
           SwingTwistJoint(glm::vec3(0.0f), kY, kX, cone_reference, cone_side,
                           twist_min, twist_max)));
  return desc;
}

// One sphere, no joint.
RagdollDesc SphereDesc(float radius, float mass) {
  RagdollDesc desc;
  desc.parts.push_back(Part(-1, Sphere(radius), glm::vec3(0.0f), mass));
  return desc;
}

RagdollTemplate MakeTemplate(const RagdollDesc& desc) {
  core::Result<RagdollTemplate, PhysicsError> tmpl =
      RagdollTemplate::Create(desc);
  REQUIRE(tmpl.ok());
  return tmpl.value();
}

// The rest poses of `tmpl` moved by `offset`.
std::vector<Pose> RestPoses(const RagdollTemplate& tmpl,
                            glm::vec3 offset = glm::vec3(0.0f)) {
  std::vector<Pose> poses;
  for (int i = 0; i < tmpl.part_count(); ++i) {
    Pose pose = tmpl.rest_pose(i);
    pose.position += offset;
    poses.push_back(pose);
  }
  return poses;
}

Ragdoll Spawn(PhysicsWorld& world, const RagdollTemplate& tmpl,
              const std::vector<Pose>& poses,
              glm::vec3 velocity = glm::vec3(0.0f)) {
  core::Result<Ragdoll, PhysicsError> created = Ragdoll::Create(
      world, tmpl,
      RagdollSpawnDesc{.poses = poses, .linear_velocity = velocity});
  REQUIRE(created.ok());
  return std::move(created.value());
}

BodyState State(const PhysicsWorld& world, const Ragdoll& ragdoll, int part) {
  return world.GetBodyState(ragdoll.bodies()[static_cast<size_t>(part)]);
}

bool IsFinite(glm::vec3 v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool AllFinite(const PhysicsWorld& world, const Ragdoll& ragdoll) {
  for (const BodyId body : ragdoll.bodies()) {
    const BodyState state = world.GetBodyState(body);
    if (!IsFinite(state.pose.position) || !IsFinite(state.linear_velocity) ||
        !IsFinite(state.angular_velocity) ||
        !std::isfinite(state.pose.rotation.w) ||
        !std::isfinite(state.pose.rotation.x) ||
        !std::isfinite(state.pose.rotation.y) ||
        !std::isfinite(state.pose.rotation.z)) {
      return false;
    }
  }
  return true;
}

float MaxSpeed(const PhysicsWorld& world, const Ragdoll& ragdoll) {
  float peak = 0.0f;
  for (const BodyId body : ragdoll.bodies()) {
    peak = std::max(peak, glm::length(world.LinearVelocity(body)));
  }
  return peak;
}

// Rotation of `child` relative to `parent`, w >= 0.
glm::quat Relative(const BodyState& parent, const BodyState& child) {
  glm::quat rel = glm::inverse(parent.pose.rotation) * child.pose.rotation;
  if (rel.w < 0.0f) {
    rel = -rel;
  }
  return rel;
}

// Signed rotation about +Z / +Y of a (nearly) pure rotation about that axis.
float AngleAboutZ(const glm::quat& q) {
  return glm::degrees(2.0f * std::atan2(q.z, q.w));
}
float AngleAboutY(const glm::quat& q) {
  return glm::degrees(2.0f * std::atan2(q.y, q.w));
}

// Angle in degrees between the child's +Y axis and the parent's, split into
// the tilt towards +X and towards +Z (valid for tilts in one of those planes).
struct Tilt {
  float towards_x;
  float towards_z;
};
Tilt TiltOf(const glm::quat& rel) {
  const glm::vec3 axis = rel * kY;
  return Tilt{glm::degrees(std::atan2(axis.x, axis.y)),
              glm::degrees(std::atan2(axis.z, axis.y))};
}

TEST_CASE("RagdollTemplate::Create validates the description") {
  Fixture f(false, false);
  CHECK_FALSE(RagdollTemplate::Create(RagdollDesc{}).ok());

  RagdollDesc too_many;
  for (int i = 0; i < 65; ++i) {
    too_many.parts.push_back(
        Part(i == 0 ? -1 : 0, Sphere(0.1f), glm::vec3(0.0f), 1.0f,
             SwingTwistJoint(glm::vec3(0.0f), kY, kX, 10, 10, -10, 10)));
  }
  CHECK_FALSE(RagdollTemplate::Create(too_many).ok());

  RagdollDesc desc = ChainDesc(3);
  CHECK(RagdollTemplate::Create(desc).ok());

  RagdollDesc bad_parent = desc;
  bad_parent.parts[1].parent = 1;  // Itself, not an earlier part.
  CHECK_FALSE(RagdollTemplate::Create(bad_parent).ok());
  bad_parent = desc;
  bad_parent.parts[0].parent = 0;
  CHECK_FALSE(RagdollTemplate::Create(bad_parent).ok());

  RagdollDesc bad_mass = desc;
  bad_mass.parts[2].mass = 0.0f;
  CHECK_FALSE(RagdollTemplate::Create(bad_mass).ok());

  RagdollDesc bad_limit = desc;
  bad_limit.parts[1].joint.twist_min_degrees = 10.0f;  // Must be <= 0.
  CHECK_FALSE(RagdollTemplate::Create(bad_limit).ok());
  bad_limit = desc;
  bad_limit.parts[1].joint.cone_side_degrees = 200.0f;
  CHECK_FALSE(RagdollTemplate::Create(bad_limit).ok());

  RagdollDesc bad_axis = desc;
  bad_axis.parts[1].joint.parent.primary = glm::vec3(0.0f);
  CHECK_FALSE(RagdollTemplate::Create(bad_axis).ok());
  bad_axis = desc;
  bad_axis.parts[1].joint.child.reference = kY;  // Parallel to primary.
  CHECK_FALSE(RagdollTemplate::Create(bad_axis).ok());

  RagdollDesc not_finite = desc;
  not_finite.parts[0].rest_pose.position.x =
      std::numeric_limits<float>::quiet_NaN();
  CHECK_FALSE(RagdollTemplate::Create(not_finite).ok());

  RagdollDesc bad_pair = desc;
  bad_pair.no_collide.emplace_back(0, 7);
  CHECK_FALSE(RagdollTemplate::Create(bad_pair).ok());
}

TEST_CASE("a template reports its parts") {
  Fixture f(false, false);
  const RagdollTemplate tmpl = MakeTemplate(ChainDesc(3));
  REQUIRE(tmpl.part_count() == 3);
  CHECK(tmpl.parent(0) == -1);
  CHECK(tmpl.parent(2) == 1);
  CHECK(tmpl.parent(3) == -1);
  CHECK(tmpl.rest_pose(2).position.y == doctest::Approx(1.0f));
  const asset::Bounds bounds = tmpl.shape(1).bounds();
  CHECK(bounds.max.y - bounds.min.y == doctest::Approx(0.4f).epsilon(1.0e-3));
}

TEST_CASE("Create rejects a wrong pose count and non-finite input") {
  Fixture f;
  const RagdollTemplate tmpl = MakeTemplate(ChainDesc(3));
  std::vector<Pose> poses = RestPoses(tmpl);
  poses.pop_back();
  CHECK_FALSE(Ragdoll::Create(f.world, tmpl, {.poses = poses}).ok());

  poses = RestPoses(tmpl);
  poses[1].position.y = std::numeric_limits<float>::infinity();
  CHECK_FALSE(Ragdoll::Create(f.world, tmpl, {.poses = poses}).ok());

  poses = RestPoses(tmpl);
  CHECK_FALSE(Ragdoll::Create(f.world, tmpl,
                              {.poses = poses,
                               .linear_velocity = glm::vec3(
                                   std::numeric_limits<float>::quiet_NaN())})
                  .ok());
  CHECK(f.world.Stats().bodies == 1);  // Only the ground; nothing leaked.
}

TEST_CASE("a chain dropped from 1 m falls, stays finite and comes to rest") {
  Fixture f;
  const RagdollTemplate tmpl = MakeTemplate(ChainDesc(3));
  // Leaning by 20 degrees (a vertical stack would balance on its end).
  const glm::quat lean = glm::angleAxis(glm::radians(20.0f), kZ);
  std::vector<Pose> poses = RestPoses(tmpl);
  for (Pose& pose : poses) {
    pose.position = lean * pose.position + glm::vec3(0.0f, 1.0f, 0.0f);
    pose.rotation = lean * pose.rotation;
  }
  Ragdoll ragdoll = Spawn(f.world, tmpl, poses);
  REQUIRE(ragdoll.valid());
  REQUIRE(ragdoll.bodies().size() == 3);
  CHECK(ragdoll.IsActive());

  for (int i = 0; i < 600; ++i) {
    f.Step();
    REQUIRE(AllFinite(f.world, ragdoll));
  }
  for (int part = 0; part < 3; ++part) {
    CHECK(State(f.world, ragdoll, part).pose.position.y > -0.02f);
    CHECK(State(f.world, ragdoll, part).pose.position.y < 0.5f);
  }
  CHECK((!ragdoll.IsActive() || MaxSpeed(f.world, ragdoll) < 0.1f));
}

TEST_CASE("the hinge limits hold and its sign convention is right") {
  struct Push {
    glm::vec3 impulse;
    float expect_min;  // The angle must reach at least / at most this.
    float expect_max;
  };
  const float kMin = -30.0f;
  const float kMax = 60.0f;
  const Push pushes[] = {
      {glm::vec3(0.0f, 0.5f, 0.0f), 55.0f, 1000.0f},     // Bend towards +Y.
      {glm::vec3(0.0f, -0.5f, 0.0f), -1000.0f, -25.0f},  // And towards -Y.
      {glm::vec3(0.0f, 0.0f, 0.5f), -1000.0f, 1000.0f},  // Along the axis.
  };
  for (const Push& push : pushes) {
    CAPTURE(push.impulse.y);
    Fixture f(false, false);
    const RagdollTemplate tmpl = MakeTemplate(HingeDesc(kMin, kMax));
    Ragdoll ragdoll = Spawn(f.world, tmpl, RestPoses(tmpl));
    f.world.AddImpulseAtPoint(ragdoll.bodies()[1], push.impulse,
                              glm::vec3(0.4f, 0.0f, 0.0f));
    float lowest = 1000.0f;
    float highest = -1000.0f;
    float worst_off_axis = 0.0f;
    for (int i = 0; i < 300; ++i) {
      f.Step();
      const glm::quat rel =
          Relative(State(f.world, ragdoll, 0), State(f.world, ragdoll, 1));
      const float angle = AngleAboutZ(rel);
      lowest = std::min(lowest, angle);
      highest = std::max(highest, angle);
      worst_off_axis = std::max(
          worst_off_axis,
          glm::degrees(2.0f * std::max(std::abs(rel.x), std::abs(rel.y))));
    }
    CHECK(lowest >= kMin - 3.0f);
    CHECK(highest <= kMax + 3.0f);
    CHECK(worst_off_axis < 5.0f);
    CHECK(highest >= push.expect_min);
    CHECK(lowest <= push.expect_max);
  }
}

TEST_CASE("the elliptic cone follows the documented directions") {
  // cone_reference (towards +-reference = X) 20, cone_side (towards
  // +-cross(reference, primary) = Z) 55.
  struct Push {
    glm::vec3 impulse;
    bool along_x;
    float limit;
  };
  const Push pushes[] = {
      {glm::vec3(0.8f, 0.0f, 0.0f), true, 20.0f},
      {glm::vec3(-0.8f, 0.0f, 0.0f), true, 20.0f},
      {glm::vec3(0.0f, 0.0f, 0.8f), false, 55.0f},
      {glm::vec3(0.0f, 0.0f, -0.8f), false, 55.0f},
  };
  for (const Push& push : pushes) {
    CAPTURE(push.impulse.x);
    CAPTURE(push.impulse.z);
    Fixture f(false, false);
    const RagdollTemplate tmpl =
        MakeTemplate(ConeDesc(20.0f, 55.0f, -30.0f, 30.0f));
    Ragdoll ragdoll = Spawn(f.world, tmpl, RestPoses(tmpl));
    f.world.AddImpulseAtPoint(ragdoll.bodies()[1], push.impulse,
                              glm::vec3(0.0f, 0.4f, 0.0f));
    float largest = 0.0f;
    float off_plane = 0.0f;
    for (int i = 0; i < 300; ++i) {
      f.Step();
      const Tilt tilt = TiltOf(
          Relative(State(f.world, ragdoll, 0), State(f.world, ragdoll, 1)));
      const float along = push.along_x ? tilt.towards_x : tilt.towards_z;
      const float across = push.along_x ? tilt.towards_z : tilt.towards_x;
      largest = std::max(largest, std::abs(along));
      off_plane = std::max(off_plane, std::abs(across));
    }
    CHECK(largest <= push.limit + 3.0f);
    CHECK(largest >= push.limit - 3.0f);
    CHECK(off_plane < 5.0f);
  }
}

TEST_CASE("the twist limits hold with the right sign") {
  for (const float sign : {1.0f, -1.0f}) {
    CAPTURE(sign);
    Fixture f(false, false);
    // Asymmetric limits so a swapped sign shows up.
    const RagdollTemplate tmpl =
        MakeTemplate(ConeDesc(40.0f, 40.0f, -20.0f, 50.0f));
    Ragdoll ragdoll = Spawn(f.world, tmpl, RestPoses(tmpl));
    // Two opposite impulses: a pure torque about Y.
    f.world.AddImpulseAtPoint(ragdoll.bodies()[1],
                              glm::vec3(0.0f, 0.0f, -3.0f * sign),
                              glm::vec3(0.05f, 0.2f, 0.0f));
    f.world.AddImpulseAtPoint(ragdoll.bodies()[1],
                              glm::vec3(0.0f, 0.0f, 3.0f * sign),
                              glm::vec3(-0.05f, 0.2f, 0.0f));
    float lowest = 1000.0f;
    float highest = -1000.0f;
    for (int i = 0; i < 300; ++i) {
      f.Step();
      const float angle = AngleAboutY(
          Relative(State(f.world, ragdoll, 0), State(f.world, ragdoll, 1)));
      lowest = std::min(lowest, angle);
      highest = std::max(highest, angle);
    }
    // At the 30 rad/s angular velocity clamp a part turns 0.5 rad per step,
    // so one step may overshoot by a few degrees before position correction
    // pulls it back; it must not stay there.
    CHECK(lowest >= -20.0f - 6.0f);
    CHECK(highest <= 50.0f + 6.0f);
    const float settled = AngleAboutY(
        Relative(State(f.world, ragdoll, 0), State(f.world, ragdoll, 1)));
    CHECK(settled >= -20.0f - 1.0f);
    CHECK(settled <= 50.0f + 1.0f);
    if (sign > 0.0f) {
      CHECK(highest >= 45.0f);
    } else {
      CHECK(lowest <= -15.0f);
    }
  }
}

TEST_CASE("a spawn outside a limit comes back without a kick") {
  SUBCASE("hinge, 40 degrees past the maximum") {
    Fixture f(false, false);
    const RagdollTemplate tmpl = MakeTemplate(HingeDesc(-30.0f, 60.0f));
    std::vector<Pose> poses = RestPoses(tmpl);
    const glm::quat turned = glm::angleAxis(glm::radians(100.0f), kZ);
    poses[1].rotation = turned;
    poses[1].position = turned * tmpl.rest_pose(1).position;
    Ragdoll ragdoll = Spawn(f.world, tmpl, poses);
    // Position correction removes about 8 % of the error per step: most of
    // the 40 degrees are gone after 30 steps, all of them after 90.
    float peak = 0.0f;
    float angle_after_30 = 0.0f;
    for (int i = 0; i < 90; ++i) {
      f.Step();
      peak = std::max(peak, MaxSpeed(f.world, ragdoll));
      if (i == 29) {
        angle_after_30 = AngleAboutZ(
            Relative(State(f.world, ragdoll, 0), State(f.world, ragdoll, 1)));
      }
    }
    const float angle = AngleAboutZ(
        Relative(State(f.world, ragdoll, 0), State(f.world, ragdoll, 1)));
    CHECK(angle_after_30 <= 60.0f + 4.0f);
    CHECK(angle <= 60.5f);
    CHECK(angle >= 50.0f);
    CHECK(peak < 3.0f);
  }
  SUBCASE("cone, 40 degrees past the reference limit") {
    Fixture f(false, false);
    const RagdollTemplate tmpl =
        MakeTemplate(ConeDesc(20.0f, 55.0f, -30.0f, 30.0f));
    std::vector<Pose> poses = RestPoses(tmpl);
    const glm::quat turned =
        glm::angleAxis(glm::radians(-60.0f), kZ);  // Y -> +X
    poses[1].rotation = turned;
    poses[1].position = turned * tmpl.rest_pose(1).position;
    Ragdoll ragdoll = Spawn(f.world, tmpl, poses);
    float peak = 0.0f;
    float tilt_after_30 = 0.0f;
    for (int i = 0; i < 90; ++i) {
      f.Step();
      peak = std::max(peak, MaxSpeed(f.world, ragdoll));
      if (i == 29) {
        tilt_after_30 = std::abs(TiltOf(Relative(State(f.world, ragdoll, 0),
                                                 State(f.world, ragdoll, 1)))
                                     .towards_x);
      }
    }
    const Tilt tilt = TiltOf(
        Relative(State(f.world, ragdoll, 0), State(f.world, ragdoll, 1)));
    CHECK(tilt_after_30 <= 20.0f + 4.5f);
    CHECK(std::abs(tilt.towards_x) <= 20.5f);
    CHECK(peak < 3.0f);
  }
}

TEST_CASE("parent and child do not collide, an explicit pair does not either") {
  SUBCASE("a pair folded onto each other stays calm") {
    Fixture f(false, false);
    const RagdollTemplate tmpl = MakeTemplate(HingeDesc(-5.0f, 155.0f));
    std::vector<Pose> poses = RestPoses(tmpl);
    const glm::quat folded = glm::angleAxis(glm::radians(150.0f), kZ);
    poses[1].rotation = folded;
    poses[1].position = folded * tmpl.rest_pose(1).position;
    Ragdoll ragdoll = Spawn(f.world, tmpl, poses);
    float peak = 0.0f;
    for (int i = 0; i < 60; ++i) {
      f.Step();
      peak = std::max(peak, MaxSpeed(f.world, ragdoll));
    }
    CHECK(AllFinite(f.world, ragdoll));
    CHECK(peak < 0.05f);
    // Collisions are resolved by position correction: had the two collided
    // they would have been pushed apart.
    CHECK(AngleAboutZ(Relative(State(f.world, ragdoll, 0),
                               State(f.world, ragdoll, 1))) ==
          doctest::Approx(150.0f).epsilon(0.01));
  }

  // A root with two free children on opposite sides; the second child is
  // swung onto the first. The two do not touch at rest, so only an explicit
  // pair can keep them from colliding.
  const auto sibling_gap = [](bool no_collide) {
    Fixture f(false, false);
    const glm::quat along_x = glm::angleAxis(glm::radians(-90.0f), kZ);
    RagdollDesc desc;
    desc.stabilize = false;
    desc.parts.push_back(
        Part(-1, Capsule(0.05f, 0.05f), glm::vec3(0.0f), 5.0f));
    const RagdollJointDesc free_joint = SwingTwistJoint(
        glm::vec3(0.0f), kY, kX, 180.0f, 180.0f, -180.0f, 180.0f);
    desc.parts.push_back(Part(0, Capsule(0.05f, 0.15f, along_x),
                              glm::vec3(0.25f, 0.0f, 0.0f), 1.0f, free_joint));
    desc.parts.push_back(Part(0, Capsule(0.05f, 0.15f, along_x),
                              glm::vec3(-0.25f, 0.0f, 0.0f), 1.0f, free_joint));
    if (no_collide) {
      desc.no_collide.emplace_back(1, 2);
    }
    const RagdollTemplate tmpl = MakeTemplate(desc);
    std::vector<Pose> poses = RestPoses(tmpl);
    // Not a full half turn: two coincident capsules have no contact normal.
    const glm::quat half_turn = glm::angleAxis(glm::radians(175.0f), kY);
    poses[2].rotation = half_turn;
    poses[2].position = half_turn * tmpl.rest_pose(2).position;
    Ragdoll ragdoll = Spawn(f.world, tmpl, poses);
    f.Step(30);
    // Penetration is resolved by position correction, not by velocity, so
    // the separation of the two children shows whether they collided.
    return std::abs(State(f.world, ragdoll, 2).pose.position.z -
                    State(f.world, ragdoll, 1).pose.position.z);
  };
  SUBCASE("a no_collide pair passes through each other") {
    // They stay where they were spawned: 2.2 cm apart, deep inside each other.
    CHECK(sibling_gap(true) < 0.03f);
  }
  SUBCASE("without the pair the siblings push each other apart") {
    CHECK(sibling_gap(false) > 0.08f);  // Two radii, less the contact slop.
  }
}

TEST_CASE("two different ragdolls collide") {
  Fixture f(false, false);
  const RagdollTemplate tmpl = MakeTemplate(SphereDesc(0.2f, 5.0f));
  Ragdoll target = Spawn(f.world, tmpl, RestPoses(tmpl));
  Ragdoll shot = Spawn(f.world, tmpl, RestPoses(tmpl, glm::vec3(1.0f, 0, 0)),
                       glm::vec3(-3.0f, 0.0f, 0.0f));
  float pushed = 0.0f;
  for (int i = 0; i < 60; ++i) {
    f.Step();
    pushed = std::min(pushed, State(f.world, target, 0).linear_velocity.x);
  }
  CHECK(pushed < -1.0f);
}

TEST_CASE("a dynamic box dropped on a lying ragdoll rests on it") {
  Fixture f;
  RagdollDesc desc;
  desc.parts.push_back(
      Part(-1, Box(glm::vec3(0.5f, 0.05f, 0.5f)), glm::vec3(0.0f), 20.0f));
  const RagdollTemplate tmpl = MakeTemplate(desc);
  Ragdoll slab = Spawn(f.world, tmpl, RestPoses(tmpl, glm::vec3(0, 0.05f, 0)));
  f.Step(60);

  BodyDesc box;
  box.body.shape = scene::ShapeType::kBox;
  box.body.motion = scene::MotionType::kDynamic;
  box.body.mass = 5.0f;
  box.pose.position = glm::vec3(0.0f, 0.6f, 0.0f);
  box.bounds = asset::Bounds{glm::vec3(-0.15f), glm::vec3(0.15f)};
  const core::Result<BodyId, PhysicsError> created = f.world.CreateBody(box);
  REQUIRE(created.ok());
  f.Step(240);
  // On the ground it would rest at 0.15; on the 10 cm slab at 0.25.
  const float height = f.world.GetBodyState(created.value()).pose.position.y;
  CHECK(height == doctest::Approx(0.25f).epsilon(0.05));
}

TEST_CASE("ray casts skip ragdoll parts unless asked for everything") {
  Fixture f(false, false);
  const BodyId wall = f.AddStaticBox(
      asset::Bounds{glm::vec3(-5.0f, -5.0f, 0.0f), glm::vec3(5.0f, 5.0f, 0.0f)},
      glm::vec3(0.0f, 0.0f, -3.0f));
  const RagdollTemplate tmpl = MakeTemplate(SphereDesc(0.3f, 5.0f));
  Ragdoll ragdoll = Spawn(f.world, tmpl, RestPoses(tmpl, glm::vec3(0, 1, 0)));
  const glm::vec3 origin(0.0f, 1.0f, 3.0f);
  const glm::vec3 direction(0.0f, 0.0f, -1.0f);

  const std::optional<RayHit> three_args =
      f.world.CastRay(origin, direction, 10.0f);
  REQUIRE(three_args.has_value());
  CHECK(three_args->body == wall);
  const std::optional<RayHit> gameplay =
      f.world.CastRay(origin, direction, 10.0f, RayFilter::kGameplay);
  REQUIRE(gameplay.has_value());
  CHECK(gameplay->body == wall);
  CHECK(gameplay->fraction == doctest::Approx(0.6f));

  const std::optional<RayHit> everything =
      f.world.CastRay(origin, direction, 10.0f, RayFilter::kEverything);
  REQUIRE(everything.has_value());
  CHECK(everything->body == ragdoll.bodies()[0]);
  CHECK(everything->fraction * 10.0f == doctest::Approx(2.7f).epsilon(0.01));
}

// Walks a character along +X through a slab lying in its way; returns where
// it ends up and how far the slab moved.
struct WalkResult {
  glm::vec3 feet;
  float slab_speed;
  glm::vec3 slab_shift;
};
WalkResult WalkThrough(bool with_ragdoll) {
  Fixture f;
  CharacterDesc character_desc;
  core::Result<std::unique_ptr<Character>, PhysicsError> created =
      Character::Create(f.world, character_desc, glm::vec3(-2.0f, 0.3f, 0.0f));
  REQUIRE(created.ok());
  std::unique_ptr<Character> character = std::move(created.value());

  RagdollDesc desc;
  desc.parts.push_back(
      Part(-1, Box(glm::vec3(0.5f, 0.2f, 0.5f)), glm::vec3(0.0f), 20.0f));
  const RagdollTemplate tmpl = MakeTemplate(desc);
  std::optional<Ragdoll> slab;
  glm::vec3 before(0.0f);
  if (with_ragdoll) {
    slab.emplace(Spawn(f.world, tmpl, RestPoses(tmpl, glm::vec3(0, 0.2f, 0))));
    f.Step(30);
    before = State(f.world, *slab, 0).pose.position;
  } else {
    f.Step(30);
  }
  character->SetDesiredVelocity(glm::vec3(2.0f, 0.0f, 0.0f));
  f.Step(180);
  WalkResult result{.feet = character->position(),
                    .slab_speed = 0.0f,
                    .slab_shift = glm::vec3(0.0f)};
  if (slab.has_value()) {
    result.slab_speed = glm::length(f.world.LinearVelocity(slab->bodies()[0]));
    result.slab_shift = State(f.world, *slab, 0).pose.position - before;
  }
  return result;
}

TEST_CASE("a character walks through a lying ragdoll as if it were not there") {
  const WalkResult without = WalkThrough(false);
  const WalkResult with = WalkThrough(true);
  CHECK(without.feet.x > 1.0f);  // It really crossed x = 0.
  CHECK(with.feet == without.feet);
  CHECK(with.slab_speed < 0.01f);
  CHECK(glm::length(with.slab_shift) < 0.005f);
}

TEST_CASE("an impulse on a part changes its velocity") {
  Fixture f(false, false);
  const RagdollTemplate tmpl = MakeTemplate(SphereDesc(0.2f, 2.0f));
  Ragdoll ragdoll = Spawn(f.world, tmpl, RestPoses(tmpl));
  const BodyId part = ragdoll.bodies()[0];
  CHECK(f.world.IsDynamic(part));
  CHECK(glm::length(f.world.LinearVelocity(part)) < 1.0e-6f);
  f.world.AddImpulseAtPoint(part, glm::vec3(2.0f, 0.0f, 0.0f), glm::vec3(0.0f));
  CHECK(f.world.LinearVelocity(part).x == doctest::Approx(1.0f));
}

TEST_CASE("the spawn velocity reaches every part") {
  Fixture f(false, false);
  const RagdollTemplate tmpl = MakeTemplate(ChainDesc(3));
  Ragdoll ragdoll =
      Spawn(f.world, tmpl, RestPoses(tmpl), glm::vec3(0, 0, -4.0f));
  for (const BodyId body : ragdoll.bodies()) {
    CHECK(f.world.LinearVelocity(body).z == doctest::Approx(-4.0f));
  }
}

TEST_CASE("interpolated part poses lie between two consecutive steps") {
  Fixture f;
  const RagdollTemplate tmpl = MakeTemplate(SphereDesc(0.2f, 2.0f));
  Ragdoll ragdoll = Spawn(f.world, tmpl, RestPoses(tmpl, glm::vec3(0, 5, 0)));
  const BodyId part = ragdoll.bodies()[0];
  f.Step();
  const float first = f.world.GetBodyState(part).pose.position.y;
  f.Step();
  const float second = f.world.GetBodyState(part).pose.position.y;
  REQUIRE(second < first);
  f.world.SetInterpolationAlpha(0.5f);
  const float middle = f.world.InterpolatedPose(part).position.y;
  CHECK(middle == doctest::Approx(0.5f * (first + second)).epsilon(1.0e-5));
  CHECK(middle < first);
  CHECK(middle > second);
  f.world.SetInterpolationAlpha(1.0f);
  CHECK(f.world.InterpolatedPose(part).position.y ==
        doctest::Approx(second).epsilon(1.0e-5));
}

TEST_CASE("RemoveBody ignores parts and destroying the ragdoll removes them") {
  Fixture f;
  const int before = f.world.Stats().bodies;
  const RagdollTemplate tmpl = MakeTemplate(ChainDesc(3));
  {
    Ragdoll ragdoll = Spawn(f.world, tmpl, RestPoses(tmpl, glm::vec3(0, 1, 0)));
    CHECK(f.world.Stats().bodies == before + 3);
    f.world.RemoveBody(ragdoll.bodies()[1]);
    CHECK(f.world.Stats().bodies == before + 3);
    CHECK(f.world.IsDynamic(ragdoll.bodies()[1]));
    CHECK(ragdoll.valid());
    f.Step(10);
    CHECK(AllFinite(f.world, ragdoll));
  }
  CHECK(f.world.Stats().bodies == before);
  f.Step(10);
}

TEST_CASE("a moved ragdoll keeps its bodies and the old handle lets go") {
  Fixture f;
  const int before = f.world.Stats().bodies;
  const RagdollTemplate tmpl = MakeTemplate(ChainDesc(2));
  Ragdoll first = Spawn(f.world, tmpl, RestPoses(tmpl, glm::vec3(0, 1, 0)));
  Ragdoll second = std::move(first);
  CHECK_FALSE(first.valid());  // NOLINT(bugprone-use-after-move)
  CHECK(first.bodies().empty());
  CHECK(second.valid());
  CHECK(second.bodies().size() == 2);
  CHECK(f.world.Stats().bodies == before + 2);

  Ragdoll third = Spawn(f.world, tmpl, RestPoses(tmpl, glm::vec3(2, 1, 0)));
  CHECK(f.world.Stats().bodies == before + 4);
  third = std::move(second);  // Releases the ragdoll `third` held.
  CHECK(f.world.Stats().bodies == before + 2);
  CHECK(third.valid());
}

TEST_CASE("RemoveAllBodies with a live ragdoll is safe") {
  Fixture f;
  const RagdollTemplate tmpl = MakeTemplate(ChainDesc(3));
  Ragdoll ragdoll = Spawn(f.world, tmpl, RestPoses(tmpl, glm::vec3(0, 1, 0)));
  REQUIRE(ragdoll.valid());
  f.world.RemoveAllBodies();
  CHECK_FALSE(ragdoll.valid());
  CHECK(ragdoll.bodies().empty());
  CHECK_FALSE(ragdoll.IsActive());
  CHECK(f.world.Stats().bodies == 0);
  f.Step(10);
  // The handle may now be destroyed (or a new ragdoll spawned).
  ragdoll = Spawn(f.world, tmpl, RestPoses(tmpl));
  CHECK(ragdoll.valid());
  f.world.RemoveAllBodies();
  f.Step(2);
}

TEST_CASE("a bullet stops at a ragdoll part only when bullets hit cosmetics") {
  const auto fire = [](bool hits_cosmetic) {
    Fixture f(false, false);
    const BodyId wall =
        f.AddStaticBox(asset::Bounds{glm::vec3(-5.0f, -5.0f, 0.0f),
                                     glm::vec3(5.0f, 5.0f, 0.0f)},
                       glm::vec3(0.0f, 0.0f, -20.0f));
    const RagdollTemplate tmpl = MakeTemplate(SphereDesc(0.3f, 5.0f));
    Ragdoll ragdoll = Spawn(f.world, tmpl, RestPoses(tmpl, glm::vec3(0, 1, 0)));

    weapon::ProjectileSystem system(
        weapon::ProjectileTuning{.hits_cosmetic = hits_cosmetic});
    system.Spawn(glm::vec3(0.0f, 1.0f, 5.0f), glm::vec3(0.0f, 0.0f, -300.0f),
                 0.004f, 0.0f);
    system.Update(f.world, glm::vec3(0.0f), 0.0f);
    system.Update(f.world, glm::vec3(0.0f), 0.1f);
    REQUIRE(system.impacts().size() == 1);
    struct Outcome {
      bool hit_part;
      bool hit_wall;
      float part_velocity_z;
    };
    return Outcome{system.impacts()[0].body == ragdoll.bodies()[0],
                   system.impacts()[0].body == wall,
                   f.world.LinearVelocity(ragdoll.bodies()[0]).z};
  };
  const auto with = fire(true);
  CHECK(with.hit_part);
  // The bullet's momentum 0.004 kg * 300 m/s over the part's 5 kg.
  CHECK(with.part_velocity_z == doctest::Approx(-0.24f).epsilon(0.05));
  const auto without = fire(false);
  CHECK(without.hit_wall);
  CHECK(without.part_velocity_z == doctest::Approx(0.0f).epsilon(1.0e-6));
}

}  // namespace
}  // namespace blue::physics
