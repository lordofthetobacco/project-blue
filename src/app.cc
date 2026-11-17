#include "app.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "animation/clip.h"
#include "camera/look.h"
#include "camera/projection.h"
#include "core/log.h"
#include "core/path.h"
#include "input/action.h"
#include "input/bindings.h"
#include "platform/file_system.h"
#include "scene/placement.h"
#include "scene/scene_serializer.h"
#include "shader/shader_compiler.h"
#include "weapon/firearm.h"
#include "weapon/loadout.h"
#include "weapon/projectile_system.h"
#include "weapon/target_log.h"
#include "weapon/viewmodel.h"
#include "weapon/weapon_geometry.h"
#include "weapon/weapon_specs.h"

namespace blue {

namespace {

constexpr int kInitialWidth = 1920;
constexpr int kInitialHeight = 1080;
// The first-person rifle comes close to the eye (the stock passes beside the
// cheek when aimed). The projection is reversed-Z and infinite on a float
// depth buffer, so a close near plane costs no depth precision worth noticing.
constexpr float kNearPlane = 0.02f;
// Avoids jumps after stalls (window drag, breakpoints).
constexpr float kMaxSimulationStep = 0.1f;
constexpr float kSensitivityStep = 1.15f;

constexpr float kImportTargetSize = 2.0f;
constexpr float kImportSpacing = 2.5f;
constexpr float kImportDropHeight = 1.5f;  // Imports start this high and fall.
constexpr float kImportMass = 10.0f;       // kg

constexpr float kPushReach = 100.0f;   // m
constexpr float kPushImpulse = 40.0f;  // N*s, about 4 m/s on a 10 kg body.
// How far below the scene camera the floor is looked for when placing the
// player; beyond that the feet go an eye height below the camera.
constexpr float kSpawnProbeDistance = 3.0f;  // m

// The default scene's ground: the builtin plane (12 m square) stretched into
// a 48 m wide strip that runs from behind the spawn to 700 m down range
// (-Z), so bullet drop can be watched all the way to the farthest target.
constexpr float kBuiltinPlaneSize = 12.0f;  // m, see render::MeshCache
constexpr float kGroundWidth = 48.0f;       // m along X
constexpr float kGroundBackZ = 24.0f;       // m, edge behind the spawn
constexpr float kGroundFrontZ = -700.0f;    // m, far edge down range
// The default player position and view: at the firing line, looking level
// down range. The house is to the left, the cube just ahead on the left.
constexpr glm::vec3 kFiringPosition{2.2f, 1.6f, 2.8f};
constexpr glm::vec3 kDownRange{0.0f, 0.0f, -1.0f};

// Flat steel plates down range, measured from kFiringPosition: 0.5 m squares
// up close, 0.5 x 1.0 m torso plates farther out. Each sits a little to one
// side so that none hides another from the firing line.
struct RangeTarget {
  float range;     // m down range
  float offset;    // m to the right of the firing line
  glm::vec2 size;  // m, width and height
};
constexpr RangeTarget kRangeTargets[] = {
    {25.0f, -1.5f, {0.5f, 0.5f}},  {50.0f, 1.5f, {0.5f, 0.5f}},
    {100.0f, -2.5f, {0.5f, 1.0f}}, {200.0f, 2.0f, {0.5f, 1.0f}},
    {300.0f, -5.0f, {0.5f, 1.0f}}, {400.0f, 6.0f, {0.5f, 1.0f}},
    {600.0f, 0.0f, {0.5f, 1.0f}},
};
constexpr float kTargetHeight = 1.5f;      // m, centre above the ground
constexpr float kTargetThickness = 0.05f;  // m
// Entities whose name starts with this are targets: the hit log finds them by
// name, so they survive a save and reload of the scene.
constexpr std::string_view kTargetNamePrefix = "target ";
// The sample house (6.4 x 7.1 x 14.8 m, authored in metres, base at y = 0),
// placed to the left of the default camera's view and clear of the cube.
constexpr const char* kHouseModel = "house.obj";
constexpr glm::vec3 kHousePosition{-9.0f, 0.0f, -3.0f};

constexpr const char* kRifleModel = "m16.obj";
constexpr const char* kPistolModel = "glock/glock17.glb";
// The pistol's authored firing animation is split into one clip per node
// (trigger, slide, barrel, whole gun); merged, they are one shot.
constexpr std::string_view kPistolFireClipName = "fire";
// Nodes of the pistol model that are not drawn: "Glock_parts_ORIGINAL_combined"
// is a static copy of the Barrel, Frame_Parts, Guide_Rod, Slide and Trigger
// meshes merged into one, which would hide the moving slide and trigger and
// z-fight with them.
constexpr std::string_view kPistolHiddenNodes[] = {
    "Glock_parts_ORIGINAL_combined"};
// Fully aimed vertical FOV as a fraction of the base one, per weapon: the
// pistol's sights are held at arm's length and magnify less than the rifle's
// cheek weld (the rifle keeps player::PlayerTuning's default).
constexpr float kPistolAimFovScale = 0.9f;
constexpr const char* kBulletMesh = "builtin:cube";
// A bullet is drawn as a thin box trailing it by the distance it covers in
// this time (about 4 m at the muzzle), never reaching back past the muzzle.
constexpr float kTracerSeconds = 0.004f;
constexpr float kTracerThickness = 0.02f;       // m
constexpr float kImpactMarkSize = 0.05f;        // m, side of the square mark
constexpr float kImpactMarkDepth = 0.004f;      // m, how far it stands out
constexpr float kLensLiftTowardsEye = 0.0005f;  // m, see CurrentSight()

constexpr const char* kDefaultScenePath = "scenes/untitled.bscn";
constexpr const char* kSettingsFileName = "input.cfg";

#ifdef NDEBUG
constexpr shader::CompileMode kShaderCompileMode =
    shader::CompileMode::kRelease;
#else
constexpr shader::CompileMode kShaderCompileMode = shader::CompileMode::kDebug;
#endif

// How often the shader sources are checked for edits (wall time).
constexpr std::chrono::milliseconds kShaderPollInterval{250};

std::string ShaderDirectory() {
#ifdef BLUE_DEV_SHADER_DIR
  // Debug builds use the repository sources so that edits apply live.
  if (platform::PathExists(BLUE_DEV_SHADER_DIR)) {
    return BLUE_DEV_SHADER_DIR;
  }
#endif
  // Shaders are deployed next to the executable, so lookups do not depend on
  // the working directory.
  return core::JoinPath(platform::BasePath(), "shaders");
}

// Returns the path of the sample model `file_name`: the repository copy in
// Debug builds, otherwise the copy deployed next to the executable.
[[nodiscard]] std::string SamplePath(std::string_view file_name) {
#ifdef BLUE_DEV_SAMPLES_DIR
  const std::string dev_path = core::JoinPath(BLUE_DEV_SAMPLES_DIR, file_name);
  if (platform::PathExists(dev_path)) {
    return dev_path;
  }
#endif
  return core::JoinPath(core::JoinPath(platform::BasePath(), "samples"),
                        file_name);
}

[[nodiscard]] std::string InputSettingsPath() {
  const std::string pref_path =
      platform::PrefPath("ProjectBlue", "ProjectBlue");
  if (pref_path.empty()) {
    core::log::Warn(
        "SDL_GetPrefPath failed, keeping input settings in the working "
        "directory: {}",
        SDL_GetError());
    return kSettingsFileName;
  }
  return core::JoinPath(pref_path, kSettingsFileName);
}

[[nodiscard]] std::string_view ModeLabel(ViewMode mode) {
  switch (mode) {
    case ViewMode::kPlayer:
      return "Player";
    case ViewMode::kFly:
      return "Fly";
    case ViewMode::kOrbit:
      return "Orbit";
  }
  return "?";
}

[[nodiscard]] std::string_view CameraHint(ViewMode mode, bool mouse_captured) {
  if (mode == ViewMode::kOrbit) {
    return "LMB rotate, RMB/MMB pan, wheel zoom";
  }
  return mouse_captured ? "Esc frees the cursor" : "Click to look";
}

[[nodiscard]] camera::CameraMode CameraModeFor(ViewMode mode) {
  return mode == ViewMode::kOrbit ? camera::CameraMode::kOrbit
                                  : camera::CameraMode::kFly;
}

// Returns "<Key> <label>" for `action`, or "" when it is unbound.
[[nodiscard]] std::string HelpEntry(const input::InputBindings& bindings,
                                    input::Action action,
                                    std::string_view label) {
  const std::span<const input::Binding> list = bindings.For(action);
  if (list.empty()) {
    return "";
  }
  return input::FormatBinding(list.front()) + " " + std::string(label);
}

// Builds the overlay's key help line from the live bindings.
[[nodiscard]] std::string BuildHelpText(const input::InputBindings& bindings) {
  using input::Action;
  const std::pair<Action, std::string_view> entries[] = {
      {Action::kMoveForward, "move"},
      {Action::kJump, "jump"},
      {Action::kSprint, "sprint"},
      {Action::kCrouch, "crouch"},
      {Action::kFire, "fire"},
      {Action::kAim, "aim"},
      {Action::kReload, "reload"},
      {Action::kToggleFireMode, "fire mode"},
      {Action::kEquipPistol, "pistol"},
      {Action::kEquipRifle, "rifle"},
      {Action::kCycleCamera, "camera"},
      {Action::kPush, "push"},
      {Action::kToggleCascadeDebug, "cascades"},
      {Action::kToggleShadows, "shadows"},
      {Action::kToggleInvertY, "invert Y"},
      {Action::kSensitivityDown, "/"},
      {Action::kSensitivityUp, "sens"},
      {Action::kToggleOverlay, "overlay"},
      {Action::kSaveScene, "save"},
      {Action::kLoadScene, "load"},
      {Action::kToggleVsync, "vsync"},
      {Action::kEscape, "release/quit"},
  };
  std::string text;
  for (const auto& [action, label] : entries) {
    const std::string entry = HelpEntry(bindings, action, label);
    if (entry.empty()) {
      continue;
    }
    if (!text.empty()) {
      text += "  ";
    }
    text += entry;
  }
  return text;
}

[[nodiscard]] std::string PlayerStateText(
    const player::PlayerController& player) {
  if (!player.spawned()) {
    return "no character";
  }
  std::string text = player.on_ground() ? "grounded" : "airborne";
  if (player.crouching()) {
    text += ", crouching";
  }
  if (player.sprinting()) {
    text += ", sprinting";
  }
  if (player.aiming()) {
    text += ", aiming";
  }
  return text;
}

[[nodiscard]] weapon::Loadout MakeLoadout() {
  return weapon::Loadout(weapon::Glock17Spec(), weapon::M16Spec());
}

// Indexed by weapon::WeaponSlot.
static_assert(static_cast<int>(weapon::WeaponSlot::kPistol) == 0 &&
              static_cast<int>(weapon::WeaponSlot::kRifle) == 1);
[[nodiscard]] std::array<weapon::Viewmodel, weapon::kWeaponSlotCount>
MakeViewmodels() {
  return {
      weapon::Viewmodel(weapon::Glock17Geometry(),
                        weapon::Glock17ViewmodelTuning()),
      weapon::Viewmodel(weapon::M16Geometry(), weapon::M16ViewmodelTuning()),
  };
}

// Returns the slot whose key was pressed this frame; the rifle wins if both
// were.
[[nodiscard]] std::optional<weapon::WeaponSlot> SelectedSlot(
    const input::InputFrame& input) {
  if (input.actions.pressed(input::Action::kEquipRifle)) {
    return weapon::WeaponSlot::kRifle;
  }
  if (input.actions.pressed(input::Action::kEquipPistol)) {
    return weapon::WeaponSlot::kPistol;
  }
  return std::nullopt;
}

// Returns the transform that turns the builtin unit cube into a box centred
// at `center` with its local z along the unit `axis` and `size` metres along
// (x, y, z). The box's roll about `axis` is arbitrary.
[[nodiscard]] glm::mat4 BoxAlong(glm::vec3 center, glm::vec3 axis,
                                 glm::vec3 size) {
  const glm::vec3 helper = std::abs(axis.y) < 0.99f
                               ? glm::vec3(0.0f, 1.0f, 0.0f)
                               : glm::vec3(1.0f, 0.0f, 0.0f);
  const glm::vec3 right = glm::normalize(glm::cross(helper, axis));
  const glm::vec3 up = glm::cross(axis, right);
  return {glm::vec4(right * size.x, 0.0f), glm::vec4(up * size.y, 0.0f),
          glm::vec4(axis * size.z, 0.0f), glm::vec4(center, 1.0f)};
}

[[nodiscard]] std::string WindowTitle(const std::string& scene_path) {
  return "Project Blue \xE2\x80\x94 " + core::FileName(scene_path);
}

// Returns "<verb> <path> (<elapsed> ms)" with the time since `start`.
[[nodiscard]] std::string TimedStatus(
    std::string_view verb, const std::string& path,
    std::chrono::steady_clock::time_point start) {
  const double milliseconds = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - start)
                                  .count();
  return std::format("{} {} ({:.1f} ms)", verb, path, milliseconds);
}

}  // namespace

