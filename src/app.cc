#include "app.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "asset/image_decoder.h"
#include "asset/mesh_data.h"
#include "camera/look.h"
#include "camera/projection.h"
#include "core/extent.h"
#include "core/log.h"
#include "core/path.h"
#include "game/inspect_screen_rules.h"
#include "game/part_stash.h"
#include "game/scene_pickups.h"
#include "game/weapon_builds.h"
#include "game/weapon_modding.h"
#include "input/action.h"
#include "input/bindings.h"
#include "interact/interaction_focus.h"
#include "lighting/environment.h"
#include "material/material.h"
#include "platform/file_system.h"
#include "prefab/prefab.h"
#include "prefab/prefab_file.h"
#include "scene/mscn_serializer.h"
#include "shader/shader_compiler.h"
#include "ui/weapon_inspect_screen.h"
#include "weapon/dropped_weapons.h"
#include "weapon/firearm.h"
#include "weapon/loadout.h"
#include "weapon/motion_history.h"
#include "weapon/parts/weapon_build.h"
#include "weapon/parts/weapon_tuning.h"
#include "weapon/projectile_system.h"
#include "weapon/target_log.h"
#include "weapon/viewmodel.h"
#include "weapon/weapon_def.h"
#include "weapon/weapon_item.h"
#include "weapon/weapon_throw.h"

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

constexpr float kPushReach = 100.0f;   // m
constexpr float kPushImpulse = 40.0f;  // N*s, about 4 m/s on a 10 kg body.
// How far below the player start the floor is looked for when placing the
// player; beyond that the feet go an eye height below the start.
constexpr float kSpawnProbeDistance = 3.0f;  // m

// Instances whose name starts with this are targets: the hit log finds them
// by name, so scenes made in the editor get them too.
constexpr std::string_view kTargetNamePrefix = "target ";

// Cooked content, in the assets directory next to the executable (built by
// `blue-pack samples`).
constexpr const char* kAssetDirectory = "assets";
constexpr const char* kDefaultScene = "range.mscn";
// The starting loadout, each prefab with the weapon type it is when it
// carries no `weapon` tag (assets cooked before prefabs had tags). The type
// is also the part catalogue preset the weapon is built from when the
// catalogue has it; the prefab is the single-piece fallback. The last one
// starts in hand.
struct StartingWeapon {
  const char* asset;
  std::string_view fallback_type;
};
constexpr std::array<StartingWeapon, 2> kStartingWeapons{{
    {"glock17.pbo", "glock17"},
    {"m16.pbo", "m16"},
}};

// Constant ambient light added to the image-based lighting (the value
// scenes had before .mscn).
constexpr glm::vec3 kAmbient{0.025f};

// Size of the generated sky used when a scene names no HDRI.
constexpr int kDefaultSkyWidth = 512;
constexpr int kDefaultSkyHeight = 256;

// A bullet is drawn as a thin box trailing it by the distance it covers in
// this time (about 4 m at the muzzle), never reaching back past the muzzle.
constexpr float kTracerSeconds = 0.004f;
constexpr float kTracerThickness = 0.02f;       // m
constexpr float kImpactMarkSize = 0.05f;        // m, side of the square mark
constexpr float kImpactMarkDepth = 0.004f;      // m, how far it stands out
constexpr float kLensLiftTowardsEye = 0.0005f;  // m, see CurrentSight()
// The interaction dot is gone at half the aim of a scoped weapon (1 / this).
constexpr float kScopeDotFadeRate = 2.0f;

// The interaction dot fades in and out over this time.
constexpr float kInteractionDotFadeSeconds = 0.1f;
// A line of sight ending this close before the aimed point still sees it
// (it grazes the item's own surface).
constexpr float kSightTolerance = 0.02f;  // m
// Where a weapon swapped out by a pick-up leaves from: in front of the
// chest, below and ahead of the eye.
constexpr float kTossAhead = 0.35f;     // m, horizontally
constexpr float kTossBelowEye = 0.35f;  // m
// The focused item's outline: white, like a selection.
constexpr glm::vec4 kFocusOutlineColor{1.0f, 1.0f, 1.0f, 1.0f};

// Hot copper-orange tracer glow, several times brighter than the sunlit
// scene so it reads at range (and feeds bloom).
material::MaterialDesc TracerMaterial() {
  material::MaterialDesc desc;
  desc.name = "tracer";
  desc.base_color = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
  desc.roughness = 1.0f;
  desc.emissive = glm::vec3(1.0f, 0.55f, 0.25f);
  desc.emissive_strength = 12.0f;
  return desc;
}

// Soot-dark, fully rough bullet hole.
material::MaterialDesc ImpactMaterial() {
  material::MaterialDesc desc;
  desc.name = "impact mark";
  desc.base_color = glm::vec4(0.02f, 0.02f, 0.02f, 1.0f);
  desc.roughness = 0.95f;
  return desc;
}

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

// Returns the path of the cooked asset `file_name` next to the executable.
[[nodiscard]] std::string AssetRoot() {
  return core::JoinPath(platform::BasePath(), kAssetDirectory);
}

[[nodiscard]] std::string AssetPath(std::string_view file_name) {
  return core::JoinPath(AssetRoot(), file_name);
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
      {Action::kInteract, "interact"},
      {Action::kDropWeapon, "drop weapon"},
      {Action::kInspectWeapon, "inspect weapon"},
      {Action::kZeroDown, "/"},
      {Action::kZeroUp, "scope zero"},
      {Action::kCycleCamera, "camera"},
      {Action::kPush, "push"},
      {Action::kToggleCascadeDebug, "cascades"},
      {Action::kToggleShadows, "shadows"},
      {Action::kToggleInvertY, "invert Y"},
      {Action::kSensitivityDown, "/"},
      {Action::kSensitivityUp, "sens"},
      {Action::kToggleOverlay, "overlay"},
      {Action::kLoadScene, "reload scene"},
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

// Returns the prefab at `path` from `cache`, loading it; a failure is fatal
// (the game cannot run without its weapons).
[[nodiscard]] world::PrefabId LoadRequiredPrefab(world::PrefabCache& cache,
                                                 const std::string& path) {
  core::Result<world::PrefabId, prefab::PrefabError> loaded = cache.Get(path);
  if (!loaded.ok()) {
    core::log::Fatal("Cannot load {}: {}", path,
                     prefab::ToString(loaded.error()));
  }
  return loaded.value();
}

// Returns the fallback weapon type of the starting weapon prefab at `path`,
// or an empty view for any other prefab.
[[nodiscard]] std::string_view FallbackWeaponType(std::string_view path) {
  for (const StartingWeapon& weapon : kStartingWeapons) {
    if (path == AssetPath(weapon.asset)) {
      return weapon.fallback_type;
    }
  }
  return {};
}

// Returns the readout's name of `mode`.
[[nodiscard]] std::string_view FireModeLabel(weapon::FireMode mode) {
  switch (mode) {
    case weapon::FireMode::kSemi:
      return "SEMI";
    case weapon::FireMode::kAuto:
      return "AUTO";
    case weapon::FireMode::kDoubleAction:
      return "DA";
  }
  return "?";
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

// Returns the transform that turns the unit cube into a box centred at
// `center` with its local z along the unit `axis` and `size` metres along
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

// Returns the target plate of a placed prefab: the box `bounds` (prefab
// space) placed by `world`, whose thinnest axis is the plate normal (turned
// towards `firing_position`). Up is the prefab's +Y unless that is the
// normal. Nullopt for a degenerate box.
[[nodiscard]] std::optional<weapon::Target> PlateTarget(
    const asset::Bounds& bounds, const glm::mat4& world,
    glm::vec3 firing_position) {
  const glm::vec3 center = glm::vec3(world * glm::vec4(bounds.center(), 1.0f));
  std::array<glm::vec3, 3> axes{};
  std::array<float, 3> extents{};
  for (int i = 0; i < 3; ++i) {
    const glm::vec3 axis = glm::vec3(world[i]);
    const float length = glm::length(axis);
    if (!(length > 0.0f) || !std::isfinite(length)) {
      return std::nullopt;
    }
    axes[static_cast<size_t>(i)] = axis / length;
    extents[static_cast<size_t>(i)] = 0.5f * bounds.size()[i] * length;
  }
  const size_t thin =
      static_cast<size_t>(std::ranges::min_element(extents) - extents.begin());
  const size_t up = thin == 1 ? 2 : 1;
  const size_t right = 3 - thin - up;
  glm::vec3 normal = axes[thin];
  if (glm::dot(normal, firing_position - center) < 0.0f) {
    normal = -normal;
  }
  return weapon::Target{
      .center = center,
      .normal = normal,
      .up = axes[up],
      .half_extents = {extents[right], extents[up]},
      .range_m = glm::length(center - firing_position),
  };
}

[[nodiscard]] std::string WindowTitle(const std::string& scene_path) {
  return "Project Blue \xE2\x80\x94 " + core::FileName(scene_path);
}

// Returns the milliseconds since `start`.
[[nodiscard]] double MillisecondsSince(
    std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - start)
      .count();
}

// Returns "<verb> <path> (<elapsed> ms)" with the time since `start`.
[[nodiscard]] std::string TimedStatus(
    std::string_view verb, const std::string& path,
    std::chrono::steady_clock::time_point start) {
  return std::format("{} {} ({:.1f} ms)", verb, path, MillisecondsSince(start));
}

}  // namespace

