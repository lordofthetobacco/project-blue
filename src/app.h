#ifndef BLUE_APP_H_
#define BLUE_APP_H_

#include <SDL3/SDL_events.h>
#include <vulkan/vulkan.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "animation/clip.h"
#include "animation/hierarchy.h"
#include "asset/mesh_data.h"
#include "camera/camera_controller.h"
#include "input/action.h"
#include "input/input_settings.h"
#include "lighting/environment.h"
#include "physics/physics_runtime.h"
#include "physics/physics_world.h"
#include "platform/input_mapper.h"
#include "platform/window.h"
#include "player/player_controller.h"
#include "render/renderer.h"
#include "scene/scene_data.h"
#include "ui/perf_overlay.h"
#include "weapon/loadout.h"
#include "weapon/motion_history.h"
#include "weapon/projectile_system.h"
#include "weapon/target_log.h"
#include "weapon/viewmodel.h"
#include "weapon/weapon_animator.h"
#include "weapon/weapon_geometry.h"
#include "world/gpu_backend.h"
#include "world/prefab_cache.h"
#include "world/prefab_pose.h"
#include "world/world.h"
#include "world/world_physics.h"

namespace blue {

struct CliOptions {
  std::optional<std::string> scene_path;
  std::optional<platform::WindowPair> window_position;
  bool show_help = false;
};

inline constexpr const char* kUsage =
    "usage: project-blue [--scene <file.mscn>] [--window-pos X,Y]";

// Parses the command line. Returns nullopt for unknown arguments, options
// missing their value and malformed values. --help / -h sets show_help.
[[nodiscard]] std::optional<CliOptions> ParseCommandLine(
    int argc, const char* const argv[]);

// Whose eyes the scene is rendered through.
enum class ViewMode {
  kPlayer,  // The first-person player; the default.
  kFly,     // Debug free-flight camera.
  kOrbit,   // Debug orbit camera, cursor never captured.
};

// The weapons' cooked data, loaded once at startup from their prefabs: the
// prefabs stay in the cache, the calibration comes from their sockets and
// the pistol's fire animation (copied out of the cache, so loading more
// prefabs never moves what the animator borrows) from its "fire" clip.
struct WeaponAssets {
  world::PrefabId rifle{};
  world::PrefabId pistol{};
  weapon::WeaponGeometry rifle_geometry;
  weapon::WeaponGeometry pistol_geometry;
  animation::Hierarchy pistol_hierarchy;
  animation::Clip pistol_fire_clip;
  weapon::WeaponAnimatorTuning pistol_tuning;
};

// The game application: owns the window, renderer, the scene (a .mscn
// resolved into a world::World), the player and debug camera, and runs the
// main loop. Startup failures are fatal. Neither copyable nor movable. Not
// thread-safe.
class App {
 public:
  // `process_start` is when main() was entered; the time to the first frame
  // is logged.
  App(const CliOptions& options,
      std::chrono::steady_clock::time_point process_start);

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
    scene::InstanceId instance = scene::kNoInstance;
    weapon::Target target;
  };