std::optional<CliOptions> ParseCommandLine(int argc, const char* const argv[]) {
  CliOptions options;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--help") == 0 ||
        std::strcmp(argv[i], "-h") == 0) {
      options.show_help = true;
      return options;
    }
    const bool is_scene = std::strcmp(argv[i], "--scene") == 0;
    const bool is_import = std::strcmp(argv[i], "--import") == 0;
    if ((!is_scene && !is_import) || i + 1 >= argc) {
      return std::nullopt;
    }
    const std::string value = argv[++i];
    if (is_scene) {
      options.scene_path = value;
    } else {
      options.imports.push_back(value);
    }
  }
  return options;
}

App::App(const CliOptions& options)
    : window_("Project Blue", kInitialWidth, kInitialHeight),
      renderer_(window_, ShaderDirectory(), kShaderCompileMode),
      mesh_cache_(renderer_),
      model_cache_(renderer_),
      physics_world_(physics_runtime_),
      scene_physics_(physics_world_),
      overlay_(window_, renderer_),
      scene_path_(options.scene_path.value_or(kDefaultScenePath)),
      settings_path_(InputSettingsPath()),
      loadout_(MakeLoadout()),
      viewmodels_(MakeViewmodels()),
      rifle_mesh_path_(SamplePath(kRifleModel)) {
  LoadPistolModel();
  settings_ = input::LoadInputSettings(settings_path_);
  input_.set_bindings(settings_.bindings);
  help_text_ = BuildHelpText(settings_.bindings);
  SDL_SetWindowTitle(window_.handle(), WindowTitle(scene_path_).c_str());
  InitializeScene(options);
  camera_.SetFromSceneCamera(scene_.camera);
  RebuildPhysics();
  FindTargets();
  PlacePlayerAtSceneCamera();
  ApplyWeaponToPlayer();
}