std::optional<CliOptions> ParseCommandLine(int argc, const char* const argv[]) {
  CliOptions options;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    if (argument == "--help" || argument == "-h") {
      options.show_help = true;
      return options;
    }
    const bool is_scene = argument == "--scene";
    const bool is_window_pos = argument == "--window-pos";
    if ((!is_scene && !is_window_pos) || i + 1 >= argc) {
      return std::nullopt;
    }
    const std::string_view value = argv[++i];
    if (is_scene) {
      options.scene_path = std::string(value);
      continue;
    }
    options.window_position = platform::ParseWindowPair(value);
    if (!options.window_position) {
      return std::nullopt;
    }
  }
  return options;
}

App::App(const CliOptions& options,
         std::chrono::steady_clock::time_point process_start)
    : window_("Project Blue", kInitialWidth, kInitialHeight,
              options.window_position),
      renderer_(window_, ShaderDirectory(), kShaderCompileMode),
      gpu_backend_(renderer_),
      prefab_cache_(gpu_backend_),
      world_(prefab_cache_),
      physics_world_(physics_runtime_),
      world_physics_(physics_world_),
      dropped_weapons_(physics_world_),
      overlay_(window_, renderer_),
      scene_path_(options.scene_path.value_or(AssetPath(kDefaultScene))),
      scene_name_(core::FileName(scene_path_)),
      settings_path_(InputSettingsPath()),
      weapon_builds_(prefab_cache_, AssetRoot()),
      process_start_(process_start) {
  (void)weapon_builds_.LoadCatalog();
  for (const StartingWeapon& weapon : kStartingWeapons) {
    const std::string path = AssetPath(weapon.asset);
    const world::PrefabId id = LoadRequiredPrefab(prefab_cache_, path);
    const std::optional<weapon::WeaponDefId> def = WeaponDefOfPrefab(id);
    if (!def) {
      core::log::Fatal("{} is not a usable weapon (see above)", path);
    }
    starting_weapons_.push_back(*def);
  }
  loadout_ = MakeStartingLoadout();
  SeedPartStash();
  RebuildWeaponViews();
  const asset::MeshData cube = asset::MakeCube();
  effect_cube_ = {.id = renderer_.UploadMesh(cube), .bounds = cube.bounds};
  tracer_material_ = renderer_.CreateMaterial(TracerMaterial());
  impact_material_ = renderer_.CreateMaterial(ImpactMaterial());
  settings_ = input::LoadInputSettings(settings_path_);
  input_.set_bindings(settings_.bindings);
  help_text_ = BuildHelpText(settings_.bindings);
  SDL_SetWindowTitle(window_.handle(), WindowTitle(scene_path_).c_str());
  RecordPrefabWriteTimes();
  if (!LoadScene()) {
    core::log::Fatal("{}", status_);
  }
  PlaceDebugCameraAtStart();
  PlacePlayerAtStart();
  ApplyWeaponToPlayer();
}

weapon::Loadout App::MakeStartingLoadout() const {
  std::vector<weapon::WeaponItem> weapons;
  for (const weapon::WeaponDefId def : starting_weapons_) {
    weapons.push_back(weapon::MakeFreshWeapon(def, weapon_library_.def(def)));
  }
  return weapon::Loadout(weapons);
}

void App::ResetLoadout() {
  CloseInspect();
  SeedPartStash();
  loadout_ = MakeStartingLoadout();
  dropped_weapons_.Clear();
  interaction_focus_.SetFocus(std::nullopt);
  for (WeaponView& view : weapon_views_) {
    view.viewmodel.Reset();
    if (view.animator) {
      view.animator->Reset();
    }
  }
  weapon_motion_.Invalidate();
  ApplyWeaponToPlayer();
}

void App::OnWeaponChanged() {
  weapon_motion_.Invalidate();
  if (loadout_.hands_empty()) {
    ApplyWeaponToPlayer();
    return;
  }
  WeaponView& view = active_view();
  view.viewmodel.Reset();
  if (view.animator) {
    view.animator->Reset();
  }
  ApplyWeaponToPlayer();
}

bool App::ChangeHeldWeaponBuild(const weapon::parts::WeaponBuild& build) {
  const weapon::WeaponItem* held = loadout_.active_item();
  if (held == nullptr) {
    status_ = "No weapon in hand to change";
    return false;
  }
  const core::Result<weapon::WeaponDefId, std::string> def =
      weapon_builds_.DefFor(build, weapon_library_);
  if (!def.ok()) {
    core::log::Error("Cannot build that weapon: {}", def.error());
    status_ = "Cannot build that weapon: " + def.error();
    return false;
  }
  AddWeaponViews();  // A new definition needs its view.
  weapon::WeaponItem changed = *held;
  weapon::RefitWeapon(changed, def.value(), weapon_library_.def(def.value()));
  if (!loadout_.ReplaceActive(std::move(changed))) {
    status_ = "A weapon cannot change its kind";
    return false;
  }
  OnWeaponChanged();
  return true;
}

bool App::ApplyModdedBuild(const weapon::parts::WeaponBuild& build) {
  // The new parts make another definition with a viewmodel of its own: it
  // takes over the inspect pose so the weapon stays where it is.
  const weapon::InspectState inspect = loadout_.hands_empty()
                                           ? weapon::InspectState{}
                                           : active_viewmodel().inspect_state();
  if (!ChangeHeldWeaponBuild(build)) {
    return false;
  }
  active_viewmodel().set_inspect_state(inspect);
  return true;
}

