#include "game/ragdolls.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "animation/hierarchy.h"
#include "animation/transform.h"
#include "core/log.h"
#include "game/character_animator.h"
#include "game/ragdoll_rig.h"
#include "physics/physics_world.h"
#include "physics/ragdoll.h"
#include "physics/shape_cooking.h"
#include "prefab/prefab.h"

namespace blue::game {
namespace {

constexpr size_t kPartCount = kRagdollPartCount;

constexpr int Index(RagdollPart part) { return static_cast<int>(part); }

// Whether `part` belongs to `zone` (kAny: every part).
bool InZone(RagdollZone zone, int part) {
  switch (zone) {
    case RagdollZone::kAny:
      return true;
    case RagdollZone::kHead:
      return part == Index(RagdollPart::kHead);
    case RagdollZone::kTorso:
      return part <= Index(RagdollPart::kRightForearm);
    case RagdollZone::kLegs:
      return part >= Index(RagdollPart::kLeftThigh);
  }
  return true;
}

// Distance from `point` to the line through `origin` along unit `direction`.
float DistanceToLine(glm::vec3 point, glm::vec3 origin, glm::vec3 direction) {
  const glm::vec3 offset = point - origin;
  return glm::length(offset - direction * glm::dot(offset, direction));
}

}  // namespace

struct Ragdolls::Impl {
  // The model every ragdoll is made from.
  struct Model {
    RagdollRig rig;
    physics::RagdollTemplate tmpl;
    animation::Hierarchy hierarchy;
    int head_node = 0;
  };

  struct Entry {
    RagdollId id = 0;
    uint64_t owner = 0;
    bool simulated = true;
    float age = 0.0f;        // Seconds since the spawn.
    float rest_time = 0.0f;  // Seconds every part has been slow.
    glm::vec3 center{0.0f};
    glm::mat4 head{1.0f};
    std::optional<physics::Ragdoll> body;
    std::array<physics::Pose, kPartCount> poses{};
    std::vector<animation::Transform> locals;
  };

  Impl(physics::PhysicsWorld& world_in, const RagdollTuning& tuning_in)
      : world(world_in), tuning(tuning_in) {
    const size_t cap = static_cast<size_t>(std::max(tuning.max_ragdolls, 1));
    entries.reserve(cap + 1);
    views.reserve(cap + 1);
  }

  // Rewrites the locals, centre and head of `entry` from its `poses`.
  void Refresh(Entry& entry) {
    LocalsFromPartPoses(model->rig, entry.poses, entry.locals);
    animation::ComputeWorldMatrices(model->hierarchy, entry.locals, scratch);
    entry.head = scratch[static_cast<size_t>(model->head_node)];
    entry.center = entry.poses[0].position;
  }

  // Finds the part and world point the bullet `hit` lands on; applies the
  // impulse.
  void ApplyHit(Entry& entry, const RagdollHit& hit) {
    const float speed = glm::length(hit.velocity);
    if (!(speed > 1e-4f) || !std::isfinite(speed)) {
      return;
    }
    const glm::vec3 direction = hit.velocity / speed;
    const glm::vec3 origin = hit.point - direction * tuning.hit_ray_back;
    const float reach = tuning.hit_ray_back + tuning.hit_ray_length;

    int part = -1;
    glm::vec3 point = hit.point;
    float best = reach;
    for (int i = 0; i < model->tmpl.part_count(); ++i) {
      const physics::Pose& pose = entry.poses[static_cast<size_t>(i)];
      const glm::quat inverse = glm::conjugate(pose.rotation);
      const auto result = model->tmpl.shape(i).CastRay(
          inverse * (origin - pose.position), inverse * direction, reach);
      if (result && result->distance <= best) {
        best = result->distance;
        part = i;
        point = pose.position + pose.rotation * result->point;
      }
    }
    if (part < 0) {
      float nearest = 0.0f;
      for (int i = 0; i < model->tmpl.part_count(); ++i) {
        if (!InZone(hit.zone, i)) {
          continue;
        }
        const physics::Pose& pose = entry.poses[static_cast<size_t>(i)];
        const asset::Bounds bounds = model->tmpl.shape(i).bounds();
        const glm::vec3 center =
            pose.position + pose.rotation * ((bounds.min + bounds.max) * 0.5f);
        const float distance = DistanceToLine(center, origin, direction);
        if (part < 0 || distance < nearest) {
          nearest = distance;
          part = i;
          point = center;
        }
      }
    }
    if (part < 0) {
      return;
    }
    const float magnitude = std::clamp(tuning.impulse_scale * hit.mass * speed,
                                       tuning.min_impulse, tuning.max_impulse);
    world.AddImpulseAtPoint(entry.body->bodies()[static_cast<size_t>(part)],
                            direction * magnitude, point);
  }

  // Freezes `entry`: its physics bodies go, the pose stays.
  static void Freeze(Entry& entry) {
    entry.body.reset();
    entry.simulated = false;
  }

  void RebuildViews() {
    views.clear();
    for (const Entry& entry : entries) {
      views.push_back({.id = entry.id,
                       .owner = entry.owner,
                       .simulated = entry.simulated,
                       .center = entry.center,
                       .head = entry.head,
                       .locals = entry.locals});
    }
  }