void App::LoadPistolModel() {
  pistol_model_ = model_cache_.Get(SamplePath(kPistolModel));
  if (pistol_model_ == nullptr) {
    return;  // The cache logged why.
  }
  pistol_fire_clip_ = animation::MergeClips(pistol_model_->clips,
                                            std::string(kPistolFireClipName));
  pistol_animator_.emplace(&pistol_model_->hierarchy, &pistol_fire_clip_);
  pistol_world_.assign(pistol_model_->hierarchy.nodes.size(), glm::mat4(1.0f));
  for (const render::UploadedPart& part : pistol_model_->parts) {
    const std::string_view node_name =
        pistol_model_->hierarchy.nodes[static_cast<size_t>(part.node)].name;
    if (std::ranges::find(kPistolHiddenNodes, node_name) ==
        std::ranges::end(kPistolHiddenNodes)) {
      pistol_parts_.push_back(part);
    }
  }
  if (pistol_fire_clip_.channels.empty()) {
    core::log::Warn("Pistol model has no animation: it will not cycle");
  }
}

void App::ResetLoadout() {
  loadout_ = MakeLoadout();
  for (weapon::Viewmodel& viewmodel : viewmodels_) {
    viewmodel.Reset();
  }
  if (pistol_animator_) {
    pistol_animator_->Reset();
  }
  ApplyWeaponToPlayer();
}

void App::OnWeaponChanged() {
  active_viewmodel().Reset();
  if (loadout_.active() == weapon::WeaponSlot::kPistol && pistol_animator_) {
    pistol_animator_->Reset();
  }
  ApplyWeaponToPlayer();
}

void App::ApplyWeaponToPlayer() {
  player::PlayerTuning tuning = player_.tuning();
  tuning.aim_fov_scale = loadout_.active() == weapon::WeaponSlot::kPistol
                             ? kPistolAimFovScale
                             : player::PlayerTuning{}.aim_fov_scale;
  player_.set_tuning(tuning);
}

