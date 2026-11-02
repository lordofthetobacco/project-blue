#include "app.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>

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

namespace blue {

namespace {

constexpr int kInitialWidth = 1920;
constexpr int kInitialHeight = 1080;
constexpr float kNearPlane = 0.1f;
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

// The default scene's ground: the builtin plane is 12 m square, scaled up so
// the house and the player have room.
constexpr float kGroundScale = 4.0f;  // 48 m square
// The sample house (6.4 x 7.1 x 14.8 m, authored in metres, base at y = 0),
// placed to the left of the default camera's view and clear of the cube.
constexpr const char* kHouseModel = "house.obj";
constexpr glm::vec3 kHousePosition{-9.0f, 0.0f, -3.0f};

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
  return text;
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
      physics_world_(physics_runtime_),
      scene_physics_(physics_world_),
      overlay_(window_, renderer_),
      scene_path_(options.scene_path.value_or(kDefaultScenePath)),
      settings_path_(InputSettingsPath()) {
  settings_ = input::LoadInputSettings(settings_path_);
  input_.set_bindings(settings_.bindings);
  help_text_ = BuildHelpText(settings_.bindings);
  SDL_SetWindowTitle(window_.handle(), WindowTitle(scene_path_).c_str());
  InitializeScene(options);
  camera_.SetFromSceneCamera(scene_.camera);
  RebuildPhysics();
  PlacePlayerAtSceneCamera();
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

  scene::Transform ground;
  ground.scale = glm::vec3(kGroundScale, 1.0f, kGroundScale);
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
      player_.Update(input, settings_, step);
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
        .show_crosshair = view_mode_ != ViewMode::kOrbit,
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
  return view;
}

}  // namespace blue
