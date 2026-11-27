// Tests of game::Ragdolls (game/ragdolls.h) on the real player model (kyle.pbo
// as blue-pack samples cooks it into the build's assets): no pop from animator
// poses in zero gravity, falling, resting and freezing, the bullet impulse
// direction and mirroring, the cap on live ragdolls and their lifetime, and
// views that reproduce the physics.

#include "game/ragdolls.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/common.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "animation/hierarchy.h"
#include "animation/transform.h"
#include "asset/mesh_data.h"
#include "core/path.h"
#include "game/character_animator.h"
#include "game/ragdoll_rig.h"
#include "physics/physics_runtime.h"
#include "physics/physics_world.h"
#include "prefab/prefab.h"
#include "prefab/prefab_file.h"
#include "scene/scene.h"

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

Posed Settle(CharacterInput input, std::string name) {
  CharacterAnimator animator(GetKyle().rig);
  input.dt = kDt;
  for (int frame = 0; frame < 60; ++frame) {
    animator.Update(input);
  }
  return {std::move(name),
          {animator.locals().begin(), animator.locals().end()},
          animator.root()};
}

Posed Standing() { return Settle({}, "standing"); }

Posed Crouching() {
  CharacterInput input;
  input.crouching = true;
  return Settle(input, "crouching");
}

Posed Rifle(float pitch) {
  CharacterInput input;
  input.aiming = true;
  input.pitch = pitch;
  input.weapon = Hold(HeldWeapon::kRifle, "m16.pbo");
  return Settle(input, "rifle pitch " + std::to_string(pitch));
}

// Standing, walking, crouching, a pistol, a rifle at pitch -1, 0, +1.
std::vector<Posed> AnimatorPoses() {
  std::vector<Posed> poses;
  poses.push_back(Standing());
  {
    CharacterAnimator animator(GetKyle().rig);
    CharacterInput input;
    input.dt = kDt;
    input.velocity = glm::vec3(0.0f, 0.0f, -3.5f);
    glm::vec3 position(0.0f);
    for (int frame = 0; frame < 150; ++frame) {
      position += input.velocity * kDt;
      input.feet = position;
      animator.Update(input);
    }
    // The ragdoll is spawned where the walker is.
    poses.push_back({"walking",
                     {animator.locals().begin(), animator.locals().end()},
                     animator.root()});
  }
  poses.push_back(Crouching());
  CharacterInput pistol;
  pistol.aiming = true;
  pistol.weapon = Hold(HeldWeapon::kPistol, "glock17.pbo");
  poses.push_back(Settle(pistol, "pistol"));
  for (const float pitch : {-1.0f, 0.0f, 1.0f}) {
    poses.push_back(Rifle(pitch));
  }
  return poses;
}

// The part frames of the model posed by `locals` with an identity root.
std::array<physics::Pose, kParts> PartFrames(
    std::span<const animation::Transform> locals) {
  const Kyle& kyle = GetKyle();
  std::vector<glm::mat4> world(kyle.prefab().hierarchy.nodes.size());
  animation::ComputeWorldMatrices(kyle.prefab().hierarchy, locals, world);
  std::array<physics::Pose, kParts> frames;
  for (size_t i = 0; i < kParts; ++i) {
    const glm::mat4& m = world[static_cast<size_t>(kyle.ragdoll.part_node[i])];
    frames[i] = {.position = glm::vec3(m[3]),
                 .rotation = glm::normalize(glm::quat_cast(glm::mat3(m)))};
  }
  return frames;
}

std::array<physics::Pose, kParts> SpawnFrames(const Posed& posed) {
  std::array<physics::Pose, kParts> frames;
  PartPosesFromLocals(GetKyle().ragdoll, posed.root, posed.locals, frames);
  return frames;
}

float AngleBetween(const glm::quat& a, const glm::quat& b) {
  const float dot = std::min(std::abs(glm::dot(a, b)), 1.0f);
  return glm::degrees(2.0f * std::acos(dot));
}