void App::PlacePlayerAtSceneCamera() {
  const scene::SceneCamera& camera = scene_.camera;
  const camera::LookAngles look =
      camera::LookTowards(camera.position, camera.target);
  const std::optional<physics::RayHit> floor = physics_world_.CastRay(
      camera.position, glm::vec3(0.0f, -1.0f, 0.0f), kSpawnProbeDistance);
  const glm::vec3 feet =
      floor ? floor->point
            : camera.position -
                  glm::vec3(0.0f, player_.tuning().eye_height_standing, 0.0f);

  if (player_.spawned()) {
    player_.Teleport(feet, look);
    return;
  }
  const auto spawned = player_.Spawn(physics_world_, {}, feet, look);
  if (!spawned.ok()) {
    core::log::Error("Player spawn failed: {}",
                     physics::ToString(spawned.error()));
    status_ = std::string("Player spawn failed: ") +
              physics::ToString(spawned.error());
    view_mode_ = ViewMode::kFly;
    camera_.set_mode(CameraModeFor(view_mode_));
  }
}

void App::RebuildPhysics() {
  const physics::RebuildSummary summary = scene_physics_.Rebuild(
      scene_,
      [this](std::string_view mesh_path) -> std::optional<physics::MeshView> {
        const std::optional<render::CachedMesh> mesh =
            mesh_cache_.Get(mesh_path);
        if (!mesh) {
          return std::nullopt;
        }
        return physics::MeshView{.bounds = mesh->bounds,
                                 .mesh = mesh->cpu_mesh};
      });
  core::log::Info("Physics: {} bodies created, {} failed", summary.created,
                  summary.failed);
  player_.OnWorldRebuilt();
}

void App::PushObject() {
  const ViewPose eye = CurrentView();
  const glm::vec3 forward = eye.forward;
  const std::optional<physics::RayHit> hit =
      physics_world_.CastRay(eye.position, forward, kPushReach);
  const std::optional<size_t> entity =
      hit && physics_world_.IsDynamic(hit->body)
          ? scene_physics_.EntityFor(hit->body)
          : std::nullopt;
  if (!entity) {
    status_ = "Push: nothing in reach";
    return;
  }
  physics_world_.AddImpulseAtPoint(hit->body, forward * kPushImpulse,
                                   hit->point);
  status_ = "Pushed " + scene_.entities[*entity].name;
}

void App::UpdateWeapon(const input::InputFrame& input, float step) {
  using input::Action;
  const weapon::LoadoutEvents loadout_events = loadout_.Update(
      {.select = SelectedSlot(input),
       .firearm = {.trigger_held = input.actions.held(Action::kFire),
                   .reload_pressed = input.actions.pressed(Action::kReload),
                   .fire_mode_pressed =
                       input.actions.pressed(Action::kToggleFireMode),
                   .can_fire = !player_.sprinting()}},
      step);
  const weapon::FirearmEvents& events = loadout_events.firearm;
  if (loadout_events.weapon_changed) {
    OnWeaponChanged();
  }

  const weapon::Firearm& firearm = loadout_.active_firearm();
  weapon::Viewmodel& viewmodel = active_viewmodel();
  const player::PlayerView& view = player_.view();
  viewmodel.Update({
      .eye_position = view.eye_position,
      .look = view.look,
      .roll = view.roll,
      .aim_held = player_.aiming(),
      .sprinting = player_.sprinting(),
      .on_ground = player_.on_ground(),
      .velocity = player_.velocity(),
      .look_delta = player_.look_delta(),
      .shots = events.shots,
      .reload_fraction = firearm.reload_fraction(),
      .reloading = firearm.reloading(),
      .raise_fraction = loadout_.raise_fraction(),
      .dt = step,
  });

  for (int i = 0; i < events.shots; ++i) {
    FireBullet(events.shot_age[static_cast<size_t>(i)]);
  }
  // Shows in the view from the next frame on, like mouse look.
  const glm::vec2 kick = viewmodel.TakeLookKick();
  player_.AddLookKick(kick.x, kick.y);

  // The holstered pistol keeps its last pose; it is reset when drawn.
  if (loadout_.active() == weapon::WeaponSlot::kPistol && pistol_animator_) {
    pistol_animator_->Update(events.shots, firearm.bolt_locked_back(), step);
  }
}

void App::FireBullet(float age) {
  const weapon::FirearmSpec& spec = loadout_.active_firearm().spec();
  const weapon::Viewmodel& viewmodel = active_viewmodel();
  const glm::vec3 muzzle = viewmodel.muzzle_position();
  // The bullet leaves the bore along the posed barrel and keeps the
  // shooter's own motion.
  const glm::vec3 velocity =
      viewmodel.muzzle_direction() * spec.muzzle_velocity + player_.velocity();

  // The weapon is not a physics body and can poke through a wall the player
  // stands against: then the bullet hits where the line from the eye to the
  // muzzle enters the wall instead of starting on the far side.
  const int shot_number = ++shots_fired_;
  const glm::vec3 eye = player_.view().eye_position;
  const glm::vec3 eye_to_muzzle = muzzle - eye;
  const float reach = glm::length(eye_to_muzzle);
  if (reach > 0.0f) {
    if (const std::optional<physics::RayHit> hit =
            physics_world_.CastRay(eye, eye_to_muzzle, reach)) {
      physics_world_.AddImpulseAtPoint(hit->body, velocity * spec.bullet_mass,
                                       hit->point);
      RecordHit({.point = hit->point,
                 .normal = hit->normal,
                 .velocity = velocity,
                 .mass = spec.bullet_mass,
                 .body = hit->body,
                 .flight_time = 0.0f,
                 .distance = 0.0f,
                 .shot_number = shot_number});
      return;
    }
  }
  projectiles_.Spawn(muzzle, velocity, spec.bullet_mass, spec.bullet_drag, age,
                     shot_number);
}