  physics::PhysicsWorld& world;
  RagdollTuning tuning;
  std::optional<Model> model;
  std::vector<Entry> entries;  // Oldest first.
  std::vector<RagdollView> views;
  std::vector<glm::mat4> scratch;  // World matrices of the nodes.
  RagdollId next_id = 1;
};

Ragdolls::Ragdolls(physics::PhysicsWorld& world, const RagdollTuning& tuning)
    : impl_(std::make_unique<Impl>(world, tuning)) {}

Ragdolls::~Ragdolls() { Clear(); }

bool Ragdolls::SetModel(const CharacterRig& rig, const prefab::Prefab& prefab) {
  Clear();
  impl_->model.reset();
  auto ragdoll_rig = MakeRagdollRig(rig, prefab);
  if (!ragdoll_rig.ok()) {
    core::log::Warn("Ragdoll skeleton not built: {}", ragdoll_rig.error());
    return false;
  }
  auto tmpl = physics::RagdollTemplate::Create(ragdoll_rig.value().desc);
  if (!tmpl.ok()) {
    core::log::Warn("Ragdoll template not created: {}",
                    physics::ToString(tmpl.error()));
    return false;
  }
  impl_->model = Impl::Model{
      .rig = std::move(ragdoll_rig).value(),
      .tmpl = std::move(tmpl).value(),
      .hierarchy = prefab.hierarchy,
      .head_node = rig.nodes[static_cast<size_t>(Joint::kHead)],
  };
  impl_->scratch.assign(prefab.hierarchy.nodes.size(), glm::mat4(1.0f));
  return true;
}

void Ragdolls::ClearModel() {
  Clear();
  impl_->model.reset();
}

bool Ragdolls::has_model() const { return impl_->model.has_value(); }

RagdollId Ragdolls::Spawn(const RagdollSpawn& spawn) {
  Impl& impl = *impl_;
  if (!impl.model || spawn.locals.size() != impl.model->rig.node_part.size()) {
    return 0;
  }
  Impl::Entry entry;
  entry.owner = spawn.owner;
  entry.locals.assign(spawn.locals.begin(), spawn.locals.end());
  PartPosesFromLocals(impl.model->rig, spawn.root, spawn.locals, entry.poses);
  auto body = physics::Ragdoll::Create(
      impl.world, impl.model->tmpl,
      {.poses = entry.poses, .linear_velocity = spawn.velocity});
  if (!body.ok()) {
    core::log::Warn("Ragdoll not spawned: {}", physics::ToString(body.error()));
    return 0;
  }
  entry.body.emplace(std::move(body).value());
  if (spawn.hit) {
    impl.ApplyHit(entry, *spawn.hit);
  }
  // The pose as drawn at spawn: the locals rewritten from the part poses
  // (identity root) rather than the animator's own, so draws agree.
  entry.id = impl.next_id++;
  impl.Refresh(entry);

  // Make room: the oldest go first.
  const size_t cap = static_cast<size_t>(std::max(impl.tuning.max_ragdolls, 1));
  while (impl.entries.size() >= cap) {
    impl.entries.erase(impl.entries.begin());
  }
  impl.entries.push_back(std::move(entry));
  impl.RebuildViews();
  return impl.entries.back().id;
}

void Ragdolls::Update(float dt) {
  Impl& impl = *impl_;
  if (!impl.model) {
    return;
  }
  const RagdollTuning& tuning = impl.tuning;
  for (Impl::Entry& entry : impl.entries) {
    entry.age += dt;
    if (!entry.simulated) {
      continue;
    }
    if (!entry.body->valid()) {
      Impl::Freeze(entry);
      continue;
    }
    const std::span<const physics::BodyId> bodies = entry.body->bodies();
    float fastest = 0.0f;
    for (size_t i = 0; i < kPartCount; ++i) {
      entry.poses[i] = impl.world.InterpolatedPose(bodies[i]);
      fastest =
          std::max(fastest, glm::length(impl.world.LinearVelocity(bodies[i])));
    }
    impl.Refresh(entry);
    entry.rest_time =
        fastest < tuning.settle_speed ? entry.rest_time + dt : 0.0f;
    if (entry.rest_time >= tuning.settle_seconds ||
        entry.age >= tuning.max_simulated_seconds) {
      Impl::Freeze(entry);
    }
  }
  std::erase_if(impl.entries, [&](const Impl::Entry& entry) {
    return entry.age >= tuning.linger_seconds;
  });
  impl.RebuildViews();
}

bool Ragdolls::Remove(RagdollId id) {
  Impl& impl = *impl_;
  const auto it =
      std::find_if(impl.entries.begin(), impl.entries.end(),
                   [id](const Impl::Entry& entry) { return entry.id == id; });
  if (it == impl.entries.end()) {
    return false;
  }
  impl.entries.erase(it);
  impl.RebuildViews();
  return true;
}

void Ragdolls::Clear() {
  impl_->entries.clear();
  impl_->views.clear();
}

std::span<const RagdollView> Ragdolls::views() const { return impl_->views; }

const RagdollView* Ragdolls::Find(RagdollId id) const {
  for (const RagdollView& view : impl_->views) {
    if (view.id == id) {
      return &view;
    }
  }
  return nullptr;
}

RagdollId Ragdolls::FindByBody(physics::BodyId body) const {
  for (const Impl::Entry& entry : impl_->entries) {
    if (!entry.simulated || !entry.body) {
      continue;
    }
    for (const physics::BodyId candidate : entry.body->bodies()) {
      if (candidate == body) {
        return entry.id;
      }
    }
  }
  return 0;
}

}  // namespace blue::game
