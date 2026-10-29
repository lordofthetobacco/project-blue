#ifndef BLUE_APP_H_
#define BLUE_APP_H_

#include <SDL3/SDL_events.h>

#include <optional>
#include <string>
#include <vector>

#include "camera/camera_controller.h"
#include "input/input_settings.h"
#include "platform/input_mapper.h"
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

// The game application: owns the window, renderer, scene and camera, and runs
// the main loop. Startup failures are fatal. Neither copyable nor movable.
// Not thread-safe.
class App {
 public:
  explicit App(const CliOptions& options);

  App(const App&) = delete;
  App& operator=(const App&) = delete;

  // Runs the main loop until the user quits. Returns the process exit code.
  int Run();

 private:
  void HandleEvent(const SDL_Event& event);
  void ApplyMouseCapture();
  void HandleAction(platform::InputAction action);
  void ChangeSensitivity(float factor);
  void SaveSettings();
  [[nodiscard]] ui::CameraOverlayInfo MakeCameraOverlayInfo() const;
  void InitializeScene(const CliOptions& options);
  void AddImports(const std::vector<std::string>& imports);
  void SaveCurrentScene();
  void ReloadCurrentScene();
  [[nodiscard]] render::SceneView BuildSceneView();

  // Declaration order is destruction order in reverse: the overlay and
  // renderer must go before the window they render into.
  platform::Window window_;
  render::Renderer renderer_;
  render::MeshCache mesh_cache_;
  ui::PerfOverlay overlay_;

  scene::Scene scene_;
  std::string scene_path_;
  std::string status_;
  std::vector<render::DrawItem> draws_;

  platform::InputMapper input_;
  camera::CameraController camera_;
  input::InputSettings settings_;
  std::string settings_path_;

  bool mouse_captured_ = false;
};

}  // namespace blue

#endif  // BLUE_APP_H_