void App::RecordHit(const weapon::Impact& impact) {
  ++impacts_total_;
  if (const std::optional<size_t> entity =
          scene_physics_.EntityFor(impact.body)) {
    for (size_t i = 0; i < targets_.size(); ++i) {
      if (targets_[i].entity == *entity) {
        // The range is the shooter's actual distance, wherever they fired
        // from.
        weapon::Target target = targets_[i].target;
        if (player_.spawned()) {
          target.range_m =
              glm::length(target.center - player_.view().eye_position);
        }
        (void)target_log_.Record(static_cast<int>(i), target, impact);
        break;
      }
    }
  }
  if (physics_world_.IsDynamic(impact.body)) {
    return;  // A mark would not move with the body.
  }
  impact_marks_[next_impact_mark_] = BoxAlong(
      impact.point + impact.normal * (kImpactMarkDepth * 0.5f), impact.normal,
      {kImpactMarkSize, kImpactMarkSize, kImpactMarkDepth});
  next_impact_mark_ = (next_impact_mark_ + 1) % kMaxImpactMarks;
  impact_mark_count_ = std::min(impact_mark_count_ + 1, kMaxImpactMarks);
}

void App::ClearBullets() {
  projectiles_.Clear();
  impact_mark_count_ = 0;
  next_impact_mark_ = 0;
  impacts_total_ = 0;
  shots_fired_ = 0;
  target_log_.Clear();
}

void App::FindTargets() {
  targets_.clear();
  target_names_.clear();
  const glm::vec3 firing_position = scene_.camera.position;
  for (size_t i = 0; i < scene_.entities.size(); ++i) {
    const scene::Entity& entity = scene_.entities[i];
    if (!entity.name.starts_with(kTargetNamePrefix)) {
      continue;
    }
    if (targets_.size() >= static_cast<size_t>(kMaxTargets)) {
      core::log::Warn("More than {} targets; '{}' is not logged", kMaxTargets,
                      entity.name);
      continue;
    }
    // A plate is the builtin unit cube scaled thin along its local z; its
    // face normal is whichever of +-z faces the firing position.
    const scene::Transform& transform = entity.transform;
    glm::vec3 normal = transform.rotation * glm::vec3(0.0f, 0.0f, 1.0f);
    if (glm::dot(normal, firing_position - transform.position) < 0.0f) {
      normal = -normal;
    }
    targets_.push_back({
        .entity = i,
        .target = {.center = transform.position,
                   .normal = normal,
                   .up = transform.rotation * glm::vec3(0.0f, 1.0f, 0.0f),
                   .half_extents = 0.5f * glm::vec2(transform.scale),
                   .range_m =
                       glm::length(transform.position - firing_position)},
    });
    target_names_.push_back(entity.name);
  }
}

void App::InitializeScene(const CliOptions& options) {
  if (options.scene_path && platform::PathExists(scene_path_)) {
    auto loaded = scene::LoadScene(scene_path_);
    if (loaded.ok()) {
      scene_ = std::move(loaded).value();
      status_ = "Loaded " + scene_path_;
      return;
    }
    core::log::Error("Failed to load '{}': {}", scene_path_,
                     scene::ToString(loaded.error()));
    status_ = std::string("Load failed: ") + scene::ToString(loaded.error());
  }

  scene_.camera.position = kFiringPosition;
  scene_.camera.target = kFiringPosition + kDownRange;

  // One plane rather than several: overlapping coplanar planes would flicker.
  // Its static box is 0.1 m thick (see physics::internal::BuildBox), 48 x 724
  // m: fine for Jolt, and float spacing at 700 m is under 0.1 mm.
  scene::Transform ground;
  ground.position.z = 0.5f * (kGroundBackZ + kGroundFrontZ);
  ground.scale = glm::vec3(kGroundWidth / kBuiltinPlaneSize, 1.0f,
                           (kGroundBackZ - kGroundFrontZ) / kBuiltinPlaneSize);
  scene_.entities.push_back({
      .name = "ground",
      .mesh_path = "builtin:plane",
      .transform = ground,
      .body = scene::RigidBody{.motion = scene::MotionType::kStatic,
                               .shape = scene::ShapeType::kBox},
  });

  // Static and collided against triangle by triangle, so the player can walk
  // around it and through its openings.
  scene::Transform house;
  house.position = kHousePosition;
  scene_.entities.push_back({
      .name = "house",
      .mesh_path = SamplePath(kHouseModel),
      .transform = house,
      .body = scene::RigidBody{.motion = scene::MotionType::kStatic,
                               .shape = scene::ShapeType::kTriangleMesh},
  });

  AddRangeTargets();

  if (options.imports.empty()) {
    // Unit cube centred at the origin, dropped from 3 m at a tilt.
    scene::Transform cube;
    cube.position.y = 3.0f;
    cube.rotation = glm::angleAxis(glm::radians(30.0f),
                                   glm::normalize(glm::vec3(1.0f, 0.0f, 1.0f)));
    scene_.entities.push_back({
        .name = "cube",
        .mesh_path = "builtin:cube",
        .transform = cube,
        .body = scene::RigidBody{.motion = scene::MotionType::kDynamic,
                                 .shape = scene::ShapeType::kBox},
    });
  } else {
    AddImports(options.imports);
  }
}

void App::AddRangeTargets() {
  const glm::vec3 right =
      glm::normalize(glm::cross(kDownRange, camera::kWorldUp));
  for (const RangeTarget& target : kRangeTargets) {
    scene::Transform transform;
    transform.position =
        kFiringPosition + kDownRange * target.range + right * target.offset;
    transform.position.y = kTargetHeight;
    // The cube's faces are axis aligned, so with kDownRange along -Z the
    // thin axis already faces the firing line.
    transform.scale = glm::vec3(target.size, kTargetThickness);
    scene_.entities.push_back({
        .name = std::format("{}{:.0f} m", kTargetNamePrefix, target.range),
        .mesh_path = "builtin:cube",
        .transform = transform,
        .body = scene::RigidBody{.motion = scene::MotionType::kStatic,
                                 .shape = scene::ShapeType::kBox},
    });
  }
}