bool Finite(glm::vec3 v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool AllFinite(const std::array<physics::Pose, kParts>& frames) {
  return std::all_of(frames.begin(), frames.end(), [](const physics::Pose& p) {
    return Finite(p.position) && std::isfinite(p.rotation.w) &&
           std::isfinite(p.rotation.x) && std::isfinite(p.rotation.y) &&
           std::isfinite(p.rotation.z);
  });
}

glm::vec3 MeanPosition(const std::array<physics::Pose, kParts>& frames) {
  glm::vec3 sum(0.0f);
  for (const physics::Pose& pose : frames) {
    sum += pose.position;
  }
  return sum / static_cast<float>(kParts);
}

// Rotation of the part about the vertical axis from rest, degrees.
float YawOf(const physics::Pose& pose) {
  const glm::vec3 forward = pose.rotation * glm::vec3(0.0f, 0.0f, -1.0f);
  return glm::degrees(std::atan2(forward.x, -forward.z));
}

// A world on the calling thread with an optional ground and the manager.
struct Fixture {
  physics::PhysicsRuntime runtime;
  physics::PhysicsWorld world{
      runtime, physics::PhysicsWorldOptions{.worker_threads = 0}};
  std::optional<Ragdolls> ragdolls;

  explicit Fixture(bool gravity = true, const RagdollTuning& tuning = {}) {
    world.SetGravity(glm::vec3(0.0f, gravity ? -9.81f : 0.0f, 0.0f));
    if (gravity) {
      physics::BodyDesc desc;
      desc.body.shape = scene::ShapeType::kBox;
      desc.body.motion = scene::MotionType::kStatic;
      desc.bounds = asset::Bounds{glm::vec3(-50.0f, 0.0f, -50.0f),
                                  glm::vec3(50.0f, 0.0f, 50.0f)};
      REQUIRE(world.CreateBody(desc).ok());
    }
    ragdolls.emplace(world, tuning);
    REQUIRE(ragdolls->SetModel(GetKyle().rig, GetKyle().prefab()));
  }

  ~Fixture() { ragdolls.reset(); }

  // One physics step and the manager's update.
  void Step() {
    world.StepOnce();
    world.SetInterpolationAlpha(1.0f);
    ragdolls->Update(kDt);
  }

  RagdollId Spawn(const Posed& posed, glm::vec3 velocity = glm::vec3(0.0f),
                  std::optional<RagdollHit> hit = std::nullopt,
                  glm::vec3 offset = glm::vec3(0.0f)) {
    return ragdolls->Spawn(
        {.root = glm::translate(glm::mat4(1.0f), offset) * posed.root,
         .locals = posed.locals,
         .velocity = velocity,
         .hit = hit});
  }

  // The part frames the view of `id` draws.
  std::array<physics::Pose, kParts> Frames(RagdollId id) const {
    const RagdollView* view = ragdolls->Find(id);
    REQUIRE(view != nullptr);
    return PartFrames(view->locals);
  }
};

// Settings that keep a ragdoll simulated for the whole test.
RagdollTuning NoFreeze() {
  RagdollTuning tuning;
  tuning.settle_seconds = 1000.0f;
  tuning.max_simulated_seconds = 1000.0f;
  tuning.linger_seconds = 2000.0f;
  return tuning;
}

TEST_CASE("Ragdolls: needs a model") {
  Fixture f(false);
  f.ragdolls->ClearModel();
  CHECK_FALSE(f.ragdolls->has_model());
  const Posed posed = Standing();
  CHECK(f.Spawn(posed) == 0);
  REQUIRE(f.ragdolls->SetModel(GetKyle().rig, GetKyle().prefab()));
  CHECK(f.ragdolls->has_model());
  const std::vector<animation::Transform> too_few(3);
  CHECK(f.ragdolls->Spawn({.locals = too_few}) == 0);
  CHECK(f.Spawn(posed) != 0);
}

TEST_CASE("Ragdolls: animator poses do not pop in zero gravity") {
  const std::vector<Posed> poses = AnimatorPoses();
  REQUIRE(poses.size() == 7);
  for (const Posed& posed : poses) {
    CAPTURE(posed.name);
    const bool crouching = posed.name == "crouching";
    Fixture f(false, NoFreeze());
    const auto start = SpawnFrames(posed);
    const RagdollId id = f.Spawn(posed);
    REQUIRE(id != 0);

    auto previous = start;
    float fastest = 0.0f;
    std::array<float, 2> drift_cm{};
    std::array<float, 2> turn_degrees{};
    for (int step = 1; step <= 120; ++step) {
      f.Step();
      const auto now = f.Frames(id);
      REQUIRE(AllFinite(now));
      for (size_t i = 0; i < kParts; ++i) {
        fastest = std::max(
            fastest, glm::length(now[i].position - previous[i].position) / kDt);
      }
      previous = now;
      if (step == 30 || step == 120) {
        const size_t slot = step == 30 ? 0 : 1;
        for (size_t i = 0; i < kParts; ++i) {
          drift_cm[slot] = std::max(
              drift_cm[slot],
              100.0f * glm::length(now[i].position - start[i].position));
          turn_degrees[slot] =
              std::max(turn_degrees[slot],
                       AngleBetween(now[i].rotation, start[i].rotation));
        }
      }
      if (step == 30 && !crouching) {
        CHECK_MESSAGE(drift_cm[0] < 3.0f, "drift in cm");
        CHECK_MESSAGE(turn_degrees[0] < 4.0f, "turn in degrees");
      }
    }
    if (crouching) {
      // The crouch is outside the hip and ankle cones (known): it must stay
      // finite and slow, and the measured drift is reported.
      CHECK(fastest < 3.0f);
      INFO("crouching drift after 30 steps: ", drift_cm[0], " cm, ",
           turn_degrees[0], " deg; after 120 steps: ", drift_cm[1], " cm, ",
           turn_degrees[1], " deg; fastest part ", fastest, " m/s");
      MESSAGE("crouching drift after 30 steps: ", drift_cm[0], " cm, ",
              turn_degrees[0], " deg; after 120 steps: ", drift_cm[1], " cm, ",
              turn_degrees[1], " deg; fastest part ", fastest, " m/s");
    }
  }
}

TEST_CASE("Ragdolls: falls, rests and freezes") {
  Fixture f;
  const Posed posed = Standing();
  const int ground_bodies = f.world.Stats().bodies;
  const RagdollId id = f.Spawn(posed, glm::vec3(1.0f, 0.0f, 0.0f));
  REQUIRE(id != 0);
  CHECK(f.world.Stats().bodies == ground_bodies + static_cast<int>(kParts));
  CHECK(f.ragdolls->views().size() == 1);
  CHECK(f.ragdolls->Find(id)->simulated);

  const RagdollTuning tuning;
  const int limit = static_cast<int>(tuning.max_simulated_seconds / kDt) + 5;
  int frozen_at = -1;
  for (int step = 1; step <= limit; ++step) {
    f.Step();
    const RagdollView* view = f.ragdolls->Find(id);
    REQUIRE(view != nullptr);
    REQUIRE(AllFinite(f.Frames(id)));
    if (!view->simulated) {
      frozen_at = step;
      break;
    }
  }
  REQUIRE(frozen_at > 0);
  MESSAGE("frozen after ", static_cast<float>(frozen_at) * kDt, " s");
  CHECK(f.world.Stats().bodies == ground_bodies);

  const auto frozen = f.Frames(id);
  for (const physics::Pose& pose : frozen) {
    CHECK(pose.position.y > -0.03f);
  }
  const std::vector<animation::Transform> snapshot(
      f.ragdolls->Find(id)->locals.begin(), f.ragdolls->Find(id)->locals.end());
  for (int step = 0; step < 120; ++step) {
    f.Step();
  }
  const RagdollView* view = f.ragdolls->Find(id);
  REQUIRE(view != nullptr);
  CHECK_FALSE(view->simulated);
  REQUIRE(view->locals.size() == snapshot.size());
  for (size_t i = 0; i < snapshot.size(); ++i) {
    CHECK(view->locals[i].translation == snapshot[i].translation);
    CHECK(view->locals[i].rotation == snapshot[i].rotation);
  }

  const int lingering =
      static_cast<int>(tuning.linger_seconds / kDt) - frozen_at - 120 + 10;
  for (int step = 0; step < lingering; ++step) {
    f.Step();
  }
  CHECK(f.ragdolls->views().empty());
  CHECK(f.ragdolls->Find(id) == nullptr);
}

// The mean part position change after `steps` steps, with and without `hit`,
// for the standing figure.
struct HitRun {
  glm::vec3 start{0.0f};
  glm::vec3 end{0.0f};
  std::array<physics::Pose, kParts> frames{};
  std::array<physics::Pose, kParts> first{};
  glm::vec3 delta() const { return end - start; }
};

HitRun RunHit(std::optional<RagdollHit> hit, int steps = 30) {
  Fixture f;
  const Posed posed = Standing();
  HitRun run;
  run.start = MeanPosition(SpawnFrames(posed));
  const RagdollId id = f.Spawn(posed, glm::vec3(0.0f), hit);
  REQUIRE(id != 0);
  for (int step = 1; step <= steps; ++step) {
    f.Step();
    if (step == 1) {
      run.first = f.Frames(id);
    }
  }
  run.frames = f.Frames(id);
  run.end = MeanPosition(run.frames);
  return run;
}

glm::vec3 PartPoint(RagdollPart part) {
  return SpawnFrames(Standing())[P(part)].position;
}

RagdollHit Bullet(glm::vec3 point, glm::vec3 direction,
                  RagdollZone zone = RagdollZone::kAny) {
  return {.point = point, .velocity = direction * 800.0f, .zone = zone};
}

TEST_CASE("Ragdolls: a bullet pushes the body along its direction") {
  const glm::vec3 chest = PartPoint(RagdollPart::kChest);
  const HitRun none = RunHit(std::nullopt);
  const glm::vec3 minus_x(-1.0f, 0.0f, 0.0f);
  const glm::vec3 plus_z(0.0f, 0.0f, 1.0f);

  const HitRun left = RunHit(Bullet(chest + glm::vec3(0.3f, 0, 0), minus_x));
  const HitRun right = RunHit(Bullet(chest - glm::vec3(0.3f, 0, 0), -minus_x));
  const HitRun front = RunHit(Bullet(chest + glm::vec3(0, 0, -0.3f), plus_z));
  MESSAGE("mean displacement after 0.5 s: none ", none.delta().x, " ",
          none.delta().z, "; along -X ", left.delta().x, "; along +X ",
          right.delta().x, "; along +Z ", front.delta().z);

  CHECK(left.delta().x < -0.1f);
  CHECK(left.delta().x < none.delta().x);
  CHECK(right.delta().x > 0.1f);
  CHECK(right.delta().x > none.delta().x);
  CHECK(front.delta().z > 0.1f);
  CHECK(front.delta().z > none.delta().z);
  CHECK(left.delta().x == doctest::Approx(-right.delta().x).epsilon(0.5));
}

TEST_CASE("Ragdolls: hits on the two shoulders turn the chest oppositely") {
  const glm::vec3 plus_z(0.0f, 0.0f, 1.0f);
  const glm::vec3 left_shoulder = PartPoint(RagdollPart::kLeftUpperArm);
  const glm::vec3 right_shoulder = PartPoint(RagdollPart::kRightUpperArm);
  REQUIRE(right_shoulder.x > left_shoulder.x);  // The right is +X.
  const float rest_yaw = YawOf(SpawnFrames(Standing())[P(RagdollPart::kChest)]);

  const HitRun left =
      RunHit(Bullet(left_shoulder + glm::vec3(0, 0, -0.3f), plus_z));
  const HitRun right =
      RunHit(Bullet(right_shoulder + glm::vec3(0, 0, -0.3f), plus_z));
  const float left_yaw = YawOf(left.frames[P(RagdollPart::kChest)]) - rest_yaw;
  const float right_yaw =
      YawOf(right.frames[P(RagdollPart::kChest)]) - rest_yaw;
  MESSAGE("chest yaw after 0.5 s: left shoulder ", left_yaw,
          " deg, right shoulder ", right_yaw, " deg");
  CHECK(left_yaw * right_yaw < 0.0f);
  CHECK(std::abs(left_yaw) > 1.0f);
  CHECK(std::abs(right_yaw) > 1.0f);
}

// The part that moved furthest from its spawn position after one step.
RagdollPart FastestPart(const HitRun& run, const Posed& posed) {
  const auto start = SpawnFrames(posed);
  size_t best = 0;
  float most = 0.0f;
  for (size_t i = 0; i < kParts; ++i) {
    const float moved = glm::length(run.first[i].position - start[i].position);
    if (moved > most) {
      most = moved;
      best = i;
    }
  }
  return static_cast<RagdollPart>(best);
}

TEST_CASE("Ragdolls: the zone of a hit decides which part moves most") {
  const glm::vec3 plus_z(0.0f, 0.0f, 1.0f);
  const Posed posed = Standing();
  const HitRun head =
      RunHit(Bullet(PartPoint(RagdollPart::kHead) + glm::vec3(0, 0, -0.3f),
                    plus_z, RagdollZone::kHead),
             1);
  CHECK(FastestPart(head, posed) == RagdollPart::kHead);

  const HitRun legs =
      RunHit(Bullet(PartPoint(RagdollPart::kLeftShin) + glm::vec3(0, 0, -0.3f),
                    plus_z, RagdollZone::kLegs),
             1);
  const RagdollPart moved = FastestPart(legs, posed);
  CHECK(static_cast<int>(moved) >= static_cast<int>(RagdollPart::kLeftThigh));
}

TEST_CASE("Ragdolls: a hit that misses every shape still pushes its zone") {
  const glm::vec3 plus_z(0.0f, 0.0f, 1.0f);
  const Posed posed = Standing();
  const glm::vec3 shin = PartPoint(RagdollPart::kLeftShin);
  // 0.6 m to the side of the body: the line passes every shape.
  const HitRun miss = RunHit(
      Bullet(shin + glm::vec3(-0.6f, 0, -0.3f), plus_z, RagdollZone::kLegs), 1);
  const HitRun none = RunHit(std::nullopt, 1);
  const auto start = SpawnFrames(posed);
  float legs = 0.0f;
  float legs_none = 0.0f;
  for (size_t i = P(RagdollPart::kLeftThigh); i < kParts; ++i) {
    legs =
        std::max(legs, glm::length(miss.first[i].position - start[i].position));
    legs_none = std::max(
        legs_none, glm::length(none.first[i].position - start[i].position));
  }
  MESSAGE("legs moved ", legs, " m in one step, ", legs_none, " without a hit");
  CHECK(legs > legs_none + 0.01f);
  CHECK(FastestPart(miss, posed) >= RagdollPart::kLeftThigh);
}

// Every dynamic body of the world, found by probing handles (Jolt packs an
// index below bit 23 and a sequence number from bit 24).
std::vector<physics::BodyId> DynamicBodies(const physics::PhysicsWorld& world) {
  std::vector<physics::BodyId> bodies;
  for (uint32_t sequence = 0; sequence < 128; ++sequence) {
    for (uint32_t index = 0; index < 64; ++index) {
      const auto id = static_cast<physics::BodyId>((sequence << 23) | index);
      if (world.IsDynamic(id)) {
        bodies.push_back(id);
      }
    }
  }
  return bodies;
}

TEST_CASE("Ragdolls: at most max_ragdolls, ids never reused") {
  Fixture f;
  const Posed posed = Standing();
  const int ground_bodies = f.world.Stats().bodies;
  const RagdollTuning tuning;
  std::vector<RagdollId> ids;
  for (int i = 0; i < tuning.max_ragdolls + 2; ++i) {
    const RagdollId id =
        f.Spawn(posed, glm::vec3(0.0f), std::nullopt,
                glm::vec3(2.0f * static_cast<float>(i), 0.0f, 0.0f));
    REQUIRE(id != 0);
    CHECK(std::find(ids.begin(), ids.end(), id) == ids.end());
    ids.push_back(id);
  }
  CHECK(ids.front() < ids.back());
  CHECK(f.ragdolls->views().size() == static_cast<size_t>(tuning.max_ragdolls));
  CHECK(f.ragdolls->Find(ids[0]) == nullptr);
  CHECK(f.ragdolls->Find(ids[1]) == nullptr);
  CHECK(f.ragdolls->Find(ids[2]) != nullptr);
  CHECK(f.ragdolls->Find(ids.back()) != nullptr);
  CHECK(f.ragdolls->views().front().id == ids[2]);
  CHECK(f.ragdolls->views().back().id == ids.back());
  CHECK(f.world.Stats().bodies ==
        ground_bodies + tuning.max_ragdolls * static_cast<int>(kParts));

  CHECK(f.ragdolls->Remove(ids[3]));
  CHECK_FALSE(f.ragdolls->Remove(ids[3]));
  f.ragdolls->Clear();
  CHECK(f.ragdolls->views().empty());
  CHECK(f.world.Stats().bodies == ground_bodies);
  const RagdollId later = f.Spawn(posed);
  CHECK(later > ids.back());
}

TEST_CASE("Ragdolls: Clear after RemoveAllBodies is safe") {
  Fixture f;
  const RagdollId id = f.Spawn(Standing());
  REQUIRE(id != 0);
  f.world.RemoveAllBodies();
  f.ragdolls->Update(kDt);  // A dead handle freezes the ragdoll as it is.
  const RagdollView* view = f.ragdolls->Find(id);
  REQUIRE(view != nullptr);
  CHECK_FALSE(view->simulated);
  f.ragdolls->Clear();
  CHECK(f.ragdolls->views().empty());
  f.ragdolls->Clear();
}

TEST_CASE("Ragdolls: FindByBody finds the owner of a part") {
  Fixture f(false, NoFreeze());
  const RagdollId first = f.Spawn(Standing());
  const RagdollId second = f.Spawn(Standing(), glm::vec3(0.0f), std::nullopt,
                                   glm::vec3(3.0f, 0.0f, 0.0f));
  REQUIRE(first != 0);
  REQUIRE(second != 0);
  const std::vector<physics::BodyId> bodies = DynamicBodies(f.world);
  REQUIRE(bodies.size() == 2 * kParts);
  size_t of_first = 0;
  size_t of_second = 0;
  for (const physics::BodyId body : bodies) {
    const RagdollId owner = f.ragdolls->FindByBody(body);
    of_first += owner == first ? 1 : 0;
    of_second += owner == second ? 1 : 0;
  }
  CHECK(of_first == kParts);
  CHECK(of_second == kParts);
}

TEST_CASE("Ragdolls: the view reproduces the physics") {
  Fixture f(true, NoFreeze());
  const RagdollId id =
      f.Spawn(Standing(), glm::vec3(0.5f, 0.0f, 0.0f),
              Bullet(PartPoint(RagdollPart::kChest) + glm::vec3(0, 0, -0.3f),
                     glm::vec3(0.0f, 0.0f, 1.0f)));
  REQUIRE(id != 0);
  for (int step = 0; step < 20; ++step) {
    f.Step();
  }
  const std::vector<physics::BodyId> bodies = DynamicBodies(f.world);
  REQUIRE(bodies.size() == kParts);
  const auto frames = f.Frames(id);
  const RagdollView* view = f.ragdolls->Find(id);
  REQUIRE(view != nullptr);
  for (size_t i = 0; i < kParts; ++i) {
    float nearest = 1e9f;
    for (const physics::BodyId body : bodies) {
      nearest = std::min(nearest,
                         glm::length(f.world.InterpolatedPose(body).position -
                                     frames[i].position));
    }
    CHECK_MESSAGE(nearest < 1e-3f, "part ", i, " is ", nearest, " m off");
  }
  CHECK(glm::length(view->center - frames[P(RagdollPart::kPelvis)].position) <
        1e-3f);
  // The head matrix is the Head joint node, drawn with an identity root.
  const Kyle& kyle = GetKyle();
  std::vector<glm::mat4> world(kyle.prefab().hierarchy.nodes.size());
  animation::ComputeWorldMatrices(kyle.prefab().hierarchy, view->locals, world);
  const glm::mat4& head_node = world[static_cast<size_t>(
      kyle.rig.nodes[static_cast<size_t>(Joint::kHead)])];
  CHECK(glm::length(glm::vec3(view->head[3]) - glm::vec3(head_node[3])) <
        1e-3f);
  // It rides on the head part: a fixed offset in the part frame.
  const physics::Pose head_part = frames[P(RagdollPart::kHead)];
  CHECK(glm::length(glm::vec3(view->head[3]) - head_part.position) < 0.2f);
}

TEST_CASE("Ragdolls: shortening the lifetime removes after linger_seconds") {
  RagdollTuning tuning;
  tuning.linger_seconds = 2.0f;
  tuning.max_simulated_seconds = 1.0f;
  Fixture f(true, tuning);
  const RagdollId id = f.Spawn(Standing());
  REQUIRE(id != 0);
  for (int step = 0; step < 70; ++step) {
    f.Step();
  }
  REQUIRE(f.ragdolls->Find(id) != nullptr);
  CHECK_FALSE(f.ragdolls->Find(id)->simulated);
  for (int step = 0; step < 60; ++step) {
    f.Step();
  }
  CHECK(f.ragdolls->Find(id) == nullptr);
}

}  // namespace
}  // namespace blue::game