void App::SeedPartStash() {
  const weapon::parts::PartCatalog* catalog = weapon_builds_.catalog();
  if (catalog == nullptr) {
    part_stash_.Clear();
    return;
  }
  std::vector<weapon::parts::WeaponBuild> starting;
  starting.reserve(starting_weapons_.size());
  for (const weapon::WeaponDefId def : starting_weapons_) {
    starting.push_back(weapon_library_.def(def).build);
  }
  part_stash_.Seed(*catalog, starting);
}

void App::UpdateInspectToggle(const input::InputFrame& input) {
  const bool open = game::NextInspectOpen({
      .open = inspect_open_,
      .toggle_pressed = input.actions.pressed(input::Action::kInspectWeapon),
      .escape_pressed = input.actions.pressed(input::Action::kEscape),
      .player_view = view_mode_ == ViewMode::kPlayer && player_.spawned(),
      .weapon_in_hand = !loadout_.hands_empty(),
      .switching = loadout_.switching(),
  });
  if (open && !inspect_open_) {
    OpenInspect();
  } else if (!open && inspect_open_) {
    CloseInspect();
  }
}

void App::OpenInspect() {
  inspect_open_ = true;
  inspect_clicks_ = {};
  inspect_message_.clear();
  input_.set_mouse_mode(platform::InputMapper::MouseMode::kCursor);
  modding_.reset();
  if (const weapon::parts::PartCatalog* catalog = weapon_builds_.catalog()) {
    modding_.emplace(*catalog);
    modding_->SetBuild(active_def().build);
  }
}

void App::CloseInspect() {
  if (!inspect_open_) {
    return;
  }
  inspect_open_ = false;
  modding_.reset();
  inspect_clicks_ = {};
  inspect_message_.clear();
  input_.set_mouse_mode(platform::InputMapper::MouseMode::kCapture);
  input_.RequestCapture();
}

void App::ApplyInspectClicks() {
  const ui::InspectScreenResult clicks = inspect_clicks_;
  inspect_clicks_.select_row.reset();
  inspect_clicks_.install.reset();
  inspect_clicks_.remove = false;
  if (!inspect_open_ || !modding_ || loadout_.hands_empty()) {
    return;
  }
  modding_->SetBuild(active_def().build);
  if (clicks.select_row) {
    modding_->Select(*clicks.select_row);
  }
  const game::ApplyBuild apply = [this](const weapon::parts::WeaponBuild& b) {
    return ApplyModdedBuild(b);
  };
  core::Result<void, std::string> changed =
      core::Result<void, std::string>::Ok();
  if (clicks.install) {
    changed = modding_->Install(part_stash_, *clicks.install, apply);
  } else if (clicks.remove) {
    changed = modding_->Remove(part_stash_, apply);
  } else {
    return;
  }
  // Stash indices moved: the hover is stale until the pointer moves.
  inspect_clicks_.hovered_stash.reset();
  inspect_clicks_.remove_hovered = false;
  if (changed.ok()) {
    inspect_message_.clear();
  } else {
    inspect_message_ = "Cannot change that: " + changed.error();
  }
}

void App::DrawInspect() {
  if (!inspect_open_ || loadout_.hands_empty()) {
    return;
  }
  const weapon::WeaponDef& def = active_def();
  const bool has_parts = modding_.has_value() && !def.build.empty();
  std::vector<game::StatLine> stats;
  std::vector<game::StashLine> stash;
  std::string missing;
  ui::InspectScreenInfo info{
      .weapon_name = def.type.spec.display_name,
      .message = inspect_message_,
      .no_parts = !has_parts,
  };
  if (has_parts) {
    modding_->SetBuild(def.build);
    std::optional<weapon::parts::WeaponStats> preview;
    if (inspect_clicks_.hovered_stash) {
      preview =
          modding_->PreviewInstall(part_stash_, *inspect_clicks_.hovered_stash);
    } else if (inspect_clicks_.remove_hovered) {
      preview = modding_->PreviewRemove();
    }
    stats =
        game::MakeStatLines(modding_->stats(), preview ? &*preview : nullptr);
    stash = modding_->Candidates(part_stash_);
    missing = weapon::parts::MissingSummary(modding_->stats());
    info.rows = modding_->rows();
    info.selected_row = modding_->selected();
    info.can_remove = modding_->CanRemove();
    info.has_preview = preview.has_value();
  } else if (def.stats) {
    stats = game::MakeStatLines(*def.stats, nullptr);
  }
  info.missing = missing;
  info.stats = stats;
  info.stash = stash;
  inspect_clicks_ = ui::DrawInspectScreen(info);
}

void App::ApplyWeaponToPlayer() {
  player::PlayerTuning tuning = player_.tuning();
  const weapon::WeaponItem* held = loadout_.active_item();
  const std::optional<float> scale =
      held != nullptr ? weapon_library_.def(held->def).type.aim_fov_scale
                      : std::nullopt;
  tuning.aim_fov_scale = scale.value_or(player::PlayerTuning{}.aim_fov_scale);
  player_.set_tuning(tuning);
  player_.SetLookScale(1.0f);
  scope_zoom_.reset();
  scope_state_.reset();
  if (held != nullptr) {
    const weapon::WeaponDef& def = weapon_library_.def(held->def);
    if (const std::optional<weapon::ScopeSpec>& scope = def.type.scope) {
      scope_zoom_.emplace(*scope, held->scope_magnification);
      active_viewmodel().SetZeroDistance(
          weapon::ClampZero(*scope, held->scope_zero));
    } else if (def.zero_range) {
      active_viewmodel().SetZeroDistance(
          weapon::parts::ClampZero(*def.zero_range, held->scope_zero));
    }
  }
}

prefab::Prefab App::WeaponPrefab(world::PrefabId id) const {
  prefab::Prefab prefab = prefab_cache_.prefab(id);  // Shares the bulk.
  const std::string_view fallback = FallbackWeaponType(prefab_cache_.key(id));
  if (!fallback.empty() && !prefab::FindTag(prefab, weapon::kWeaponTag)) {
    core::log::Info("{} has no '{}' tag; using '{}' (re-cook the samples)",
                    prefab_cache_.key(id), weapon::kWeaponTag, fallback);
    prefab::SetTag(prefab, weapon::kWeaponTag, fallback);
  }
  return prefab;
}

std::optional<weapon::WeaponDefId> App::WeaponDefOfPrefab(world::PrefabId id) {
  const prefab::Prefab prefab = WeaponPrefab(id);
  if (const std::optional<std::string_view> tag =
          prefab::FindTag(prefab, weapon::kWeaponTag)) {
    if (const std::optional<weapon::WeaponDefId> preset =
            weapon_builds_.PresetDef(*tag, weapon_library_)) {
      return preset;
    }
  }
  return weapon_library_.Register(static_cast<uint32_t>(id), prefab,
                                  prefab_cache_.key(id));
}

void App::RebuildWeaponViews() {
  weapon_views_.clear();
  AddWeaponViews();
  weapon_motion_.Invalidate();
  // The fresh viewmodels start at their tuning's zero: give the weapon in
  // hand its scope's zero back.
  ApplyWeaponToPlayer();
}