void App::AddImports(const std::vector<std::string>& imports) {
  const float centered_offset =
      (static_cast<float>(imports.size()) - 1.0f) * 0.5f;
  for (size_t i = 0; i < imports.size(); ++i) {
    const std::string& mesh_path = imports[i];
    const std::optional<render::CachedMesh> mesh = mesh_cache_.Get(mesh_path);
    if (!mesh) {
      continue;  // The cache already logged why.
    }
    const glm::vec3 ground{
        (static_cast<float>(i) - centered_offset) * kImportSpacing, 0.0f, 0.0f};
    scene::Transform transform = scene::FitOnGround(
        mesh->bounds.min, mesh->bounds.max, kImportTargetSize, ground);
    transform.position.y += kImportDropHeight;
    scene_.entities.push_back({
        .name = core::FileStem(imports[i]),
        .mesh_path = mesh_path,
        .transform = transform,
        .body = scene::RigidBody{.motion = scene::MotionType::kDynamic,
                                 .shape = scene::ShapeType::kConvexHull,
                                 .mass = kImportMass},
    });
  }
}

void App::SaveCurrentScene() {
  // The scene holds the current (interpolated) poses. Velocities are not part
  // of the file, so loaded bodies start at rest.
  scene_.camera = view_mode_ == ViewMode::kPlayer
                      ? player_.view().ToSceneCamera(settings_.fov_degrees)
                      : camera_.ToSceneCamera();
  const auto start = std::chrono::steady_clock::now();
  const auto result = scene::SaveScene(scene_, scene_path_);
  if (result.ok()) {
    status_ = TimedStatus("Saved", scene_path_, start);
    core::log::Info("{}", status_);
  } else {
    status_ = std::string("Save failed: ") + scene::ToString(result.error());
    core::log::Error("{}", status_);
  }
}

void App::ReloadCurrentScene() {
  const auto start = std::chrono::steady_clock::now();
  auto loaded = scene::LoadScene(scene_path_);
  if (loaded.ok()) {
    scene_ = std::move(loaded).value();
    camera_.SetFromSceneCamera(scene_.camera);
    RebuildPhysics();
    ClearBullets();  // Their impacts would name bodies that are gone.
    // A reload is a fresh start: both weapons full, the rifle in hand.
    ResetLoadout();
    FindTargets();
    PlacePlayerAtSceneCamera();
    status_ = TimedStatus("Loaded", scene_path_, start);
    core::log::Info("{}", status_);
  } else {
    status_ = std::string("Load failed: ") + scene::ToString(loaded.error());
    core::log::Error("{}", status_);
  }
}

int App::Run() {
  using Clock = std::chrono::steady_clock;
  auto previous = Clock::now();
  auto next_shader_poll = previous + kShaderPollInterval;

  for (;;) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      HandleEvent(event);
    }

    ApplyMouseCapture();
    if (input_.quit_requested()) {
      break;
    }

    if (window_.IsMinimized()) {
      // Sleep until something happens instead of spinning.
      SDL_WaitEvent(nullptr);
      previous = Clock::now();
      continue;
    }

    const auto now = Clock::now();
    const float frame_seconds =
        std::chrono::duration<float>(now - previous).count();
    previous = now;

    const input::InputFrame input = input_.EndFrame();
    for (const input::Action action : input::AllActions()) {
      if (input::IsEdgeTriggered(action) && input.actions.pressed(action)) {
        HandleAction(action);
      }
    }
    const float step = std::min(frame_seconds, kMaxSimulationStep);
    if (view_mode_ == ViewMode::kPlayer) {
      // Both hands are busy during a reload or a weapon switch: no aiming
      // down the sights.
      input::InputFrame player_input = input;
      if (loadout_.active_firearm().reloading() || loadout_.switching()) {
        player_input.actions.SetHeld(input::Action::kAim, false);
      }
      player_.Update(player_input, settings_, step);
    } else {
      // The character stays put under gravity while a debug camera is used.
      player_.Update({}, settings_, step);
      camera_.Update(input, settings_, step);
    }

    // The world clamps long frames itself. Poses must be synced before the
    // draw items and the overlay read the scene; the player's eye follows its
    // interpolated character.
    physics_world_.Update(frame_seconds);
    scene_physics_.SyncToScene(scene_);
    player_.UpdateView(settings_, step);

    // The rifle follows the final eye of this frame. Bullets spawned now fly
    // first by their shot age, then everything is swept against the world
    // as just stepped.
    if (view_mode_ == ViewMode::kPlayer && player_.spawned()) {
      UpdateWeapon(input, step);
    } else {
      // Debug cameras neither fire, aim nor switch weapons; a running reload
      // or switch still finishes.
      if (loadout_.Update({}, step).weapon_changed) {
        OnWeaponChanged();
      }
    }
    projectiles_.Update(physics_world_, scene_.gravity, step);
    for (const weapon::Impact& impact : projectiles_.impacts()) {
      RecordHit(impact);
    }

    // Between frames: no command buffer is being recorded.
    if (now >= next_shader_poll) {
      renderer_.ReloadChangedShaders();
      next_shader_poll = now + kShaderPollInterval;
    }

    player_state_ = PlayerStateText(player_);
    const physics::PhysicsStats physics_stats = physics_world_.Stats();
    const render::RenderStats stats = renderer_.stats();
    const ui::SceneOverlayInfo scene_info{
        .camera = MakeCameraOverlayInfo(),
        .physics = {.bodies = physics_stats.bodies,
                    .active_bodies = physics_stats.active_bodies,
                    .step_ms = physics_stats.last_step_ms,
                    .gravity = scene_.gravity},
        .shadows = {.enabled = shadows_enabled_ && scene_.sun.casts_shadows,
                    .cascade_debug = cascade_debug_,
                    .shadow_ms =
                        renderer_.gpu_scope_ms(render::GpuScope::kShadow),
                    .main_ms = renderer_.gpu_scope_ms(render::GpuScope::kMain)},
        .weapon = MakeWeaponOverlayInfo(),
        .hit_log = MakeHitLogOverlayInfo(),
        // The player aims with the rifle's sights only; the dot is a tool of
        // the fly camera, which pushes objects with it.
        .show_crosshair = view_mode_ == ViewMode::kFly,
        .entity_count = scene_.entities.size(),
        .draw_calls = stats.draw_calls,
        .culled_draws = stats.culled_draws,
        .shadow_draw_calls = stats.shadow_draw_calls,
        .shadow_culled_draws = stats.shadow_culled_draws,
        .triangles = stats.triangles,
        .scene_path = scene_path_,
        .status = status_,
        .shader_error = renderer_.last_shader_error(),
        .help = help_text_,
    };
    const ui::BuildResult ui =
        overlay_.Build(frame_seconds * 1000.0f, renderer_.gpu_frame_ms(),
                       scene_info, settings_);
    if (ui.settings_changed) {
      SaveSettings();
    }
    if (ui.clear_hit_log) {
      // Starts a new string of shots; the marks on the plates stay.
      target_log_.Clear();
      shots_fired_ = 0;
    }
    renderer_.DrawFrame(BuildSceneView(), ui.draw_data);
  }
  return 0;
}