  // The builtin unit cube the bullet streaks and impact marks are drawn
  // with; uploaded once.
  struct EffectMesh {
    render::MeshId id{};
    asset::Bounds bounds;
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
  // Loads scene_path_ into scene_ and rebuilds the world (RebuildWorld()).
  // Returns false (status set, nothing changed) if the file cannot be
  // loaded.
  bool LoadScene();
  // Rebuilds the world from scene_: instances, environment, bodies and
  // targets. Needed after every scene load and every prefab replacement.
  void RebuildWorld();
  // Reloads the scene file (F9) after re-reading every cached prefab whose
  // file changed: a fresh start with the loadout reset and the player back
  // at the scene's start. Made for the editor's save-and-play loop.
  void ReloadScene();
  // Appends the file times of the prefabs cached since the last call.
  void RecordPrefabWriteTimes();
  // Replaces every cached prefab whose file changed since it was loaded (a
  // file that fails to load is kept and logged) and re-reads the weapons if
  // theirs changed. Returns the number replaced; the world must be rebuilt
  // afterwards.
  int ReloadChangedPrefabs();
  // Re-reads weapons_ from the cached weapon prefabs and rebuilds what
  // depends on it (viewmodels, rifle pose, pistol animator). A broken prefab
  // keeps the previous calibration (logged).
  void RereadWeapons();
  // Puts the player on the floor under the scene's player start, looking
  // its way. Spawns the character on first use, teleports it afterwards.
  void PlacePlayerAtStart();
  // Points the debug camera from the scene's player start.
  void PlaceDebugCameraAtStart();
  [[nodiscard]] ViewPose CurrentView() const;
  [[nodiscard]] ui::CameraOverlayInfo MakeCameraOverlayInfo() const;
  // Makes the renderer's environment match scene_.environment: loads the
  // HDRI (or the generated default sky) when its path differs from the one
  // loaded, or always when `force`; otherwise only updates intensity and
  // rotation. A failed load keeps the default sky.
  void LoadEnvironment(bool force);
  // Recreates the physics bodies of world_; call after every world build.
  void RebuildPhysics();
  // Pushes the dynamic body in the middle of the view along the camera
  // direction.
  void PushObject();
  // Runs the loadout for one frame after the player's view is final: weapon
  // selection (keys 2 / 3), trigger, the pose of the weapon in hand, bullet
  // spawns, recoil and the pistol's fire animation. Player view mode only.
  void UpdateWeapon(const input::InputFrame& input, float step);
  // Brings the loadout back to a fresh start: both weapons full, the rifle
  // in hand, viewmodels and the pistol's animation at rest.
  void ResetLoadout();
  // Prepares the weapon that just came into the hands (Loadout's
  // weapon_changed): fresh viewmodel and animation, its aim zoom.
  void OnWeaponChanged();
  // Applies what depends on the weapon in hand to the player (aim zoom).
  void ApplyWeaponToPlayer();
  [[nodiscard]] weapon::Viewmodel& active_viewmodel() {
    return viewmodels_[static_cast<size_t>(loadout_.active())];
  }
  [[nodiscard]] const weapon::Viewmodel& active_viewmodel() const {
    return viewmodels_[static_cast<size_t>(loadout_.active())];
  }
  // Fires one bullet of the weapon in hand from its muzzle, `age` seconds
  // before the end of the frame, or lands it on whatever blocks the line from
  // the eye to the muzzle (a weapon poking through a wall must not shoot
  // through it).
  void FireBullet(float age);
  // Counts a bullet hit, logs it when the body is a target and, on a static
  // body, leaves an impact mark. Does not push the body.
  void RecordHit(const weapon::Impact& impact);
  // Removes every bullet, impact mark and logged hit.
  void ClearBullets();
  // Rebuilds targets_ from the instances named as targets; call after every
  // world build. Ranges are measured from the player start.
  void FindTargets();
  // Appends the draws of the weapon in hand to draws_ when the player view
  // shows it; returns whether it did.
  bool AppendHeldWeaponDraws();
  // Appends one draw per mesh of the pistol prefab as posed this frame.
  void AppendPistolDraws();
  // Appends the bullet streaks and the impact marks to draws_.
  void AppendEffectDraws();
  // Feeds this frame's weapon draws (the transforms of `weapon_draws`, seen
  // through `camera` on a target of `extent` pixels) to weapon_motion_ and,
  // when the history is valid, sets their previous_transform and the
  // camera's previous_view_projection so the weapon is motion-blurred.
  void ApplyWeaponMotion(std::span<render::DrawItem> weapon_draws,
                         float frame_seconds, VkExtent2D extent,
                         render::Camera& camera);
  [[nodiscard]] ui::WeaponOverlayInfo MakeWeaponOverlayInfo() const;
  [[nodiscard]] ui::HitLogOverlayInfo MakeHitLogOverlayInfo() const;
  // Returns the reflex sight as posed this frame, nullopt outside the
  // player view.
  [[nodiscard]] std::optional<render::ReticleSight> CurrentSight() const;
  // Returns the frame to draw; `frame_seconds` is its duration (since the
  // previous frame), for the weapon motion blur.
  [[nodiscard]] render::SceneView BuildSceneView(float frame_seconds);