void App::AddWeaponViews() {
  weapon_views_.reserve(weapon_library_.size());
  for (size_t i = weapon_views_.size(); i < weapon_library_.size(); ++i) {
    const auto id = static_cast<weapon::WeaponDefId>(i);
    const weapon::WeaponDef& def = weapon_library_.def(id);
    WeaponView view{.viewmodel =
                        weapon::Viewmodel(def.geometry, def.type.viewmodel)};
    view.viewmodel.SetInspectBounds(def.bounds.center(),
                                    0.5f * glm::length(def.bounds.size()));
    view.part_poses.reserve(def.parts.size());
    for (const weapon::PartPlacement& part : def.parts) {
      if (part.prefab_key) {
        view.part_poses.emplace_back(world::PrefabPose(
            prefab_cache_, static_cast<world::PrefabId>(*part.prefab_key)));
      } else {
        view.part_poses.emplace_back(std::nullopt);
      }
    }
    if (def.fire) {
      // Borrows the definition, whose address the library keeps stable.
      view.animator.emplace(&def.fire->hierarchy, &def.fire->clip,
                            def.fire->tuning);
      view.node_world.assign(def.fire->hierarchy.nodes.size(), glm::mat4(1.0f));
    }
    weapon_views_.push_back(std::move(view));
    // Bodies already dropped keep their boxes until the next reset.
    (void)dropped_weapons_.SetShape(id, def.bounds);
  }
}

void App::FindScenePickups() {
  const size_t known = weapon_library_.size();
  scene_pickups_.Find(scene_, world_, [this](world::PrefabId prefab) {
    return WeaponDefOfPrefab(prefab);
  });
  if (weapon_library_.size() != known) {
    AddWeaponViews();  // The views in use stay as they are.
  }
}

void App::PlacePlayerAtStart() {
  const scene::PlayerStart& start = scene_.player_start;
  const camera::LookAngles look{.yaw = start.yaw_radians,
                                .pitch = start.pitch_radians};
  const std::optional<physics::RayHit> floor = physics_world_.CastRay(
      start.position, glm::vec3(0.0f, -1.0f, 0.0f), kSpawnProbeDistance);
  const glm::vec3 feet =
      floor ? floor->point
            : start.position -
                  glm::vec3(0.0f, player_.tuning().eye_height_standing, 0.0f);

  weapon_motion_.Invalidate();  // The view jumps.
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

void App::PlaceDebugCameraAtStart() {
  const scene::PlayerStart& start = scene_.player_start;
  camera_.SetPose(start.position,
                  {.yaw = start.yaw_radians, .pitch = start.pitch_radians});
}

void App::RebuildPhysics() {
  const world::PhysicsBuildSummary summary =
      world_physics_.Build(scene_, world_);
  core::log::Info("Physics: {} bodies created, {} failed", summary.created,
                  summary.failed);
  // Building removed every body, the dropped weapons' too, and brought back
  // every scene weapon picked up before.
  dropped_weapons_.RecreateBodies();
  player_.OnWorldRebuilt();
}

void App::PushObject() {
  const ViewPose eye = CurrentView();
  const glm::vec3 forward = eye.forward;
  const std::optional<physics::RayHit> hit =
      physics_world_.CastRay(eye.position, forward, kPushReach);
  const bool dynamic = hit && physics_world_.IsDynamic(hit->body);
  const std::optional<scene::InstanceId> instance =
      dynamic ? world_physics_.InstanceFor(hit->body) : std::nullopt;
  const weapon::DroppedWeapon* dropped =
      dynamic ? dropped_weapons_.FindByBody(hit->body) : nullptr;
  if (!instance && dropped == nullptr) {
    status_ = "Push: nothing in reach";
    return;
  }
  physics_world_.AddImpulseAtPoint(hit->body, forward * kPushImpulse,
                                   hit->point);
  if (dropped != nullptr) {
    status_ = std::format("Pushed the dropped {}",
                          dropped->weapon.firearm.spec().display_name);
    return;
  }
  const scene::Instance* pushed = scene::FindInstance(scene_, *instance);
  status_ = "Pushed " + (pushed != nullptr ? pushed->name : std::string("?"));
}

void App::UpdateInteraction(const input::InputFrame& input, float step) {
  // The inspect screen hides the dot and the focus outline.
  const bool player_view =
      view_mode_ == ViewMode::kPlayer && player_.spawned() && !inspect_open_;
  std::optional<interact::Focus> focus;
  if (player_view) {
    interactables_.clear();
    for (const weapon::DroppedWeapon& item : dropped_weapons_.items()) {
      interactables_.push_back(
          {.id = item.id,
           .kind = interact::InteractableKind::kDroppedWeapon,
           .shape = game::PlacedBox(weapon::DroppedWeapons::Matrix(item),
                                    dropped_weapons_.bounds(item.weapon.def))});
    }
    scene_pickups_.AppendCandidates(world_, interactables_);
    const player::PlayerView& view = player_.view();
    const glm::vec3 eye = view.eye_position;
    focus = interaction_focus_.Update(
        {.eye = eye, .direction = view.Forward()}, interactables_,
        [&](const interact::Interactable& candidate, glm::vec3 aim_point) {
          return IsInteractableVisible(candidate, eye, aim_point);
        });
  } else {
    interaction_focus_.SetFocus(std::nullopt);
  }
  const float fade = step / kInteractionDotFadeSeconds;
  interaction_dot_opacity_ =
      focus ? std::min(interaction_dot_opacity_ + fade, 1.0f)
            : std::max(interaction_dot_opacity_ - fade, 0.0f);
  if (!player_view) {
    interaction_dot_opacity_ = 0.0f;  // Never shown outside the player view.
    return;
  }

  if (input.actions.pressed(input::Action::kDropWeapon)) {
    DropHeldWeapon();
  }
  if (focus && input.actions.pressed(input::Action::kInteract)) {
    // What F does depends on the kind of the focused thing.
    switch (focus->kind) {
      case interact::InteractableKind::kDroppedWeapon:
        PickUpDroppedWeapon(focus->id);
        break;
      case interact::InteractableKind::kSceneWeapon:
        PickUpSceneWeapon(focus->id);
        break;
    }
  }
}

bool App::IsInteractableVisible(const interact::Interactable& candidate,
                                glm::vec3 eye, glm::vec3 aim_point) const {
  const glm::vec3 to_point = aim_point - eye;
  const float distance = glm::length(to_point);
  if (!(distance > kSightTolerance)) {
    return true;  // The eye is at (or in) the item.
  }
  const std::optional<physics::RayHit> hit =
      physics_world_.CastRay(eye, to_point, distance);
  if (!hit || hit->fraction * distance >= distance - kSightTolerance) {
    return true;
  }
  // The item's own body does not hide it.
  switch (candidate.kind) {
    case interact::InteractableKind::kDroppedWeapon: {
      const weapon::DroppedWeapon* item =
          dropped_weapons_.FindByBody(hit->body);
      return item != nullptr && item->id == candidate.id;
    }
    case interact::InteractableKind::kSceneWeapon:
      return world_physics_.InstanceFor(hit->body) == candidate.id;
  }
  return false;
}

void App::DropHeldWeapon() {
  if (loadout_.hands_empty()) {
    return;
  }
  // From where its model was drawn.
  const glm::mat4 held = active_viewmodel().model_matrix();
  std::optional<weapon::WeaponItem> weapon = loadout_.Drop();
  if (!weapon) {
    return;  // Being put away for a switch.
  }
  status_ = std::format("Dropped the {}", weapon->firearm.spec().display_name);
  ThrowWeapon(std::move(*weapon), held, weapon::ThrowTuning{});
  OnWeaponChanged();
}

void App::PickUpDroppedWeapon(uint32_t id) {
  std::optional<weapon::WeaponItem> weapon = dropped_weapons_.Take(id);
  if (!weapon) {
    return;
  }
  TakeWeapon(std::move(*weapon));
}

void App::PickUpSceneWeapon(scene::InstanceId instance) {
  // Gone from the running world until it is rebuilt; the scene data keeps
  // it (F9 brings it back).
  std::optional<weapon::WeaponItem> weapon =
      scene_pickups_.Take(instance, world_, world_physics_, weapon_library_);
  if (!weapon) {
    return;
  }
  interaction_focus_.SetFocus(std::nullopt);
  TakeWeapon(std::move(*weapon));
}

void App::TakeWeapon(weapon::WeaponItem weapon) {
  status_ = std::format("Picked up the {}", weapon.firearm.spec().display_name);
  // The model of the weapon it replaces, before the pick-up: the replaced
  // weapon leaves with its orientation.
  const weapon::WeaponItem* occupant =
      loadout_.item(weapon::SlotFor(weapon.firearm.spec().kind));
  glm::mat4 toss = occupant != nullptr
                       ? view_of(occupant->def).viewmodel.model_matrix()
                       : glm::mat4(1.0f);
  weapon::PickUpResult result = loadout_.PickUp(std::move(weapon));
  if (result.replaced) {
    // Tossed from the chest, in front of the player and below the item
    // just taken (whose body is gone already).
    const player::PlayerView& view = player_.view();
    glm::vec3 ahead = view.Forward();
    ahead.y = 0.0f;
    const float length = glm::length(ahead);
    ahead = length > 0.0f ? ahead / length : glm::vec3(0.0f);
    toss[3] = glm::vec4(view.eye_position + ahead * kTossAhead -
                            glm::vec3(0.0f, kTossBelowEye, 0.0f),
                        1.0f);
    ThrowWeapon(std::move(*result.replaced), toss, weapon::SwapTossTuning());
  }
  if (result.weapon_changed) {
    OnWeaponChanged();
  }
}

void App::ThrowWeapon(weapon::WeaponItem weapon, const glm::mat4& held,
                      const weapon::ThrowTuning& tuning) {
  const player::PlayerView& view = player_.view();
  const weapon::ThrowInput input{
      .held = held,
      .bounds = dropped_weapons_.bounds(weapon.def),
      .eye = view.eye_position,
      .forward = view.Forward(),
      .player_velocity = player_.velocity(),
  };
  const weapon::ThrowProbe probe = weapon::MakeThrowProbe(input);
  std::optional<float> blocked_at;
  if (probe.length > 0.0f) {
    if (const std::optional<physics::RayHit> hit = physics_world_.CastRay(
            probe.origin, probe.direction, probe.length)) {
      blocked_at = hit->fraction * probe.length;
    }
  }
  (void)dropped_weapons_.Add(std::move(weapon),
                             weapon::PlanThrow(input, tuning, blocked_at));
}

void App::UpdateWeapon(const input::InputFrame& input, float step,
                       glm::vec2 inspect_turn) {
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
  if (loadout_.hands_empty()) {
    return;  // Nothing to pose, fire or animate.
  }

  const weapon::Firearm& firearm = *loadout_.active_firearm();
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
      .inspecting = inspect_open_,
      .inspect_turn = inspect_turn,
      .dt = step,
  });

  if (!inspect_open_) {
    UpdateScope(input, step);
  }

  for (int i = 0; i < events.shots; ++i) {
    FireBullet(events.shot_age[static_cast<size_t>(i)]);
  }
  // Shows in the view from the next frame on, like mouse look.
  const glm::vec2 kick = viewmodel.TakeLookKick();
  player_.AddLookKick(kick.x, kick.y);

  // A holstered weapon keeps its last pose; it is reset when drawn.
  if (std::optional<weapon::WeaponAnimator>& animator =
          active_view().animator) {
    animator->Update(events.shots, firearm.bolt_locked_back(), step);
  }
}