void App::HandleEvent(const SDL_Event& event) {
  overlay_.ProcessEvent(event);

  // Presses that ImGui is consuming (a slider drag, a click on a window) stay
  // out of the game. Releases and window events always go through so that no
  // key or button gets stuck.
  const bool key_press = event.type == SDL_EVENT_KEY_DOWN;
  const bool mouse_press = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                           event.type == SDL_EVENT_MOUSE_WHEEL;
  const bool ui_keyboard = key_press && overlay_.WantsKeyboard();
  const bool ui_mouse =
      mouse_press && !mouse_captured_ && overlay_.WantsMouse();
  if (!ui_keyboard && !ui_mouse) {
    input_.HandleEvent(event);
  }

  if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
    renderer_.RequestSwapchainRebuild();
  }
}

void App::ApplyMouseCapture() {
  if (input_.wants_mouse_captured() != mouse_captured_) {
    mouse_captured_ = input_.wants_mouse_captured();
    window_.SetRelativeMouseMode(mouse_captured_);
  }
}

void App::HandleAction(input::Action action) {
  using input::Action;
  switch (action) {
    case Action::kCycleCamera:
      CycleViewMode();
      break;
    case Action::kToggleInvertY:
      settings_.invert_y = !settings_.invert_y;
      SaveSettings();
      break;
    case Action::kSensitivityDown:
      ChangeSensitivity(1.0f / kSensitivityStep);
      break;
    case Action::kSensitivityUp:
      ChangeSensitivity(kSensitivityStep);
      break;
    case Action::kToggleOverlay:
      overlay_.ToggleVisible();
      break;
    case Action::kSaveScene:
      SaveCurrentScene();
      break;
    case Action::kLoadScene:
      ReloadCurrentScene();
      break;
    case Action::kToggleVsync:
      renderer_.SetVsync(!renderer_.vsync());
      break;
    case Action::kPush:
      PushObject();
      break;
    case Action::kToggleCascadeDebug:
      cascade_debug_ = !cascade_debug_;
      break;
    case Action::kToggleShadows:
      shadows_enabled_ = !shadows_enabled_;
      break;
    case Action::kEscape:  // Handled inside the mapper (capture / quit).
    case Action::kMoveForward:
    case Action::kMoveBack:
    case Action::kMoveLeft:
    case Action::kMoveRight:
    case Action::kFlyUp:
    case Action::kFlyDown:
    case Action::kJump:
    case Action::kSprint:
    case Action::kCrouch:
    case Action::kFire:  // The weapon reads these from the frame directly.
    case Action::kAim:
    case Action::kReload:
    case Action::kToggleFireMode:
    case Action::kEquipPistol:
    case Action::kEquipRifle:
    case Action::kCount:
      break;
  }
}

void App::CycleViewMode() {
  switch (view_mode_) {
    case ViewMode::kPlayer: {
      // The debug camera takes off from where the player is looking.
      const player::PlayerView& view = player_.view();
      camera_.SetPose(view.eye_position, view.look);
      view_mode_ = ViewMode::kFly;
      break;
    }
    case ViewMode::kFly:
      view_mode_ = ViewMode::kOrbit;
      input_.set_mouse_mode(platform::InputMapper::MouseMode::kDrag);
      break;
    case ViewMode::kOrbit:
      view_mode_ = player_.spawned() ? ViewMode::kPlayer : ViewMode::kFly;
      input_.set_mouse_mode(platform::InputMapper::MouseMode::kCapture);
      break;
  }
  camera_.set_mode(CameraModeFor(view_mode_));
}

App::ViewPose App::CurrentView() const {
  if (view_mode_ == ViewMode::kPlayer) {
    const player::PlayerView& view = player_.view();
    return {.position = view.eye_position,
            .forward = view.Forward(),
            .view = view.View(),
            .look = view.look,
            .vertical_fov_degrees = view.vertical_fov_degrees};
  }
  return {.position = camera_.position(),
          .forward = camera_.Forward(),
          .view = camera_.View(),
          .look = camera_.look(),
          .vertical_fov_degrees = camera_.vertical_fov_degrees()};
}

void App::ChangeSensitivity(float factor) {
  settings_.mouse_sensitivity =
      std::clamp(settings_.mouse_sensitivity * factor,
                 input::kMinMouseSensitivity, input::kMaxMouseSensitivity);
  SaveSettings();
}

void App::SaveSettings() {
  if (!input::SaveInputSettings(settings_, settings_path_)) {
    status_ = "Saving input settings failed";
  }
}