  // Declaration order is construction order and, reversed, destruction order:
  // the overlay and renderer must go before the window they render into; the
  // prefab cache borrows the GPU backend, the world borrows the cache;
  // world_physics_ and player_ (which own bodies of the physics world) go
  // before the physics world, which needs the runtime.
  platform::Window window_;
  render::Renderer renderer_;
  world::RendererBackend gpu_backend_;
  world::PrefabCache prefab_cache_;
  world::World world_;
  // File time of every cached prefab when it was (re)loaded, indexed by
  // PrefabId; nullopt if it could not be read.
  std::vector<std::optional<int64_t>> prefab_write_times_;
  physics::PhysicsRuntime physics_runtime_;
  physics::PhysicsWorld physics_world_;
  world::WorldPhysics world_physics_;
  player::PlayerController player_;  // Owns a character in physics_world_.
  ui::PerfOverlay overlay_;

  scene::SceneData scene_;
  std::string scene_path_;
  std::string scene_name_;  // File name of scene_path_, for the overlay.
  std::string status_;
  std::vector<render::DrawItem> draws_;

  platform::InputMapper input_;
  camera::CameraController camera_;
  input::InputSettings settings_;
  std::string settings_path_;
  std::string help_text_;     // Key bindings line of the overlay.
  std::string player_state_;  // Overlay stance text, rebuilt per frame.
  ViewMode view_mode_ = ViewMode::kPlayer;

  // The player's weapons: the loadout owns one firearm per slot, and each
  // slot has its own viewmodel (indexed by weapon::WeaponSlot). Bullets are
  // not part of the scene and are dropped on every scene reload.
  WeaponAssets weapons_;  // Before the members built from it.
  weapon::Loadout loadout_;
  std::array<weapon::Viewmodel, weapon::kWeaponSlotCount> viewmodels_;
  weapon::ProjectileSystem projectiles_;
  // The rifle at rest (it has no animation).
  world::PrefabPose rifle_pose_;
  // Borrows weapons_.pistol_hierarchy and weapons_.pistol_fire_clip.
  weapon::WeaponAnimator pistol_animator_;
  // World matrix of every node of the pistol this frame, sized once.
  std::vector<glm::mat4> pistol_world_;
  // Last frame's weapon draws, for the weapon motion blur; invalidated
  // whenever the weapon jumps (respawn, a fresh viewmodel, a weapon reload)
  // or is not drawn.
  weapon::MotionHistory weapon_motion_;
  // This frame's weapon draw transforms for weapon_motion_; reused.
  std::vector<glm::mat4> weapon_transforms_;
  EffectMesh effect_cube_;
  // Ring buffer of the transforms of the most recent bullet holes on static
  // surfaces, each a small tile of the effect cube: the first
  // `impact_mark_count_` entries are valid, `next_impact_mark_` is the slot
  // the next mark overwrites.
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

  // Owned by renderer_; created once at startup. Tracers glow (emissive,
  // bright enough for bloom), impact marks are dark and matte.
  render::MaterialId tracer_material_ = render::kDefaultMaterial;
  render::MaterialId impact_material_ = render::kDefaultMaterial;
  // Exposure and post-processing, edited live in the overlay and not
  // saved; the exposure is reset from the scene's environment on every load.
  render::PostSettings post_settings_;
  // The HDRI path the renderer's environment was loaded from (empty for the
  // generated default sky); nullopt before the first LoadEnvironment().
  std::optional<std::string> environment_source_;

  std::chrono::steady_clock::time_point process_start_;
  bool first_frame_logged_ = false;
  bool mouse_captured_ = false;
  bool shadows_enabled_ = true;
  bool cascade_debug_ = false;
};

}  // namespace blue

#endif  // BLUE_APP_H_