void App::UpdateScope(const input::InputFrame& input, float step) {
  scope_state_.reset();
  if (!scope_zoom_) {
    player_.SetLookScale(1.0f);
    UpdateSightZero(input);
    return;
  }
  // The wheel zooms only while aiming with the weapon up; otherwise it
  // keeps doing nothing in the player view.
  if (player_.aiming() && !loadout_.switching() &&
      scope_zoom_->AddWheel(input.wheel)) {
    (void)loadout_.SetScopeMagnification(scope_zoom_->target());
  }
  scope_zoom_->Update(step);

  // The elevation turret, whenever the weapon is in hand. The new zero
  // applies from the next pose on.
  weapon::Viewmodel& viewmodel = active_viewmodel();
  const weapon::ScopeSpec& spec = scope_zoom_->spec();
  const int clicks = (input.actions.pressed(input::Action::kZeroUp) ? 1 : 0) -
                     (input.actions.pressed(input::Action::kZeroDown) ? 1 : 0);
  if (clicks != 0 && !loadout_.switching()) {
    const float zero =
        weapon::StepZero(spec, viewmodel.zero_distance(), clicks);
    if (zero != viewmodel.zero_distance()) {
      viewmodel.SetZeroDistance(zero);
      (void)loadout_.SetScopeZero(zero);
    }
  }

  player_.SetLookScale(
      weapon::ScopeLookScale(scope_zoom_->current(), viewmodel.aim_fraction()));
  const player::PlayerView& view = player_.view();
  const core::Extent2D extent = renderer_.output_extent();
  scope_state_ = weapon::MakeScopeViewState(
      spec, viewmodel, view.eye_position, scope_zoom_->current(),
      {.vertical_fov_radians = glm::radians(view.vertical_fov_degrees),
       .height_pixels = static_cast<float>(extent.height)});
}

void App::UpdateSightZero(const input::InputFrame& input) {
  const std::optional<weapon::parts::ZeroRange>& range =
      active_def().zero_range;
  const int clicks = (input.actions.pressed(input::Action::kZeroUp) ? 1 : 0) -
                     (input.actions.pressed(input::Action::kZeroDown) ? 1 : 0);
  if (!range || clicks == 0 || loadout_.switching()) {
    return;
  }
  weapon::Viewmodel& viewmodel = active_viewmodel();
  const float zero =
      weapon::parts::StepZero(*range, viewmodel.zero_distance(), clicks);
  if (zero != viewmodel.zero_distance()) {
    viewmodel.SetZeroDistance(zero);
    (void)loadout_.SetScopeZero(zero);
  }
}