ui::CameraOverlayInfo App::MakeCameraOverlayInfo() const {
  const ViewPose eye = CurrentView();
  const bool is_player = view_mode_ == ViewMode::kPlayer;
  return {
      .mode = ModeLabel(view_mode_),
      .position = eye.position,
      .yaw_degrees = glm::degrees(eye.look.yaw),
      .pitch_degrees = glm::degrees(eye.look.pitch),
      .speed = is_player ? player_.horizontal_speed() : camera_.fly_speed(),
      .fov_degrees = eye.vertical_fov_degrees,
      .sensitivity = settings_.mouse_sensitivity,
      .invert_y = settings_.invert_y,
      .state = is_player ? std::string_view(player_state_) : std::string_view(),
      .hint = CameraHint(view_mode_, mouse_captured_),
  };
}

ui::HitLogOverlayInfo App::MakeHitLogOverlayInfo() const {
  return {
      .log = &target_log_,
      .target_names = target_names_,
      .shots_fired = shots_fired_,
  };
}

std::optional<render::ReticleSight> App::CurrentSight() const {
  const weapon::Viewmodel& viewmodel = active_viewmodel();
  // Iron sights (the pistol) have no lens and no dot.
  if (view_mode_ != ViewMode::kPlayer || !player_.spawned() ||
      !(viewmodel.lens_radius() > 0.0f)) {
    return std::nullopt;
  }
  // The model has no glass: the window is drawn in the hood's front opening,
  // lifted a hair towards the eye so it never fights the hood's own faces.
  const glm::vec3 axis = viewmodel.optic_axis();
  const glm::mat4 lift =
      glm::translate(glm::mat4(1.0f), -axis * kLensLiftTowardsEye);
  render::ReticleSight sight;
  sight.lens = lift * viewmodel.lens_matrix();
  sight.axis = axis;
  return sight;
}

ui::WeaponOverlayInfo App::MakeWeaponOverlayInfo() const {
  const weapon::Firearm& firearm = loadout_.active_firearm();
  return {
      .name = firearm.spec().display_name,
      .switching_to =
          loadout_.switching()
              ? loadout_.firearm(loadout_.selected()).spec().display_name
              : std::string_view(),
      .rounds_in_magazine = firearm.rounds_in_magazine(),
      .magazine_inserted = firearm.magazine_inserted(),
      .round_chambered = firearm.round_chambered(),
      .bolt_locked_back = firearm.bolt_locked_back(),
      .reloading = firearm.reloading(),
      .reload_fraction = firearm.reload_fraction(),
      .fire_mode =
          firearm.fire_mode() == weapon::FireMode::kAuto ? "AUTO" : "SEMI",
      .spare_magazines = firearm.spare_magazines(),
      .projectiles = static_cast<int>(projectiles_.projectiles().size()),
      .impacts = impacts_total_,
  };
}

void App::AppendWeaponDraws() {
  if (view_mode_ == ViewMode::kPlayer && player_.spawned()) {
    if (loadout_.active() == weapon::WeaponSlot::kPistol) {
      AppendPistolDraws();
    } else if (const std::optional<render::CachedMesh> rifle =
                   mesh_cache_.Get(rifle_mesh_path_)) {
      draws_.push_back({.mesh = rifle->id,
                        .transform = active_viewmodel().model_matrix(),
                        .bounds = rifle->bounds});
    }
  }

  const std::optional<render::CachedMesh> cube = mesh_cache_.Get(kBulletMesh);
  if (!cube) {
    return;
  }
  for (const weapon::Projectile& bullet : projectiles_.projectiles()) {
    const float speed = glm::length(bullet.velocity);
    const float length = std::min(speed * kTracerSeconds, bullet.distance);
    if (!(speed > 0.0f) || !(length > 0.0f)) {
      continue;
    }
    const glm::vec3 direction = bullet.velocity / speed;
    draws_.push_back(
        {.mesh = cube->id,
         .transform =
             BoxAlong(bullet.position - direction * (length * 0.5f), direction,
                      {kTracerThickness, kTracerThickness, length}),
         .bounds = cube->bounds});
  }
  for (size_t i = 0; i < impact_mark_count_; ++i) {
    draws_.push_back({.mesh = cube->id,
                      .transform = impact_marks_[i],
                      .bounds = cube->bounds});
  }
}

void App::AppendPistolDraws() {
  if (pistol_model_ == nullptr || !pistol_animator_) {
    return;
  }
  pistol_animator_->ComputeWorld(active_viewmodel().model_matrix(),
                                 pistol_world_);
  for (const render::UploadedPart& part : pistol_parts_) {
    draws_.push_back(
        {.mesh = part.mesh,
         .transform = pistol_world_[static_cast<size_t>(part.node)],
         .bounds = part.bounds});
  }
}

render::SceneView App::BuildSceneView() {
  const VkExtent2D extent = renderer_.swapchain().extent();
  const float aspect = static_cast<float>(extent.width) /
                       static_cast<float>(std::max(extent.height, 1u));

  draws_.clear();
  for (const scene::Entity& entity : scene_.entities) {
    if (const std::optional<render::CachedMesh> mesh =
            mesh_cache_.Get(entity.mesh_path)) {
      draws_.push_back({.mesh = mesh->id,
                        .transform = entity.transform.Matrix(),
                        .bounds = mesh->bounds});
    }
  }
  AppendWeaponDraws();

  const ViewPose eye = CurrentView();
  render::SceneView view;
  view.camera.position = eye.position;
  view.camera.view = eye.view;
  view.camera.vertical_fov_radians = glm::radians(eye.vertical_fov_degrees);
  view.camera.near_plane = kNearPlane;
  view.camera.projection = camera::ReversedInfinitePerspective(
      view.camera.vertical_fov_radians, aspect, kNearPlane);
  view.light = {.position = scene_.light.position,
                .color = scene_.light.color,
                .intensity = scene_.light.intensity};
  view.ambient = scene_.ambient;
  view.sun = scene_.sun;
  view.shadows_enabled = shadows_enabled_;
  view.cascade_debug = cascade_debug_;
  view.draws = draws_;
  view.sight = CurrentSight();
  return view;
}

}  // namespace blue
