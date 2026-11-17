#ifndef BLUE_APP_H_
#define BLUE_APP_H_

#include <SDL3/SDL_events.h>

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

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
#include "weapon/firearm.h"
#include "weapon/projectile_system.h"
#include "weapon/target_log.h"
#include "weapon/viewmodel.h"

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

  // A target plate of the scene, found by name after every scene change.
  struct RangeTargetEntry {
    size_t entity = 0;  // Index into scene_.entities.
    weapon::Target target;
  };

  static constexpr size_t kMaxImpactMarks = 64;
  static constexpr int kMaxTargets = 16;
  static constexpr int kHitLogCapacity = 64;

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
  // Adds the shooting range's static target plates to the default scene.
  void AddRangeTargets();
  void SaveCurrentScene();
  void ReloadCurrentScene();
  // Recreates the physics bodies of `scene_`; call after every scene change.
  void RebuildPhysics();
  // Pushes the dynamic body in the middle of the view along the camera
  // direction.
  void PushObject();
  // Runs the rifle for one frame after the player's view is final: trigger,
  // viewmodel pose, bullet spawns and recoil. Player view mode only.
  void UpdateWeapon(const input::InputFrame& input, float step);
  // Fires one bullet from the muzzle, `age` seconds before the end of the
  // frame, or lands it on whatever blocks the line from the eye to the
  // muzzle (a rifle poking through a wall must not shoot through it).
  void FireBullet(float age);
  // Counts a bullet hit, logs it when the body is a target and, on a static
  // body, leaves an impact mark. Does not push the body.
  void RecordHit(const weapon::Impact& impact);
  // Removes every bullet, impact mark and logged hit.
  void ClearBullets();
  // Rebuilds targets_ from the entities named as targets; call after every
  // scene change. Ranges are measured from the scene camera.
  void FindTargets();
  // Appends the rifle, the bullet streaks and the impact marks to draws_.
  void AppendWeaponDraws();
  [[nodiscard]] ui::WeaponOverlayInfo MakeWeaponOverlayInfo() const;
  [[nodiscard]] ui::HitLogOverlayInfo MakeHitLogOverlayInfo() const;
  // Returns the reflex sight as posed this frame, nullopt outside the
  // player view.
  [[nodiscard]] std::optional<render::ReticleSight> CurrentSight() const;
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

  // The player's rifle. Bullets are not part of the scene: they are neither
  // saved nor loaded and are dropped on every scene reload.
  weapon::Firearm firearm_;
  weapon::Viewmodel viewmodel_;
  weapon::ProjectileSystem projectiles_;
  std::string rifle_mesh_path_;  // Resolved once; looked up every frame.
  // Ring buffer of the transforms of the most recent bullet holes on static
  // surfaces, each a small tile of the builtin cube: the first
  // `impact_mark_count_` entries are valid, `next_impact_mark_` is the slot the
  // next mark overwrites.
  std::array<glm::mat4, kMaxImpactMarks> impact_marks_{};
  size_t impact_mark_count_ = 0;
  size_t next_impact_mark_ = 0;
  int impacts_total_ = 0;  // Bullet hits since the last scene reload.
  int shots_fired_ = 0;    // Since the last scene reload; numbers the shots.
  // At most kMaxTargets, in scene order; target i of target_log_ is
  // targets_[i]. Rebuilt (and reallocated) only on scene changes.
  std::vector<RangeTargetEntry> targets_;
  std::vector<std::string> target_names_;  // Parallel to targets_.
  weapon::TargetLog target_log_{kHitLogCapacity, kMaxTargets};

  bool mouse_captured_ = false;
  bool shadows_enabled_ = true;
  bool cascade_debug_ = false;
};

}  // namespace blue

#endif  // BLUE_APP_H_