void App::FireBullet(float age) {
  const weapon::FirearmSpec& spec = loadout_.active_firearm()->spec();
  const weapon::Viewmodel& viewmodel = active_viewmodel();
  const glm::vec3 muzzle = viewmodel.muzzle_position();
  // The bullet leaves the bore along the posed barrel, spread by the
  // weapon's accuracy, and keeps the shooter's own motion.
  const glm::vec3 direction = dispersion_.Apply(viewmodel.muzzle_direction(),
                                                active_def().accuracy_moa);
  const glm::vec3 velocity =
      direction * spec.muzzle_velocity + player_.velocity();

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
  if (const std::optional<scene::InstanceId> instance =
          world_physics_.InstanceFor(impact.body)) {
    for (size_t i = 0; i < targets_.size(); ++i) {
      if (targets_[i].instance == *instance) {
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
  const glm::vec3 firing_position = scene_.player_start.position;
  for (const world::Placement& placement : world_.placements()) {
    if (placement.parent != -1) {
      continue;  // Nested prefabs are part of their instance's plate.
    }
    const scene::Instance* instance =
        scene::FindInstance(scene_, placement.owner);
    if (instance == nullptr || !instance->name.starts_with(kTargetNamePrefix)) {
      continue;
    }
    if (targets_.size() >= static_cast<size_t>(kMaxTargets)) {
      core::log::Warn("More than {} targets; '{}' is not logged", kMaxTargets,
                      instance->name);
      continue;
    }
    const std::optional<weapon::Target> target =
        PlateTarget(prefab_cache_.prefab(placement.prefab).bounds,
                    placement.world, firing_position);
    if (!target) {
      core::log::Warn("Target '{}' has no usable bounds", instance->name);
      continue;
    }
    targets_.push_back({.instance = instance->id, .target = *target});
    target_names_.push_back(instance->name);
  }
}

bool App::LoadScene() {
  const auto start = std::chrono::steady_clock::now();
  core::Result<scene::SceneData, scene::MscnError> loaded =
      scene::LoadMscn(scene_path_);
  if (!loaded.ok()) {
    core::log::Error("Cannot load {}: {}", scene_path_,
                     scene::ToString(loaded.error()));
    status_ = std::format("Cannot load {}: {}", scene_name_,
                          scene::ToString(loaded.error()));
    return false;
  }
  scene_ = std::move(loaded).value();
  post_settings_.exposure_ev = scene_.environment.exposure_ev;
  RebuildWorld();
  RecordPrefabWriteTimes();  // Prefabs the scene loaded for the first time.
  core::log::Info("{}", TimedStatus("Loaded", scene_path_, start));
  status_ = TimedStatus("Loaded", scene_name_, start);
  return true;
}

void App::RebuildWorld() {
  for (const world::WorldIssue& issue : world_.Build(scene_)) {
    core::log::Warn("{}: instance {}: {}", scene_path_, issue.instance,
                    issue.message);
  }
  LoadEnvironment(/*force=*/false);
  RebuildPhysics();
  FindTargets();
  FindScenePickups();
}

void App::RecordPrefabWriteTimes() {
  for (size_t i = prefab_write_times_.size(); i < prefab_cache_.size(); ++i) {
    prefab_write_times_.push_back(platform::LastWriteTime(
        prefab_cache_.key(static_cast<world::PrefabId>(i))));
  }
}

int App::ReloadChangedPrefabs() {
  int replaced = 0;
  bool weapons_changed = false;
  for (size_t i = 0; i < prefab_write_times_.size(); ++i) {
    const auto id = static_cast<world::PrefabId>(i);
    const std::string path = prefab_cache_.key(id);
    const std::optional<int64_t> write_time = platform::LastWriteTime(path);
    if (!write_time || write_time == prefab_write_times_[i]) {
      continue;
    }
    core::Result<prefab::Prefab, prefab::PrefabError> loaded =
        prefab::LoadPrefab(path);
    if (!loaded.ok()) {
      // Possibly still being written: the next F9 tries again.
      core::log::Error("Cannot reload {}: {}", path,
                       prefab::ToString(loaded.error()));
      continue;
    }
    prefab_cache_.Replace(id, std::move(loaded).value());
    prefab_write_times_[i] = write_time;
    // It may be a part of built weapons too; they are rebuilt below.
    // A weapon's new data, or a prefab that just became a weapon; the views
    // draw the replaced prefab either way.
    weapons_changed =
        weapon_library_
            .Rebuild(static_cast<uint32_t>(id), WeaponPrefab(id), path)
            .has_value() ||
        weapons_changed;
    core::log::Info("Reloaded {}", path);
    ++replaced;
  }
  if (replaced > 0 && weapon_builds_.RebuildDefs(weapon_library_) > 0) {
    weapons_changed = true;
  }
  if (weapons_changed) {
    RebuildWeaponViews();
  }
  return replaced;
}

void App::ReloadScene() {
  // Replaced prefabs invalidate the world's draws and bodies: the world is
  // rebuilt below either way.
  const int replaced = ReloadChangedPrefabs();
  if (!LoadScene()) {
    if (replaced > 0) {
      RebuildWorld();  // The previous scene, on the new prefabs.
    }
    return;
  }
  ClearBullets();  // Their impacts would name bodies that are gone.
  // A reload is a fresh start: the starting weapons full, the rifle in
  // hand.
  ResetLoadout();
  PlaceDebugCameraAtStart();
  PlacePlayerAtStart();
  if (replaced > 0) {
    status_ += std::format(", {} prefab{} reloaded", replaced,
                           replaced == 1 ? "" : "s");
  }
}

void App::LoadEnvironment(bool force) {
  const scene::Environment& environment = scene_.environment;
  if (!force && environment_source_ == environment.hdri_path) {
    renderer_.SetEnvironmentParams(environment.intensity,
                                   environment.rotation_radians);
    return;
  }
  const auto start = std::chrono::steady_clock::now();
  std::optional<asset::ImageDataF> image;
  if (!environment.hdri_path.empty()) {
    auto loaded = asset::LoadHdrFile(environment.hdri_path);
    if (loaded.ok()) {
      image = std::move(loaded).value();
    } else {
      status_ = std::format("Cannot load HDRI {}: {}", environment.hdri_path,
                            asset::ToString(loaded.error()));
      core::log::Error("{}", status_);
    }
  }
  if (!image) {
    image =
        lighting::MakeDefaultEnvironment(kDefaultSkyWidth, kDefaultSkyHeight);
  }
  renderer_.SetEnvironment(*image, environment.intensity,
                           environment.rotation_radians);
  environment_source_ = environment.hdri_path;
  core::log::Info("{}", TimedStatus("Environment",
                                    environment.hdri_path.empty()
                                        ? std::string("(default sky)")
                                        : environment.hdri_path,
                                    start));
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

    const input::InputFrame raw_input = input_.EndFrame();
    UpdateInspectToggle(raw_input);
    // The inspect screen keeps the player's input from the game.
    const input::InputFrame input =
        inspect_open_ ? game::GateInspectInput(raw_input) : raw_input;
    const glm::vec2 inspect_turn =
        inspect_open_ ? game::InspectTurn(raw_input) : glm::vec2(0.0f);
    for (const input::Action action : input::AllActions()) {
      if (input::IsEdgeTriggered(action) && input.actions.pressed(action)) {
        HandleAction(action);
      }
    }
    const float step = std::min(frame_seconds, kMaxSimulationStep);
    if (view_mode_ == ViewMode::kPlayer) {
      // Both hands are busy during a reload or a weapon switch, and empty
      // hands have no sights: no aiming down the sights.
      input::InputFrame player_input = input;
      const weapon::Firearm* held = loadout_.active_firearm();
      if (held == nullptr || held->reloading() || loadout_.switching()) {
        player_input.actions.SetHeld(input::Action::kAim, false);
      }
      player_.Update(player_input, settings_, step);
    } else {
      // The character stays put under gravity while a debug camera is used.
      player_.Update({}, settings_, step);
      camera_.Update(input, settings_, step);
    }

    // The world clamps long frames itself. Poses must be synced before the
    // draw items and the overlay read the world; the player's eye follows
    // its interpolated character.
    physics_world_.Update(frame_seconds);
    world_physics_.SyncToWorld(world_);
    dropped_weapons_.Sync();
    world_.Update(step);
    player_.UpdateView(settings_, step);

    // The focus follows the final eye of this frame; a drop or pick-up
    // happens before the weapon in hand is posed.
    UpdateInteraction(input, step);
    // The rifle follows the final eye of this frame. Bullets spawned now fly
    // first by their shot age, then everything is swept against the world
    // as just stepped.
    scope_state_.reset();
    ApplyInspectClicks();
    if (view_mode_ == ViewMode::kPlayer && player_.spawned()) {
      UpdateWeapon(input, step, inspect_turn);
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
                    .cascade_debug = cascade_debug_},
        .weapon = MakeWeaponOverlayInfo(),
        .hit_log = MakeHitLogOverlayInfo(),
        // The player aims with the weapons' sights only; the dot is a tool
        // of the fly camera, which pushes objects with it.
        .show_crosshair = view_mode_ == ViewMode::kFly,
        // It would sit on the scope's picture: it fades out as a scope
        // comes up.
        .interaction_dot_opacity =
            scope_state_
                ? interaction_dot_opacity_ *
                      std::max(1.0f - kScopeDotFadeRate *
                                          active_viewmodel().aim_fraction(),
                               0.0f)
                : interaction_dot_opacity_,
        .entity_count = scene_.instances.size(),
        .draw_calls = stats.draw_calls,
        .culled_draws = stats.culled_draws,
        .shadow_draw_calls = stats.shadow_draw_calls,
        .shadow_culled_draws = stats.shadow_culled_draws,
        .triangles = stats.triangles,
        .scene_name = scene_name_,
        .status = status_,
        .shader_error = renderer_.last_shader_error(),
        .help = help_text_,
        .hide_windows = inspect_open_,
    };
    const ui::BuildResult ui =
        overlay_.Build(frame_seconds * 1000.0f, renderer_.gpu_frame_ms(),
                       scene_info, settings_, post_settings_);
    DrawInspect();
    ImDrawData* const ui_draw_data = overlay_.Render();
    if (ui.settings_changed) {
      SaveSettings();
    }
    if (ui.clear_hit_log) {
      // Starts a new string of shots; the marks on the plates stay.
      target_log_.Clear();
      shots_fired_ = 0;
    }
    renderer_.DrawFrame(BuildSceneView(frame_seconds), ui_draw_data);
    if (!first_frame_logged_) {
      first_frame_logged_ = true;
      core::log::Info("Startup: {:.0f} ms from process start to first frame",
                      MillisecondsSince(process_start_));
    }
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
      status_ = "Scenes are edited and saved in blue-editor";
      break;
    case Action::kLoadScene:
      ReloadScene();
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
    // Escape is handled inside the mapper (capture / quit) and, with the
    // inspect screen open, by UpdateInspectToggle() like kInspectWeapon.
    case Action::kEscape:
    case Action::kInspectWeapon:
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
    case Action::kInteract:  // UpdateInteraction(), player view only.
    case Action::kDropWeapon:
    case Action::kZeroUp:  // UpdateScope(), player view only.
    case Action::kZeroDown:
    case Action::kCount:
      break;
  }
}

void App::CycleViewMode() {
  CloseInspect();  // The screen belongs to the player view.
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
  if (view_mode_ != ViewMode::kPlayer || !player_.spawned() ||
      loadout_.hands_empty()) {
    return std::nullopt;
  }
  const weapon::Viewmodel& viewmodel = active_viewmodel();
  // Iron sights (the pistol) have no lens and no dot.
  if (!(viewmodel.lens_radius() > 0.0f)) {
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

std::optional<render::ScopeSight> App::CurrentScope() const {
  if (!scope_state_ || !scope_zoom_ || view_mode_ != ViewMode::kPlayer ||
      !player_.spawned() || loadout_.hands_empty()) {
    return std::nullopt;
  }
  const weapon::ScopeSpec& spec = scope_zoom_->spec();
  const weapon::ScopeOptics& optics = spec.optics;
  const weapon::ScopeViewState& state = *scope_state_;
  const weapon::Viewmodel& viewmodel = active_viewmodel();
  render::ScopeSight scope{
      .view = state.camera_view,
      .position = state.camera_position,
      .camera_tangent = state.camera_tangent,
      .resolution = state.picture_size,
      .render_picture = state.render_picture,
      .lens = state.lens,
      .right = viewmodel.lens_right(),
      .up = viewmodel.lens_up(),
      .axis = viewmodel.optic_axis(),
      .magnification = scope_zoom_->current(),
      .apparent_field = optics.apparent_field,
      .distortion = optics.distortion,
      .chromatic_aberration = optics.chromatic_aberration,
      .vignetting = optics.vignetting,
      .eye_lateral = state.eye.lateral,
      .eye_relief_error = state.eye.relief_error,
      .exit_pupil_radius = state.exit_pupil_radius,
      .eye_pupil_radius = optics.eye_pupil_radius,
      .pupil_shadow_depth = optics.pupil_shadow_depth,
  };
  const weapon::Reticle& reticle = spec.reticle;
  const size_t lines =
      std::min(static_cast<size_t>(
                   std::clamp(reticle.line_count, 0, weapon::kMaxReticleLines)),
               render::kMaxScopeReticleLines);
  for (size_t i = 0; i < lines; ++i) {
    const weapon::ReticleLine& line = reticle.lines[i];
    scope.reticle_lines[i] = glm::vec4(line.from * optics.apparent_field,
                                       line.to * optics.apparent_field);
    scope.reticle_half_widths[i] = line.half_width;
  }
  scope.reticle_line_count = static_cast<uint32_t>(lines);
  scope.reticle_color = reticle.color;
  return scope;
}

ui::WeaponOverlayInfo App::MakeWeaponOverlayInfo() const {
  const weapon::Firearm* held = loadout_.active_firearm();
  if (held == nullptr) {
    return {
        .hands_empty = true,
        .projectiles = static_cast<int>(projectiles_.projectiles().size()),
        .impacts = impacts_total_,
    };
  }
  const weapon::Firearm& firearm = *held;
  const weapon::Firearm* next = loadout_.switching() && loadout_.selected()
                                    ? loadout_.firearm(*loadout_.selected())
                                    : nullptr;
  const weapon::WeaponDef& def = active_def();
  // A part-built weapon's zero can be set without a scope too.
  const float zero =
      scope_zoom_ || def.zero_range ? active_viewmodel().zero_distance() : 0.0f;
  return {
      .name = firearm.spec().display_name,
      .stats = def.stats_text,
      .not_functional = def.missing_text,
      .switching_to =
          next != nullptr ? next->spec().display_name : std::string_view(),
      .rounds_in_magazine = firearm.rounds_in_magazine(),
      .magazine_inserted = firearm.magazine_inserted(),
      .round_chambered = firearm.round_chambered(),
      .bolt_locked_back = firearm.bolt_locked_back(),
      .reloading = firearm.reloading(),
      .reload_fraction = firearm.reload_fraction(),
      .fire_mode = FireModeLabel(firearm.fire_mode()),
      .scope_magnification = scope_zoom_ ? scope_zoom_->target() : 0.0f,
      .scope_zero = zero,
      .spare_magazines = firearm.spare_magazines(),
      .projectiles = static_cast<int>(projectiles_.projectiles().size()),
      .impacts = impacts_total_,
  };
}

bool App::AppendHeldWeaponDraws() {
  if (view_mode_ != ViewMode::kPlayer || !player_.spawned() ||
      loadout_.hands_empty()) {
    return false;
  }
  const WeaponView& view = active_view();
  if (view.animator) {
    AppendAnimatedWeaponDraws();
  } else {
    AppendRestWeaponDraws(active_def(), view, view.viewmodel.model_matrix());
  }
  return true;
}

void App::AppendRestWeaponDraws(const weapon::WeaponDef& def,
                                const WeaponView& view, const glm::mat4& root) {
  for (size_t i = 0; i < def.parts.size(); ++i) {
    if (const std::optional<world::PrefabPose>& pose = view.part_poses[i]) {
      pose->AppendDraws(root * def.parts[i].rest, draws_);
    }
  }
}

void App::AppendDroppedWeaponDraws() {
  selected_draws_.clear();
  const std::optional<interact::Focus>& focus = interaction_focus_.focus();
  for (const weapon::DroppedWeapon& item : dropped_weapons_.items()) {
    const size_t first = draws_.size();
    AppendRestWeaponDraws(weapon_library_.def(item.weapon.def),
                          view_of(item.weapon.def),
                          weapon::DroppedWeapons::Matrix(item));
    if (focus && focus->kind == interact::InteractableKind::kDroppedWeapon &&
        focus->id == item.id) {
      for (size_t i = first; i < draws_.size(); ++i) {
        selected_draws_.push_back(static_cast<uint32_t>(i));
      }
    }
  }
}

void App::AppendEffectDraws() {
  for (const weapon::Projectile& bullet : projectiles_.projectiles()) {
    const float speed = glm::length(bullet.velocity);
    const float length = std::min(speed * kTracerSeconds, bullet.distance);
    if (!(speed > 0.0f) || !(length > 0.0f)) {
      continue;
    }
    const glm::vec3 direction = bullet.velocity / speed;
    draws_.push_back(
        {.mesh = effect_cube_.id,
         .transform =
             BoxAlong(bullet.position - direction * (length * 0.5f), direction,
                      {kTracerThickness, kTracerThickness, length}),
         .bounds = effect_cube_.bounds,
         .material_override = tracer_material_});
  }
  for (size_t i = 0; i < impact_mark_count_; ++i) {
    draws_.push_back({.mesh = effect_cube_.id,
                      .transform = impact_marks_[i],
                      .bounds = effect_cube_.bounds,
                      .material_override = impact_material_});
  }
}

void App::AppendAnimatedWeaponDraws() {
  // The animator poses the definition's own hierarchy (a copy of the root
  // prefab's, so the cached prefab's meshes and their nodes match it).
  WeaponView& view = active_view();
  const weapon::WeaponDef& def = active_def();
  const glm::mat4 root = view.viewmodel.model_matrix();
  view.animator->ComputeWorld(root, view.node_world);
  // The node matrix, or the root for -1 and nodes the hierarchy lacks.
  const auto node_matrix = [&](int node) -> const glm::mat4& {
    return node < 0 || static_cast<size_t>(node) >= view.node_world.size()
               ? root
               : view.node_world[static_cast<size_t>(node)];
  };
  const auto id = static_cast<world::PrefabId>(
      weapon_library_.prefab_key(loadout_.active_item()->def));
  const prefab::Prefab& prefab = prefab_cache_.prefab(id);
  const world::PrefabGpu& gpu = prefab_cache_.gpu(id);
  for (size_t i = 0; i < prefab.meshes.size(); ++i) {
    const prefab::Mesh& mesh = prefab.meshes[i];
    draws_.push_back({.mesh = gpu.meshes[i],
                      .transform = node_matrix(mesh.node),
                      .bounds = mesh.bounds,
                      .submeshes = gpu.submeshes[i]});
  }
  // The other parts ride on the node their slot socket hangs on.
  for (size_t i = 1; i < def.parts.size(); ++i) {
    if (const std::optional<world::PrefabPose>& pose = view.part_poses[i]) {
      const weapon::PartPlacement& part = def.parts[i];
      pose->AppendDraws(node_matrix(part.root_node) * part.root_local, draws_);
    }
  }
}

void App::ApplyWeaponMotion(std::span<render::DrawItem> weapon_draws,
                            float frame_seconds, core::Extent2D extent,
                            render::Camera& camera) {
  weapon_transforms_.clear();
  for (const render::DrawItem& item : weapon_draws) {
    weapon_transforms_.push_back(item.transform);
  }
  const std::optional<weapon::PreviousMotion> previous =
      weapon_motion_.Advance({
          .slot = static_cast<int>(*loadout_.active()),
          .width = static_cast<int>(extent.width),
          .height = static_cast<int>(extent.height),
          .frame_seconds = frame_seconds,
          .view_projection = camera.projection * camera.view,
          .transforms = weapon_transforms_,
      });
  if (!previous) {
    return;  // The weapon goes out unblurred this frame.
  }
  camera.previous_view_projection = previous->view_projection;
  for (size_t i = 0; i < weapon_draws.size(); ++i) {
    weapon_draws[i].previous_transform = previous->transforms[i];
  }
}

render::SceneView App::BuildSceneView(float frame_seconds) {
  const core::Extent2D extent = renderer_.output_extent();
  const float aspect = static_cast<float>(extent.width) /
                       static_cast<float>(std::max(extent.height, 1u));

  draws_.clear();
  world_.AppendDraws(draws_);
  // The focused scene weapon is outlined like a dropped one.
  scene_selection_.clear();
  if (const std::optional<interact::Focus>& focus = interaction_focus_.focus();
      focus && focus->kind == interact::InteractableKind::kSceneWeapon) {
    scene_selection_.push_back(focus->id);
  }
  const size_t weapon_first = draws_.size();
  const bool weapon_drawn = AppendHeldWeaponDraws();
  const size_t weapon_count = draws_.size() - weapon_first;
  // The weapon carries the scope: never in the scope's own picture.
  for (size_t i = weapon_first; i < weapon_first + weapon_count; ++i) {
    draws_[i].hidden_in_scope = true;
  }
  AppendDroppedWeaponDraws();
  // World draws come first (index 0 on): the focused scene weapon's.
  world_.AppendSelectedDraws(scene_selection_, 0, selected_draws_);
  AppendEffectDraws();

  const ViewPose eye = CurrentView();
  render::SceneView view;
  view.camera.position = eye.position;
  view.camera.view = eye.view;
  view.camera.vertical_fov_radians = glm::radians(eye.vertical_fov_degrees);
  view.camera.near_plane = kNearPlane;
  view.camera.projection = camera::ReversedInfinitePerspective(
      view.camera.vertical_fov_radians, aspect, kNearPlane);
  view.camera.previous_view_projection =
      view.camera.projection * view.camera.view;
  if (weapon_drawn) {
    ApplyWeaponMotion(std::span(draws_).subspan(weapon_first, weapon_count),
                      frame_seconds, extent, view.camera);
  } else {
    weapon_motion_.Invalidate();  // E.g. a debug camera.
  }
  view.point_lights = world_.point_lights();
  view.ambient = kAmbient;
  view.sun = scene_.sun;
  view.shadows_enabled = shadows_enabled_;
  view.cascade_debug = cascade_debug_;
  view.draws = draws_;
  view.selected_draws = selected_draws_;
  view.selection_color = kFocusOutlineColor;
  view.sight = CurrentSight();
  view.scope = CurrentScope();
  view.post = post_settings_;
  view.frame_seconds = frame_seconds;
  return view;
}

}  // namespace blue
