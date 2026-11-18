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

#include "animation/clip.h"
#include "camera/camera_controller.h"
#include "input/action.h"
#include "input/input_settings.h"
#include "lighting/environment.h"
#include "physics/physics_runtime.h"
#include "physics/physics_world.h"
#include "physics/scene_physics.h"
#include "platform/input_mapper.h"
#include "platform/window.h"
#include "player/player_controller.h"
#include "render/material_cache.h"
#include "render/mesh_cache.h"
#include "render/model_cache.h"
#include "render/renderer.h"
#include "scene/scene.h"
#include "ui/editor_panels.h"
#include "ui/perf_overlay.h"
#include "weapon/loadout.h"
#include "weapon/projectile_system.h"
#include "weapon/target_log.h"
#include "weapon/viewmodel.h"
#include "weapon/weapon_animator.h"

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
  // Adds the metallic x roughness sweep of preview spheres to the default
  // scene, beside the spawn and clear of the firing line.
  void AddPreviewSpheres();
  // Sets the default scene's sky, sun, point light and exposure (see
  // scene/default_lighting.h).
  void ApplyDefaultLighting();
  // Makes the renderer's environment match scene_.environment: loads the
  // HDRI (or the generated default sky) when its path differs from the one
  // loaded, or always when `force`, and estimates its sun; otherwise only
  // updates intensity and rotation. A failed load keeps the default sky.
  void LoadEnvironment(bool force);
  // Returns the material that replaces all of `entity`'s own materials: its
  // material file, else the target steel for target plates, else nothing.
  [[nodiscard]] std::optional<render::MaterialId> EntityMaterial(
      const scene::Entity& entity);
  // Fills weapon_slots_ with the weapons' and effects' materials; call once
  // after they are loaded.
  void BuildWeaponMaterialSlots();
  // Refills entity_slots_ with the materials of the editor's selection.
  void UpdateEntityMaterialSlots();
  // Selects the entity in the middle of the view in the editor.
  void PickEntity();
  // Builds the editor panels (cursor free and overlay shown) and carries out
  // what they request.
  void UpdateEditor();
  void SaveCurrentScene();
  void ReloadCurrentScene();
  // Recreates the physics bodies of `scene_`; call after every scene change.
  void RebuildPhysics();
  // Pushes the dynamic body in the middle of the view along the camera
  // direction.
  void PushObject();
  // Runs the loadout for one frame after the player's view is final: weapon
  // selection (keys 2 / 3), trigger, the pose of the weapon in hand, bullet
  // spawns, recoil and the pistol's fire animation. Player view mode only.
  void UpdateWeapon(const input::InputFrame& input, float step);
  // Imports and uploads the pistol and prepares its fire animation. On
  // failure (logged) the pistol is not drawn.
  void LoadPistolModel();
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
  // Rebuilds targets_ from the entities named as targets; call after every
  // scene change. Ranges are measured from the scene camera.
  void FindTargets();
  // Appends the weapon in hand, the bullet streaks and the impact marks to
  // draws_.
  void AppendWeaponDraws();
  // Appends one draw per part of the pistol model as posed this frame.
  void AppendPistolDraws();
  [[nodiscard]] ui::WeaponOverlayInfo MakeWeaponOverlayInfo() const;
  [[nodiscard]] ui::HitLogOverlayInfo MakeHitLogOverlayInfo() const;
  // Returns the reflex sight as posed this frame, nullopt outside the
  // player view.
  [[nodiscard]] std::optional<render::ReticleSight> CurrentSight() const;
  // Creates the materials of the things drawn with the builtin cube that are
  // not scene entities of their own look: tracers, impact marks, targets.
  void CreateEffectMaterials();
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
  render::ModelCache model_cache_;
  render::MaterialCache material_cache_;
  physics::PhysicsRuntime physics_runtime_;
  physics::PhysicsWorld physics_world_;
  physics::ScenePhysics scene_physics_;
  player::PlayerController player_;  // Owns a character in physics_world_.
  ui::PerfOverlay overlay_;
  ui::EditorPanels editor_;

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

  // The player's weapons: the loadout owns one firearm per slot, and each
  // slot has its own viewmodel (indexed by weapon::WeaponSlot). Bullets are
  // not part of the scene: they are neither saved nor loaded and are dropped
  // on every scene reload.
  weapon::Loadout loadout_;
  std::array<weapon::Viewmodel, weapon::kWeaponSlotCount> viewmodels_;
  weapon::ProjectileSystem projectiles_;
  std::string rifle_mesh_path_;  // Resolved once; looked up every frame.
  // The pistol, loaded at startup so the first switch does not hitch. Owned
  // by model_cache_; null if loading failed, then the pistol is not drawn
  // (it still fires).
  const render::UploadedModel* pistol_model_ = nullptr;
  // The model's per-node clips merged into one firing animation; borrowed by
  // pistol_animator_, so it never changes after LoadPistolModel().
  animation::Clip pistol_fire_clip_;
  // Engaged with pistol_model_; borrows its hierarchy and pistol_fire_clip_.
  std::optional<weapon::WeaponAnimator> pistol_animator_;
  // World matrix of every node of pistol_model_ this frame, sized once at
  // startup.
  std::vector<glm::mat4> pistol_world_;
  // The parts of pistol_model_ that are drawn (all but kPistolHiddenNodes),
  // filled once at startup.
  std::vector<render::UploadedPart> pistol_parts_;
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

  // Owned by renderer_; created once at startup (CreateEffectMaterials).
  // Tracers glow (emissive, bright enough for bloom), impact marks are dark
  // and matte, target plates bare steel.
  render::MaterialId tracer_material_ = render::kDefaultMaterial;
  render::MaterialId impact_material_ = render::kDefaultMaterial;
  render::MaterialId target_material_ = render::kDefaultMaterial;
  // Post-processing settings edited in the editor's Post and Environment
  // panels; not saved. Its exposure is ignored: the scene's
  // environment.exposure_ev is used (and saved) instead.
  render::PostSettings post_settings_;
  // The materials the editor lists: the weapons' and effects' (built once
  // at startup) and the selected entity's (rebuilt while the editor shows).
  std::vector<ui::MaterialSlot> weapon_slots_;
  std::vector<ui::MaterialSlot> entity_slots_;
  // The HDRI path the renderer's environment was loaded from (empty for the
  // generated default sky); nullopt before the first LoadEnvironment().
  std::optional<std::string> environment_source_;
  // The sun found in the loaded environment (EstimateSun), for aligning the
  // scene's sun with it.
  lighting::SunEstimate environment_sun_;

  bool mouse_captured_ = false;
  bool shadows_enabled_ = true;
  bool cascade_debug_ = false;
};

}  // namespace blue

#endif  // BLUE_APP_H_
