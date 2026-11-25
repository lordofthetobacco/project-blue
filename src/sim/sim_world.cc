#include "sim/sim_world.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "asset/mesh_data.h"
#include "camera/look.h"
#include "core/det_math.h"
#include "core/log.h"
#include "core/path.h"
#include "core/result.h"
#include "interact/interaction_focus.h"
#include "physics/physics_runtime.h"
#include "physics/physics_world.h"
#include "platform/file_system.h"
#include "prefab/prefab.h"
#include "scene/mscn_serializer.h"
#include "scene/scene_data.h"
#include "sim/damage.h"
#include "sim/player_sim.h"
#include "sim/user_command.h"
#include "sim/weapon_table.h"
#include "weapon/dropped_weapons.h"
#include "weapon/loadout.h"
#include "weapon/parts/part_catalog_file.h"
#include "weapon/projectile_system.h"
#include "weapon/weapon_def.h"
#include "weapon/weapon_item.h"
#include "weapon/weapon_throw.h"
#include "world/core/collision_world.h"
#include "world/core/placement.h"
#include "world/core/placement_plan.h"

namespace blue::sim {
namespace {

using VoidResult = core::Result<void, std::string>;

// The part catalogue's directory under the asset root, as the game's
// (game::kCatalogDirectory).
constexpr std::string_view kCatalogDirectory = "weapons/catalog";
// Where a weapon swapped out by a pick-up (or dropped by a dying player)
// leaves from, like the game's toss: in front of the chest.
constexpr float kTossAhead = 0.35f;     // m, horizontally
constexpr float kTossBelowEye = 0.35f;  // m
// A line of sight ending this close before the aimed point still sees it
// (the game's kSightTolerance).
constexpr float kSightTolerance = 0.02f;  // m
constexpr uint64_t kFnv64Prime = 0x100000001b3ull;
// Largest scene file hashed into the content hash.
constexpr size_t kMaxSceneBytes = size_t{256} << 20;

// Returns the prefab space -> world matrix of `pose` scaled by `scale`
// (weapon::DroppedWeapons::Matrix()).
glm::mat4 PoseMatrix(const physics::Pose& pose, glm::vec3 scale) {
  glm::mat4 matrix = glm::mat4_cast(pose.rotation);
  for (int i = 0; i < 3; ++i) {
    matrix[i] *= scale[i];
  }
  matrix[3] = glm::vec4(pose.position, 1.0f);
  return matrix;
}

// Returns the box `bounds` (prefab space) placed by `matrix`, like the
// game's PlacedBox().
interact::OrientedBox PlacedBox(const glm::mat4& matrix,
                                const asset::Bounds& bounds) {
  interact::OrientedBox box;
  box.center = glm::vec3(matrix * glm::vec4(bounds.center(), 1.0f));
  const glm::vec3 half = bounds.size() * 0.5f;
  for (int i = 0; i < 3; ++i) {
    const glm::vec3 column(matrix[i]);
    const float length = glm::length(column);
    box.axes[i] = length > 0.0f ? column / length : glm::vec3(0.0f);
    box.half_extents[i] = half[i] * length;
  }
  return box;
}

// Returns where `def`'s model is held when aimed by an eye at `eye` looking
// along `look`: the sight's eye point `eye_relief` ahead of the eye, the
// sight axis along the view (weapon::Viewmodel's aimed pose without sway,
// bob or recoil). Prefab space -> world.
glm::mat4 AimedHold(const weapon::WeaponDef& def, glm::vec3 eye,
                    camera::LookAngles look) {
  const weapon::WeaponGeometry& geometry = def.geometry;
  const glm::vec3 axis = glm::length(geometry.sight_axis) > 0.0f
                             ? glm::normalize(geometry.sight_axis)
                             : glm::vec3(0.0f, 0.0f, 1.0f);
  const glm::vec3 model_up(0.0f, 1.0f, 0.0f);
  const glm::vec3 up_raw = model_up - glm::dot(model_up, axis) * axis;
  const glm::vec3 up = glm::length(up_raw) > 0.0f ? glm::normalize(up_raw)
                                                  : glm::vec3(1.0f, 0.0f, 0.0f);
  const glm::vec3 right = glm::cross(axis, up);
  const glm::mat3 model_frame(right, up, axis);
  const glm::mat3 camera_frame(glm::vec3(1.0f, 0.0f, 0.0f),
                               glm::vec3(0.0f, 1.0f, 0.0f),
                               glm::vec3(0.0f, 0.0f, -1.0f));
  const glm::mat3 aligned = camera_frame * glm::transpose(model_frame);

  // The view rotation, yaw about +Y then pitch about +X, with deterministic
  // trigonometry (glm::rotate would use the C library's).
  const core::SinCos yaw = core::DetSinCos(look.yaw);
  const core::SinCos pitch = core::DetSinCos(look.pitch);
  const glm::mat3 yaw_rotation(glm::vec3(yaw.cos, 0.0f, -yaw.sin),
                               glm::vec3(0.0f, 1.0f, 0.0f),
                               glm::vec3(yaw.sin, 0.0f, yaw.cos));
  const glm::mat3 pitch_rotation(glm::vec3(1.0f, 0.0f, 0.0f),
                                 glm::vec3(0.0f, pitch.cos, pitch.sin),
                                 glm::vec3(0.0f, -pitch.sin, pitch.cos));
  glm::mat4 matrix = glm::translate(glm::mat4(1.0f), eye);
  matrix = matrix * glm::mat4(yaw_rotation * pitch_rotation);
  matrix = glm::translate(
      matrix, glm::vec3(0.0f, 0.0f, -def.type.viewmodel.eye_relief));
  matrix = matrix * glm::mat4(aligned);
  matrix = glm::scale(matrix, glm::vec3(geometry.scale));
  return glm::translate(matrix, -geometry.sight_eye_point);
}

// Returns the smallest root in [0, 1] of a*t^2 + b*t + c = 0 (a > 0).
std::optional<float> FirstRoot(float a, float b, float c) {
  const float discriminant = b * b - 4.0f * a * c;
  if (!(a > 0.0f) || discriminant < 0.0f) {
    return std::nullopt;
  }
  const float t = (-b - std::sqrt(discriminant)) / (2.0f * a);
  if (t < 0.0f || t > 1.0f) {
    return std::nullopt;
  }
  return t;
}

// Returns the first fraction at which the segment enters the sphere.
std::optional<float> SegmentHitsSphere(glm::vec3 origin, glm::vec3 delta,
                                       glm::vec3 center, float radius) {
  const glm::vec3 m = origin - center;
  return FirstRoot(glm::dot(delta, delta), 2.0f * glm::dot(m, delta),
                   glm::dot(m, m) - radius * radius);
}

void Keep(std::optional<float>& best, std::optional<float> candidate) {
  if (candidate && (!best || *candidate < *best)) {
    best = candidate;
  }
}

// Returns `path` relative to `root`, or nullopt when it is not inside it.
std::optional<std::string> AssetRelative(std::string_view path,
                                         std::string_view root) {
  std::optional<std::string> relative = core::RelativePath(path, root);
  if (!relative || relative->starts_with("..") || *relative == ".") {
    return std::nullopt;
  }
  return relative;
}

}  // namespace

std::vector<StartingWeapon> DefaultStartingWeapons() {
  return {{.path = "glock17.pbo", .fallback_type = "glock17"},
          {.path = "m16.pbo", .fallback_type = "m16"}};
}

std::optional<float> SegmentHitsCapsule(glm::vec3 origin, glm::vec3 delta,
                                        const Capsule& capsule) {
  const float radius = capsule.radius;
  const float bottom = capsule.feet.y + radius;
  const float top = capsule.feet.y + std::max(capsule.height - radius, radius);
  const glm::vec3 low(capsule.feet.x, bottom, capsule.feet.z);
  const glm::vec3 high(capsule.feet.x, top, capsule.feet.z);
  // Inside at the start: the distance to the vertical axis segment.
  const glm::vec3 axis_point(capsule.feet.x, std::clamp(origin.y, bottom, top),
                             capsule.feet.z);
  if (glm::length(origin - axis_point) <= radius) {
    return 0.0f;
  }
  // The capsule is the union of a vertical cylinder and two spheres; the
  // first entry into it is the first entry into any of them.
  std::optional<float> best;
  const float mx = origin.x - capsule.feet.x;
  const float mz = origin.z - capsule.feet.z;
  if (const std::optional<float> t =
          FirstRoot(delta.x * delta.x + delta.z * delta.z,
                    2.0f * (mx * delta.x + mz * delta.z),
                    mx * mx + mz * mz - radius * radius)) {
    const float y = origin.y + *t * delta.y;
    if (y >= bottom && y <= top) {
      best = t;
    }
  }
  Keep(best, SegmentHitsSphere(origin, delta, low, radius));
  Keep(best, SegmentHitsSphere(origin, delta, high, radius));
  return best;
}

core::Result<std::unique_ptr<SimWorld>, std::string> SimWorld::Create(
    const physics::PhysicsRuntime& runtime, const SimWorldOptions& options) {
  using CreateResult = core::Result<std::unique_ptr<SimWorld>, std::string>;
  std::unique_ptr<SimWorld> world(new SimWorld(runtime, options));
  if (VoidResult built = world->Build(options); !built.ok()) {
    return CreateResult::Error(built.error());
  }
  return CreateResult::Ok(std::move(world));
}

SimWorld::SimWorld(const physics::PhysicsRuntime& runtime,
                   const SimWorldOptions& options)
    : physics_(runtime,
               {.worker_threads = std::max(options.worker_threads, 0)}),
      collision_(physics_),
      dropped_(physics_),
      max_players_(std::max(options.max_players, 0)),
      dropped_lifetime_ticks_(std::max(options.dropped_lifetime_ticks, 1u)),
      max_dropped_weapons_(
          static_cast<size_t>(std::max(options.max_dropped_weapons, 1))),
      kill_plane_depth_(std::max(options.kill_plane_depth, 0.0f)) {}

SimWorld::~SimWorld() {
  // Characters and dropped weapons' bodies go before the physics world.
  players_.clear();
  dropped_.Clear();
}

VoidResult SimWorld::Build(const SimWorldOptions& options) {
  if (options.asset_dir.empty() || options.scene_path.empty()) {
    return VoidResult::Error("no asset directory or scene path given");
  }
  asset_root_ = core::NormalizePath(platform::AbsolutePath(options.asset_dir));
  const std::string scene_path =
      core::JoinPath(asset_root_, options.scene_path);
  core::Result<scene::SceneData, scene::MscnError> loaded =
      scene::LoadMscn(scene_path);
  if (!loaded.ok()) {
    return VoidResult::Error(std::format("scene {}: {}", scene_path,
                                         scene::ToString(loaded.error())));
  }
  scene_ = std::move(loaded).value();
  core::Result<std::vector<std::byte>, platform::FileError> scene_bytes =
      platform::ReadFile(scene_path, kMaxSceneBytes);
  if (!scene_bytes.ok()) {
    return VoidResult::Error(std::format(
        "scene {}: {}", scene_path, platform::ToString(scene_bytes.error())));
  }

  const std::string catalog_dir =
      core::JoinPath(asset_root_, kCatalogDirectory);
  if (platform::PathExists(catalog_dir)) {
    auto catalog = weapon::parts::LoadPartCatalog(catalog_dir);
    if (catalog.ok()) {
      catalog_ = std::move(catalog).value();
    } else {
      core::log::Error(
          "No weapon part catalogue ({}); only single-piece weapons are "
          "available",
          catalog.error().ToString());
    }
  }

  core::Result<std::vector<std::string>, std::string> starting =
      LoadStartingWeapons(options);
  if (!starting.ok()) {
    return VoidResult::Error(starting.error());
  }
  std::vector<std::string> weapon_paths = std::move(starting).value();

  const world::PlacementPlan plan = ResolvePlacements(scene_, store_);
  for (const world::WorldIssue& issue : plan.issues) {
    core::log::Warn("Scene instance {}: {}", issue.instance, issue.message);
  }
  const size_t starting_count = weapon_paths.size();
  const std::vector<std::string> pickup_paths = FindPickups(plan.placements);
  for (const std::string& path : pickup_paths) {
    if (std::ranges::find(weapon_paths, path) == weapon_paths.end()) {
      weapon_paths.push_back(path);
    }
  }

  core::Result<WeaponTable, std::string> table =
      BuildWeaponTable(catalog_ ? &*catalog_ : nullptr, weapon_paths, store_,
                       asset_root_, library_);
  if (!table.ok()) {
    return VoidResult::Error(table.error());
  }
  table_ = std::move(table).value();
  for (size_t i = 0; i < weapon_paths.size(); ++i) {
    const std::optional<weapon::WeaponDefId> def =
        table_.Find(KeyOfWeaponName(PrefabWeaponName(weapon_paths[i])));
    if (!def) {
      return VoidResult::Error(std::format(
          "weapon {} is missing from the weapon table", weapon_paths[i]));
    }
    if (i < starting_count) {
      starting_defs_.push_back(*def);
    }
  }
  for (size_t i = 0; i < pickups_.size(); ++i) {
    const WeaponKey key = KeyOfWeaponName(PrefabWeaponName(pickup_paths[i]));
    pickups_[i].key = key;
    pickups_[i].def = *table_.Find(key);
  }
  std::ranges::sort(pickups_, {}, &Pickup::instance);

  kill_plane_y_ = LowestColliderY(plan.placements) - kill_plane_depth_;
  const world::PhysicsBuildSummary summary =
      collision_.Build(scene_, plan.placements, store_);
  if (summary.failed > 0) {
    core::log::Warn("Simulation physics: {} bodies created, {} failed",
                    summary.created, summary.failed);
  }
  for (Pickup& pickup : pickups_) {
    pickup.body = collision_.BodyFor(pickup.instance);
  }
  for (size_t i = 0; i < library_.size(); ++i) {
    const auto def = static_cast<weapon::WeaponDefId>(i);
    (void)dropped_.SetShape(def, library_.def(def).bounds);
  }

  content_hash_ = table_.content_hash();
  for (const std::byte byte : scene_bytes.value()) {
    content_hash_ ^= static_cast<uint64_t>(byte);
    content_hash_ *= kFnv64Prime;
  }
  return VoidResult::Ok();
}

core::Result<std::vector<std::string>, std::string>
SimWorld::LoadStartingWeapons(const SimWorldOptions& options) {
  using PathsResult = core::Result<std::vector<std::string>, std::string>;
  std::vector<std::string> paths;
  for (const StartingWeapon& weapon : options.starting_weapons) {
    const std::string path = core::NormalizePath(weapon.path);
    core::Result<world::PrefabId, prefab::PrefabError> id =
        store_.Get(core::JoinPath(asset_root_, path));
    if (!id.ok()) {
      return PathsResult::Error(std::format("starting weapon {}: {}", path,
                                            prefab::ToString(id.error())));
    }
    const prefab::Prefab& prefab = store_.prefab(id.value());
    if (!weapon.fallback_type.empty() &&
        !prefab::FindTag(prefab, weapon::kWeaponTag)) {
      core::log::Info("{} has no '{}' tag; using '{}' (re-cook the samples)",
                      path, weapon::kWeaponTag, weapon.fallback_type);
      prefab::Prefab tagged = prefab;  // Shares the bulk.
      prefab::SetTag(tagged, weapon::kWeaponTag, weapon.fallback_type);
      const std::string key = store_.key(id.value());
      (void)store_.Add(std::move(tagged), key);
    }
    paths.push_back(path);
  }
  return PathsResult::Ok(std::move(paths));
}

std::vector<std::string> SimWorld::FindPickups(
    std::span<const world::Placement> placements) {
  std::vector<std::string> paths;
  for (const world::Placement& placement : placements) {
    if (placement.parent != -1) {
      continue;  // A prefab nested in a prefab is part of its instance.
    }
    const prefab::Prefab& prefab = store_.prefab(placement.prefab);
    if (!prefab::FindTag(prefab, weapon::kWeaponTag)) {
      continue;
    }
    const scene::Instance* instance =
        scene::FindInstance(scene_, placement.owner);
    if (instance == nullptr) {
      continue;
    }
    if (instance->parent != scene::kNoInstance) {
      core::log::Warn(
          "Weapon instance '{}' is a child of another instance: not a "
          "pick-up",
          instance->name);
      continue;
    }
    const std::optional<std::string> relative =
        AssetRelative(store_.key(placement.prefab), asset_root_);
    if (!relative) {
      core::log::Warn(
          "Weapon instance '{}' uses {} from outside the asset directory: "
          "not a pick-up",
          instance->name, store_.key(placement.prefab));
      continue;
    }
    glm::vec3 scale;
    for (int i = 0; i < 3; ++i) {
      scale[i] = glm::length(glm::vec3(placement.world[i]));
    }
    pickups_.push_back({.instance = instance->id,
                        .placement = placement.world,
                        .scale = scale,
                        .bounds = prefab.bounds});
    paths.push_back(*relative);
  }
  return paths;
}

bool SimWorld::AddPlayer(uint32_t id) {
  if (FindPlayer(id) != nullptr ||
      players_.size() >= static_cast<size_t>(max_players_)) {
    return false;
  }
  const scene::PlayerStart& start = scene_.player_start;
  const camera::LookAngles look{.yaw = start.yaw_radians,
                                .pitch = start.pitch_radians};
  const player::PlayerTuning tuning;
  CollectOccupied(id);
  const glm::vec3 feet = ChooseSpawnPoint(physics_, start, id, 0, occupied_,
                                          tuning.eye_height_standing);
  auto created = PlayerSim::Create(physics_, library_, table_, id, feet, look,
                                   StartingLoadout(), {}, tuning);
  if (!created.ok()) {
    core::log::Error("Player {} cannot be created: {}", id,
                     physics::ToString(created.error()));
    return false;
  }
  Player player{.id = id, .sim = std::move(created).value()};
  player.last.yaw = look.yaw;
  player.last.pitch = look.pitch;
  const auto at = std::ranges::lower_bound(players_, id, {}, &Player::id);
  players_.insert(at, std::move(player));
  return true;
}

bool SimWorld::RemovePlayer(uint32_t id) {
  const auto at = std::ranges::lower_bound(players_, id, {}, &Player::id);
  if (at == players_.end() || at->id != id) {
    return false;
  }
  players_.erase(at);
  return true;
}

bool SimWorld::ApplyCommand(uint32_t id, const UserCommand& command) {
  Player* player = FindPlayer(id);
  if (player == nullptr) {
    return false;
  }
  player->pending = command;
  player->has_pending = true;
  return true;
}

void SimWorld::Step(uint32_t tick) {
  tick_ = tick;
  swept_ = false;
  const scene::PlayerStart& start = scene_.player_start;
  for (Player& player : players_) {
    PlayerSim& sim = *player.sim;
    if (sim.alive() || !sim.CountDownRespawn()) {
      continue;
    }
    CollectOccupied(player.id);
    const glm::vec3 feet =
        ChooseSpawnPoint(physics_, start, player.id, sim.Save().deaths,
                         occupied_, sim.tuning().eye_height_standing);
    sim.Respawn(feet, {.yaw = start.yaw_radians, .pitch = start.pitch_radians},
                StartingLoadout());
    Emit({.type = SimEventType::kRespawn,
          .player = player.id,
          .yaw = start.yaw_radians,
          .position = feet});
  }

  shots_.clear();
  for (Player& player : players_) {
    UserCommand command = player.last;
    if (player.has_pending) {
      command = player.pending;
    } else {
      // Held buttons and angles repeat; edges and requests do not.
      command.pressed = 0;
      command.select_slot = -1;
      command.interact_kind = interact_kind::kNone;
      command.interact_id = 0;
    }
    command.tick = tick;
    player.has_pending = false;
    player.last = command;
    const StepEvents events = player.sim->Step(command);
    for (int i = 0; i < events.shot_count; ++i) {
      const ShotEvent& shot = events.shots[static_cast<size_t>(i)];
      shots_.push_back({.owner = player.id, .shot = shot});
      Emit({.type = SimEventType::kShot,
            .player = player.id,
            .id = shot.shot_number,
            .weapon_key = shot.weapon_key,
            .position = shot.origin,
            .vector = shot.velocity});
    }
  }
  for (Player& player : players_) {
    if (player.sim->alive()) {
      Interact(player, player.last);
    }
  }

  physics_.StepOnce();
  ApplyKillPlane();
  ExpireDroppedWeapons();

  for (const PendingShot& pending : shots_) {
    const ShotEvent& shot = pending.shot;
    projectiles_.Spawn(shot.origin, shot.velocity, shot.mass, shot.drag,
                       shot.age, static_cast<int>(shot.shot_number),
                       pending.owner);
    shot_weapons_.push_back({.owner = pending.owner,
                             .shot_number = static_cast<int>(shot.shot_number),
                             .weapon_key = shot.weapon_key});
  }
  PruneShotWeapons();
}

std::span<const weapon::Segment> SimWorld::SweepProjectiles() {
  if (!swept_) {
    projectiles_.Sweep(physics_, scene_.gravity, kTickSeconds);
    swept_ = true;
  }
  return projectiles_.segments();
}

void SimWorld::ResolveProjectiles(std::span<const ShotHit> hits) {
  const std::span<const weapon::Segment> segments = SweepProjectiles();
  player_hits_.clear();
  hit_heights_.clear();
  for (const ShotHit& hit : hits) {
    if (hit.segment < 0 ||
        static_cast<size_t>(hit.segment) >= segments.size()) {
      continue;
    }
    const weapon::Segment& segment = segments[static_cast<size_t>(hit.segment)];
    player_hits_.push_back({.segment = hit.segment,
                            .victim = hit.victim,
                            .fraction = hit.fraction});
    hit_heights_.push_back({.owner = segment.owner,
                            .shot_number = segment.shot_number,
                            .victim = hit.victim,
                            .height_fraction = hit.height_fraction});
  }
  projectiles_.Resolve(physics_, player_hits_);
  swept_ = false;

  for (const weapon::Impact& impact : projectiles_.impacts()) {
    Emit(
        {.type = SimEventType::kImpact,
         .id = collision_.InstanceFor(impact.body).value_or(scene::kNoInstance),
         .value = impact.shot_number,
         .position = impact.point,
         .vector = impact.normal});
  }
  for (const weapon::PlayerImpact& impact : projectiles_.player_impacts()) {
    ApplyPlayerImpact(impact);
  }
}

void SimWorld::HitCapsulesNow(std::vector<ShotHit>& out) const {
  out.clear();
  if (!swept_) {
    return;
  }
  const std::span<const weapon::Segment> segments = projectiles_.segments();
  for (size_t i = 0; i < segments.size(); ++i) {
    const weapon::Segment& segment = segments[i];
    for (const Player& player : players_) {
      if (player.id == segment.owner || !player.sim->alive()) {
        continue;
      }
      const Capsule capsule = *CurrentCapsule(player.id);
      const std::optional<float> fraction =
          SegmentHitsCapsule(segment.origin, segment.delta, capsule);
      if (!fraction) {
        continue;
      }
      const float y = segment.origin.y + *fraction * segment.delta.y;
      out.push_back({.segment = static_cast<int>(i),
                     .victim = player.id,
                     .fraction = *fraction,
                     .height_fraction = std::clamp(
                         (y - capsule.feet.y) / capsule.height, 0.0f, 1.0f)});
    }
  }
}

std::optional<Capsule> SimWorld::CurrentCapsule(uint32_t id) const {
  const Player* player = FindPlayer(id);
  if (player == nullptr) {
    return std::nullopt;
  }
  return Capsule{.feet = player->sim->feet(),
                 .height = player->sim->capsule_height(),
                 .radius = player->sim->capsule_radius()};
}

std::optional<SimRayHit> SimWorld::CastRay(glm::vec3 origin,
                                           glm::vec3 direction,
                                           float max_distance) const {
  const std::optional<physics::RayHit> hit =
      physics_.CastRay(origin, direction, max_distance);
  if (!hit) {
    return std::nullopt;
  }
  const weapon::DroppedWeapon* dropped = dropped_.FindByBody(hit->body);
  return SimRayHit{.point = hit->point,
                   .normal = hit->normal,
                   .fraction = hit->fraction,
                   .instance = collision_.InstanceFor(hit->body).value_or(
                       scene::kNoInstance),
                   .dropped_id = dropped != nullptr ? dropped->id : 0};
}

scene::InstanceId SimWorld::InstanceOf(physics::BodyId body) const {
  return collision_.InstanceFor(body).value_or(scene::kNoInstance);
}

size_t SimWorld::ReadPlayers(std::span<PlayerReadout> out) const {
  const size_t count = std::min(out.size(), players_.size());
  for (size_t i = 0; i < count; ++i) {
    out[i] = {.id = players_[i].id, .state = players_[i].sim->Save()};
  }
  return players_.size();
}

size_t SimWorld::ReadBodies(std::span<BodyReadout> out) const {
  size_t total = 0;
  for (const world::CollisionBody& body : collision_.bodies()) {
    if (!body.root || !body.simulated || body.removed) {
      continue;
    }
    if (total < out.size()) {
      out[total] = {.instance = body.instance,
                    .state = physics_.GetBodyState(body.body)};
    }
    ++total;
  }
  return total;
}

size_t SimWorld::ReadDroppedWeapons(std::span<DroppedReadout> out) const {
  const std::span<const weapon::DroppedWeapon> items = dropped_.items();
  const size_t count = std::min(out.size(), items.size());
  for (size_t i = 0; i < count; ++i) {
    const weapon::DroppedWeapon& item = items[i];
    DroppedReadout& readout = out[i];
    readout = {.id = item.id,
               .weapon_key = table_.KeyOf(item.weapon.def).value_or(0),
               .pose = item.pose,
               .scale = item.scale,
               .magazine = item.weapon.firearm.rounds_in_magazine(),
               .chambered = item.weapon.firearm.round_chambered()};
    if (item.body) {
      const physics::BodyState state = physics_.GetBodyState(*item.body);
      readout.pose = state.pose;
      readout.linear_velocity = state.linear_velocity;
      readout.angular_velocity = state.angular_velocity;
      readout.active = state.active;
    }
  }
  return items.size();
}

size_t SimWorld::ReadPickups(std::span<PickupReadout> out) const {
  const size_t count = std::min(out.size(), pickups_.size());
  for (size_t i = 0; i < count; ++i) {
    const Pickup& pickup = pickups_[i];
    out[i] = {.instance = pickup.instance,
              .weapon_key = pickup.key,
              .pose = PickupPose(pickup),
              .taken = pickup.taken};
  }
  return pickups_.size();
}

size_t SimWorld::PollEvents(std::span<SimEvent> out) {
  const size_t count = std::min(out.size(), pending_events());
  std::copy_n(events_.begin() + static_cast<std::ptrdiff_t>(events_read_),
              count, out.begin());
  events_read_ += count;
  if (events_read_ == events_.size()) {
    events_.clear();  // Keeps the capacity.
    events_read_ = 0;
  }
  return count;
}

const PlayerSim* SimWorld::player(uint32_t id) const {
  const Player* found = FindPlayer(id);
  return found != nullptr ? found->sim.get() : nullptr;
}

SimWorld::Player* SimWorld::FindPlayer(uint32_t id) {
  const auto at = std::ranges::lower_bound(players_, id, {}, &Player::id);
  return at != players_.end() && at->id == id ? &*at : nullptr;
}

const SimWorld::Player* SimWorld::FindPlayer(uint32_t id) const {
  const auto at = std::ranges::lower_bound(players_, id, {}, &Player::id);
  return at != players_.end() && at->id == id ? &*at : nullptr;
}

weapon::Loadout SimWorld::StartingLoadout() const {
  std::vector<weapon::WeaponItem> weapons;
  weapons.reserve(starting_defs_.size());
  for (const weapon::WeaponDefId def : starting_defs_) {
    weapons.push_back(weapon::MakeFreshWeapon(def, library_.def(def)));
  }
  return weapon::Loadout(weapons);
}

void SimWorld::CollectOccupied(uint32_t except) {
  occupied_.clear();
  for (const Player& player : players_) {
    if (player.id != except && player.sim->alive()) {
      occupied_.push_back(player.sim->feet());
    }
  }
}

void SimWorld::Interact(Player& player, const UserCommand& command) {
  PlayerSim& sim = *player.sim;
  if ((command.pressed & buttons::pressed::kDropWeapon) != 0) {
    if (std::optional<weapon::WeaponItem> weapon = sim.DropActiveWeapon()) {
      ThrowWeapon(sim, std::move(*weapon), false);
    }
  }
  if ((command.pressed & buttons::pressed::kInteract) == 0) {
    return;
  }
  switch (command.interact_kind) {
    case interact_kind::kDroppedWeapon: {
      const weapon::DroppedWeapon* item = dropped_.Find(command.interact_id);
      if (item == nullptr) {
        return;
      }
      const physics::Pose pose =
          item->body ? physics_.GetBodyState(*item->body).pose : item->pose;
      if (!CanReach(sim, PoseMatrix(pose, item->scale),
                    dropped_.bounds(item->weapon.def), item->body)) {
        return;
      }
      std::optional<weapon::WeaponItem> weapon =
          dropped_.Take(command.interact_id);
      std::erase_if(dropped_ages_, [&command](const DroppedAge& age) {
        return age.id == command.interact_id;
      });
      Emit({.type = SimEventType::kDroppedTaken,
            .player = player.id,
            .id = command.interact_id,
            .weapon_key = table_.KeyOf(weapon->def).value_or(0)});
      TakeWeapon(player, std::move(*weapon));
      return;
    }
    case interact_kind::kSceneWeapon: {
      const auto pickup = std::ranges::lower_bound(
          pickups_, command.interact_id, {}, &Pickup::instance);
      if (pickup == pickups_.end() || pickup->instance != command.interact_id ||
          pickup->taken ||
          !CanReach(sim, PickupMatrix(*pickup), pickup->bounds, pickup->body)) {
        return;
      }
      pickup->taken = true;
      (void)collision_.RemoveInstanceBodies(pickup->instance);
      Emit({.type = SimEventType::kPickupTaken,
            .player = player.id,
            .id = pickup->instance,
            .weapon_key = pickup->key});
      TakeWeapon(player, weapon::MakeFreshWeapon(pickup->def,
                                                 library_.def(pickup->def)));
      return;
    }
    default:
      return;  // kNone or a kind this version does not know.
  }
}

void SimWorld::TakeWeapon(Player& player, weapon::WeaponItem weapon) {
  weapon::PickUpResult result = player.sim->PickUp(std::move(weapon));
  if (!result.replaced) {
    return;
  }
  ThrowWeapon(*player.sim, std::move(*result.replaced), true);
}

void SimWorld::ThrowWeapon(const PlayerSim& player, weapon::WeaponItem weapon,
                           bool toss) {
  const weapon::WeaponDef& def = library_.def(weapon.def);
  const glm::vec3 eye = player.eye();
  const glm::vec3 forward = camera::ForwardFrom(player.look());
  glm::mat4 held = AimedHold(def, eye, player.look());
  if (toss) {
    glm::vec3 ahead = camera::FlatForwardFrom(player.look().yaw);
    held[3] = glm::vec4(
        eye + ahead * kTossAhead - camera::kWorldUp * kTossBelowEye, 1.0f);
  }
  const weapon::ThrowInput input{.held = held,
                                 .bounds = dropped_.bounds(weapon.def),
                                 .eye = eye,
                                 .forward = forward,
                                 .player_velocity = player.velocity()};
  const weapon::ThrowProbe probe = weapon::MakeThrowProbe(input);
  std::optional<float> blocked_at;
  if (probe.length > 0.0f) {
    if (const std::optional<physics::RayHit> hit =
            physics_.CastRay(probe.origin, probe.direction, probe.length)) {
      blocked_at = hit->fraction * probe.length;
    }
  }
  const weapon::ThrowTuning tuning =
      toss ? weapon::SwapTossTuning() : weapon::ThrowTuning{};
  SimEvent event{.type = SimEventType::kWeaponDropped,
                 .chambered = weapon.firearm.round_chambered(),
                 .player = player.id(),
                 .weapon_key = table_.KeyOf(weapon.def).value_or(0),
                 .value = weapon.firearm.rounds_in_magazine()};
  // The cap: the oldest weapons make room, before the new one is added.
  while (!dropped_ages_.empty() &&
         dropped_ages_.size() >= max_dropped_weapons_) {
    RemoveDropped(dropped_ages_.front().id);
  }
  event.id = dropped_.Add(std::move(weapon),
                          weapon::PlanThrow(input, tuning, blocked_at));
  dropped_ages_.push_back({.id = event.id, .tick = tick_});
  Emit(event);
}

void SimWorld::RemoveDropped(uint32_t id) {
  const weapon::DroppedWeapon* item = dropped_.Find(id);
  if (item != nullptr) {
    const WeaponKey key = table_.KeyOf(item->weapon.def).value_or(0);
    (void)dropped_.Remove(id);
    Emit({.type = SimEventType::kDroppedRemoved, .id = id, .weapon_key = key});
  }
  std::erase_if(dropped_ages_,
                [id](const DroppedAge& age) { return age.id == id; });
}

void SimWorld::ExpireDroppedWeapons() {
  // Oldest first: the front is due before anything behind it.
  while (!dropped_ages_.empty() &&
         tick_ - dropped_ages_.front().tick >= dropped_lifetime_ticks_) {
    RemoveDropped(dropped_ages_.front().id);
  }
}

void SimWorld::ApplyKillPlane() {
  for (Player& player : players_) {
    if (player.sim->alive() && player.sim->feet().y < kill_plane_y_) {
      // Fell out of the world: nothing to drop where it fell.
      KillPlayer(player, 0, 0, /*drop_weapon=*/false);
    }
  }
  // In drop order, so removals are deterministic.
  for (size_t i = 0; i < dropped_ages_.size();) {
    const weapon::DroppedWeapon* item = dropped_.Find(dropped_ages_[i].id);
    const physics::Pose pose = item == nullptr ? physics::Pose{}
                               : item->body
                                   ? physics_.GetBodyState(*item->body).pose
                                   : item->pose;
    if (item != nullptr && pose.position.y < kill_plane_y_) {
      RemoveDropped(dropped_ages_[i].id);  // Erases entry i.
    } else {
      ++i;
    }
  }
  for (const world::CollisionBody& body : collision_.bodies()) {
    if (body.removed || !body.simulated || !physics_.IsActive(body.body)) {
      continue;
    }
    physics::BodyState state = physics_.GetBodyState(body.body);
    if (state.pose.position.y < kill_plane_y_) {
      state.active = false;  // Sleeping zeroes its velocities.
      physics_.SetBodyState(body.body, state);
    }
  }
}

float SimWorld::LowestColliderY(
    std::span<const world::Placement> placements) const {
  float lowest = std::numeric_limits<float>::max();
  for (const world::Placement& placement : placements) {
    const prefab::Prefab& prefab = store_.prefab(placement.prefab);
    if (prefab.physics.kind == prefab::ColliderKind::kNone) {
      continue;
    }
    const asset::Bounds& bounds = prefab.bounds;
    for (int corner = 0; corner < 8; ++corner) {
      const glm::vec3 local((corner & 1) != 0 ? bounds.max.x : bounds.min.x,
                            (corner & 2) != 0 ? bounds.max.y : bounds.min.y,
                            (corner & 4) != 0 ? bounds.max.z : bounds.min.z);
      lowest = std::min(lowest, (placement.world * glm::vec4(local, 1.0f)).y);
    }
  }
  // A scene without colliders: the plane sits below the origin.
  return lowest == std::numeric_limits<float>::max() ? 0.0f : lowest;
}

bool SimWorld::CanReach(const PlayerSim& player, const glm::mat4& matrix,
                        const asset::Bounds& bounds,
                        std::optional<physics::BodyId> own_body) const {
  // The focused item's limits, release widening included: the client
  // focused it a moment ago, the server need not be stricter.
  const interact::FocusTuning tuning;
  const glm::vec3 eye = player.eye();
  const interact::AimMeasure aim = interact::MeasureAim(
      eye, camera::ForwardFrom(player.look()), PlacedBox(matrix, bounds));
  if (aim.reach_distance > tuning.reach + tuning.release_reach ||
      aim.miss_distance > tuning.aim_radius * tuning.release_aim_scale ||
      aim.miss_angle > tuning.max_aim_angle * tuning.release_aim_scale) {
    return false;
  }
  const glm::vec3 to_point = aim.aim_point - eye;
  const float distance = glm::length(to_point);
  if (!(distance > kSightTolerance)) {
    return true;  // The eye is at (or in) the item.
  }
  const std::optional<physics::RayHit> hit =
      physics_.CastRay(eye, to_point, distance);
  if (!hit || hit->fraction * distance >= distance - kSightTolerance) {
    return true;
  }
  return own_body && hit->body == *own_body;  // Its own body does not hide it.
}

glm::mat4 SimWorld::PickupMatrix(const Pickup& pickup) const {
  if (pickup.body && physics_.IsDynamic(*pickup.body)) {
    return PoseMatrix(physics_.GetBodyState(*pickup.body).pose, pickup.scale);
  }
  return pickup.placement;
}

physics::Pose SimWorld::PickupPose(const Pickup& pickup) const {
  if (pickup.body && physics_.IsDynamic(*pickup.body)) {
    return physics_.GetBodyState(*pickup.body).pose;
  }
  glm::mat3 rotation(pickup.placement);
  for (int i = 0; i < 3; ++i) {
    if (pickup.scale[i] > 0.0f) {
      rotation[i] /= pickup.scale[i];
    }
  }
  return {.position = glm::vec3(pickup.placement[3]),
          .rotation = glm::quat_cast(rotation)};
}

void SimWorld::ApplyPlayerImpact(const weapon::PlayerImpact& impact) {
  Player* victim = FindPlayer(impact.victim);
  if (victim == nullptr || !victim->sim->alive() ||
      impact.victim == impact.owner) {
    return;
  }
  PlayerSim& sim = *victim->sim;
  float height_fraction = -1.0f;
  for (const HitHeight& height : hit_heights_) {
    if (height.owner == impact.owner &&
        height.shot_number == impact.shot_number &&
        height.victim == impact.victim) {
      height_fraction = height.height_fraction;
      break;
    }
  }
  if (height_fraction < 0.0f) {
    const float height = sim.capsule_height();
    height_fraction =
        height > 0.0f ? (impact.point.y - sim.feet().y) / height : 0.5f;
  }
  const HitZone zone = ZoneOfHeight(height_fraction);
  const int damage =
      ComputeDamage(impact.mass, impact.velocity, sim.velocity(), zone);
  const int health = std::max(sim.health() - damage, 0);
  sim.SetHealth(health);
  const WeaponKey weapon_key = ShotWeaponKey(impact.owner, impact.shot_number);
  Emit({.type = SimEventType::kDamage,
        .zone = zone,
        .player = impact.victim,
        .other = impact.owner,
        .id = static_cast<uint32_t>(impact.shot_number),
        .weapon_key = weapon_key,
        .value = damage,
        .health = health,
        .position = impact.point,
        .vector = impact.velocity});
  if (health == 0) {
    KillPlayer(*victim, impact.owner, weapon_key);
  }
}

WeaponKey SimWorld::ShotWeaponKey(uint32_t owner, int shot_number) const {
  for (const ShotWeapon& shot : shot_weapons_) {
    if (shot.owner == owner && shot.shot_number == shot_number) {
      return shot.weapon_key;
    }
  }
  return 0;
}

void SimWorld::PruneShotWeapons() {
  // Bounded by the bullet capacity on both sides; a few hundred entries.
  const std::span<const weapon::Projectile> flying = projectiles_.projectiles();
  std::erase_if(shot_weapons_, [flying](const ShotWeapon& shot) {
    return std::ranges::none_of(flying, [&shot](const weapon::Projectile& p) {
      return p.owner == shot.owner && p.shot_number == shot.shot_number;
    });
  });
}

void SimWorld::KillPlayer(Player& victim, uint32_t attacker,
                          WeaponKey weapon_key, bool drop_weapon) {
  PlayerSim& sim = *victim.sim;
  if (drop_weapon) {
    if (std::optional<weapon::WeaponItem> weapon = sim.DropActiveWeapon()) {
      ThrowWeapon(sim, std::move(*weapon), true);
    }
  }
  sim.Kill(kRespawnTicks);
  Emit({.type = SimEventType::kDeath,
        .player = victim.id,
        .other = attacker,
        .weapon_key = attacker != 0 ? weapon_key : WeaponKey{0},
        .position = sim.feet()});
}

void SimWorld::Emit(const SimEvent& event) {
  events_.push_back(event);
  events_.back().tick = tick_;
}

}  // namespace blue::sim
