#ifndef BLUE_APP_H_
#define BLUE_APP_H_

#include <SDL3/SDL_events.h>

#include <optional>
#include <string>
#include <vector>

#include "camera/camera_controller.h"
#include "input/action.h"
#include "input/input_settings.h"
#include "physics/physics_runtime.h"
#include "physics/physics_world.h"
#include "physics/scene_physics.h"
#include "platform/input_mapper.h"
#include "player/player_controller.h"
#include "platform/window.h"
#include "render/mesh_cache.h"
#include "render/renderer.h"
#include "scene/scene.h"
#include "ui/perf_overlay.h"

namespace blue {

struct CliOptions {
  std::optional<std::string> scene_path;
  std::vector<std::string> imports;
  bool show_help = false;
};

inline constexpr const char* kUsage =
    "usage: project-blue [--scene <file.bscn>] [--import <model>]...";

// Parses the command line. Returns nullopt for unknown arguments or options
// missing their value. --help / -h sets show_help.
[[nodiscard]] std::optional<CliOptions> ParseCommandLine(
    int argc, const char* const argv[]);

// Whose eyes the scene is rendered through.
enum class ViewMode {
  kPlayer,  // The first-person player; the default.
  kFly,     // Debug free-flight camera.
  kOrbit,   // Debug orbit camera, cursor never captured.
};

// The game application: owns the window, renderer, scene, player and debug
// camera, and runs the main loop. Startup failures are fatal. Neither
// copyable nor movable. Not thread-safe.
class App {
 public:
  explicit App(const CliOptions& options);

  App(const App&) = delete;
  App& operator=(const App&) = delete;

  // Runs the main loop until the user quits. Returns the process exit code.
  int Run();

 private:
  // The eye the frame is rendered from, in whichever mode is active.
  struct ViewPose {
    glm::vec3 position{0.0f};
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
    glm::mat4 view{1.0f};
    camera::LookAngles look;
    float vertical_fov_degrees = 60.0f;
  };

  void HandleEvent(const SDL_Event& event);
  void ApplyMouseCapture();
  void HandleAction(input::Action action);
  // Player -> Fly -> Orbit -> Player; Fly starts from the player's eye.
  void CycleViewMode();
  void ChangeSensitivity(float factor);
  void SaveSettings();
  // Puts the player on the floor under the scene camera, facing its target.
  // Spawns the character on first use, teleports it afterwards.
  void PlacePlayerAtSceneCamera();
  [[nodiscard]] ViewPose CurrentView() const;
  [[nodiscard]] ui::CameraOverlayInfo MakeCameraOverlayInfo() const;
  void InitializeScene(const CliOptions& options);
  void AddImports(const std::vector<std::string>& imports);
  void SaveCurrentScene();
  void ReloadCurrentScene();
  // Recreates the physics bodies of `scene_`; call after every scene change.
  void RebuildPhysics();
  // Pushes the dynamic body under the crosshair along the camera direction.
  void PushObject();
  [[nodiscard]] render::SceneView BuildSceneView();

  // Declaration order is construction order and, reversed, destruction order:
  // the overlay and renderer must go before the window they render into, and
  // scene_physics_ and player_ (which borrow the world) before the world,
  // which needs the runtime. The mesh cache owns the CPU meshes shapes are
  // built from, but bodies copy what they need, so it only has to outlive
  // Rebuild calls.
  platform::Window window_;
  render::Renderer renderer_;
  render::MeshCache mesh_cache_;
  physics::PhysicsRuntime physics_runtime_;
  physics::PhysicsWorld physics_world_;
  physics::ScenePhysics scene_physics_;
  player::PlayerController player_;  // Owns a character in physics_world_.
  ui::PerfOverlay overlay_;

  scene::Scene scene_;
  std::string scene_path_;
  std::string status_;
  std::vector<render::DrawItem> draws_;

  platform::InputMapper input_;
  camera::CameraController camera_;
  input::InputSettings settings_;
  std::string settings_path_;
  std::string help_text_;     // Key bindings line of the overlay.
  std::string player_state_;  // Overlay stance text, rebuilt per frame.
  ViewMode view_mode_ = ViewMode::kPlayer;

  bool mouse_captured_ = false;
  bool shadows_enabled_ = true;
  bool cascade_debug_ = false;
};

}  // namespace blue

#endif  // BLUE_APP_H_
