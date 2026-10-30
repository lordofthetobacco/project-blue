#include "app.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>

#include "camera/projection.h"
#include "core/log.h"
#include "core/path.h"
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

[[nodiscard]] std::string_view ModeLabel(camera::CameraMode mode) {
  return mode == camera::CameraMode::kFly ? "Fly" : "Orbit";
}

[[nodiscard]] std::string_view CameraHint(camera::CameraMode mode,
                                          bool mouse_captured) {
  if (mode == camera::CameraMode::kOrbit) {
    return "LMB rotate, RMB/MMB pan, wheel zoom";
  }
  return mouse_captured ? "Esc to release" : "Click to look, Esc releases";
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
  SDL_SetWindowTitle(window_.handle(), WindowTitle(scene_path_).c_str());
  InitializeScene(options);
  camera_.SetFromSceneCamera(scene_.camera);
  RebuildPhysics();
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
}

void App::PushObject() {
  const glm::vec3 forward = camera_.Forward();
  const std::optional<physics::RayHit> hit =
      physics_world_.CastRay(camera_.position(), forward, kPushReach);
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

  scene_.entities.push_back({
      .name = "ground",
      .mesh_path = "builtin:plane",
      .transform = {},
      .body = scene::RigidBody{.motion = scene::MotionType::kStatic,
                               .shape = scene::ShapeType::kBox},
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
  scene_.camera = camera_.ToSceneCamera();
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
    for (const platform::InputAction action : input_.TakeActions()) {
      HandleAction(action);
    }
    camera_.set_mode(input_.mode());
    camera_.Update(input, settings_,
                   std::min(frame_seconds, kMaxSimulationStep));

    // The world clamps long frames itself. Poses must be synced before the
    // draw items and the overlay read the scene.
    physics_world_.Update(frame_seconds);
    scene_physics_.SyncToScene(scene_);

    // Between frames: no command buffer is being recorded.
    if (now >= next_shader_poll) {
      renderer_.ReloadChangedShaders();
      next_shader_poll = now + kShaderPollInterval;
    }

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
        .show_crosshair = camera_.mode() == camera::CameraMode::kFly,
        .entity_count = scene_.entities.size(),
        .draw_calls = stats.draw_calls,
        .triangles = stats.triangles,
        .scene_path = scene_path_,
        .status = status_,
        .shader_error = renderer_.last_shader_error(),
    };
    ImDrawData* draw_data = overlay_.Build(
        frame_seconds * 1000.0f, renderer_.gpu_frame_ms(), scene_info);
    renderer_.DrawFrame(BuildSceneView(), draw_data);
  }
  return 0;
}

void App::HandleEvent(const SDL_Event& event) {
  overlay_.ProcessEvent(event);
  input_.HandleEvent(event);

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

void App::HandleAction(platform::InputAction action) {
  switch (action) {
    case platform::InputAction::kToggleInvertY:
      settings_.invert_y = !settings_.invert_y;
      SaveSettings();
      break;
    case platform::InputAction::kSensitivityDown:
      ChangeSensitivity(1.0f / kSensitivityStep);
      break;
    case platform::InputAction::kSensitivityUp:
      ChangeSensitivity(kSensitivityStep);
      break;
    case platform::InputAction::kToggleOverlay:
      overlay_.ToggleVisible();
      break;
    case platform::InputAction::kSaveScene:
      SaveCurrentScene();
      break;
    case platform::InputAction::kLoadScene:
      ReloadCurrentScene();
      break;
    case platform::InputAction::kToggleVsync:
      renderer_.SetVsync(!renderer_.vsync());
      break;
    case platform::InputAction::kPush:
      PushObject();
      break;
    case platform::InputAction::kToggleCascadeDebug:
      cascade_debug_ = !cascade_debug_;
      break;
    case platform::InputAction::kToggleShadows:
      shadows_enabled_ = !shadows_enabled_;
      break;
  }
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
  return {
      .mode = ModeLabel(camera_.mode()),
      .position = camera_.position(),
      .yaw_degrees = glm::degrees(camera_.yaw()),
      .pitch_degrees = glm::degrees(camera_.pitch()),
      .fly_speed = camera_.fly_speed(),
      .sensitivity = settings_.mouse_sensitivity,
      .invert_y = settings_.invert_y,
      .hint = CameraHint(camera_.mode(), mouse_captured_),
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
      draws_.push_back(
          {.mesh = mesh->id, .transform = entity.transform.Matrix()});
    }
  }

  render::SceneView view;
  view.camera.position = camera_.position();
  view.camera.view = camera_.View();
  view.camera.vertical_fov_radians =
      glm::radians(camera_.vertical_fov_degrees());
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
