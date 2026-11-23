#ifndef BLUE_APP_H_
#define BLUE_APP_H_

#include <SDL3/SDL_events.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "asset/mesh_data.h"
#include "camera/camera_controller.h"
#include "game/part_stash.h"
#include "game/scene_pickups.h"
#include "game/weapon_builds.h"
#include "game/weapon_modding.h"
#include "input/action.h"
#include "input/input_frame.h"
#include "input/input_settings.h"
#include "interact/interaction_focus.h"
#include "lighting/environment.h"
#include "physics/physics_runtime.h"
#include "physics/physics_world.h"
#include "platform/input_mapper.h"
#include "platform/window.h"
#include "player/player_controller.h"
#include "prefab/prefab.h"
#include "render/renderer.h"
#include "scene/scene_data.h"
#include "ui/perf_overlay.h"
#include "ui/weapon_inspect_screen.h"
#include "weapon/dispersion.h"
#include "weapon/dropped_weapons.h"
#include "weapon/firearm.h"
#include "weapon/loadout.h"
#include "weapon/motion_history.h"
#include "weapon/parts/weapon_build.h"
#include "weapon/projectile_system.h"
#include "weapon/scope.h"
#include "weapon/scope_view.h"
#include "weapon/target_log.h"
#include "weapon/viewmodel.h"
#include "weapon/weapon_animator.h"
#include "weapon/weapon_def.h"
#include "weapon/weapon_item.h"
#include "weapon/weapon_throw.h"
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

  // How one weapon definition is posed and drawn (indexed by
  // weapon::WeaponDefId): the viewmodel and fire animation used while a
  // weapon of it is in the hands (reset whenever one comes into them), and
  // the models of its parts at rest.
  struct WeaponView {
    weapon::Viewmodel viewmodel;
    // Parallel to WeaponDef::parts: each part's prefab at rest; nullopt for
    // a part without a model.
    std::vector<std::optional<world::PrefabPose>> part_poses;
    // Borrows the definition's fire animation; nullopt for a static weapon.
    std::optional<weapon::WeaponAnimator> animator;
    // World matrix of every node of the animated root model this frame,
    // sized once.
    std::vector<glm::mat4> node_world;
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
  // Opens or closes the weapon inspect screen by this frame's raw input
  // (kInspectWeapon, kEscape) and closes it out of the player view or with
  // empty hands (game::NextInspectOpen).
  void UpdateInspectToggle(const input::InputFrame& input);
  // Frees the cursor and starts a modding session on the weapon in hand.
  void OpenInspect();
  // Ends the session and gives the player the mouse back. Does nothing
  // while closed.
  void CloseInspect();
  // Applies what the player clicked on the inspect screen last frame
  // (select, install, remove) before the weapon is posed, so a changed
  // weapon is posed before it is drawn.
  void ApplyInspectClicks();
  // Emits the inspect screen into this frame's ImGui frame and keeps what
  // the player clicked and hovers for the next frame.
  void DrawInspect();
  // Makes the weapon in hand one of `build` keeping its inspect pose
  // (ChangeHeldWeaponBuild); the modding session's apply step.
  bool ApplyModdedBuild(const weapon::parts::WeaponBuild& build);
  // Refills the part stash with the starter set for the starting weapons;
  // empties it without a catalogue.
  void SeedPartStash();
  // Player -> Fly -> Orbit -> Player; Fly starts from the player's eye.
  void CycleViewMode();
  void ChangeSensitivity(float factor);
  void SaveSettings();
  // Loads scene_path_ into scene_ and rebuilds the world (RebuildWorld()).
  // Returns false (status set, nothing changed) if the file cannot be
  // loaded.
  bool LoadScene();
  // Rebuilds the world from scene_: instances, environment, bodies,
  // targets and weapon pick-ups. Needed after every scene load and every
  // prefab replacement.
  void RebuildWorld();
  // Reloads the scene file (F9) after re-reading every cached prefab whose
  // file changed: a fresh start with the loadout reset and the player back
  // at the scene's start. Made for the editor's save-and-play loop.
  void ReloadScene();
  // Appends the file times of the prefabs cached since the last call.
  void RecordPrefabWriteTimes();
  // Replaces every cached prefab whose file changed since it was loaded (a
  // file that fails to load is kept and logged) and rebuilds the weapon
  // definitions of the replaced prefabs (a broken one keeps its previous
  // data, logged). Returns the number replaced; the world must be rebuilt
  // afterwards.
  int ReloadChangedPrefabs();
  // Returns cached prefab `id` as the weapon library reads it: the sample
  // weapons cooked before prefabs had tags get their `weapon` tag from
  // their file name.
  [[nodiscard]] prefab::Prefab WeaponPrefab(world::PrefabId id) const;
  // Returns the definition a weapon of prefab `id` (a starting weapon's or
  // a scene pick-up's) is: the preset named like its `weapon` tag if the
  // catalogue has one that can be built, else the prefab's own single-piece
  // definition; nullopt for a prefab that is no weapon.
  [[nodiscard]] std::optional<weapon::WeaponDefId> WeaponDefOfPrefab(
      world::PrefabId id);
  // Rebuilds weapon_views_ from the library (one per definition) and the
  // dropped weapons' shapes; the motion history is invalidated and the
  // weapon in hand re-applied (ApplyWeaponToPlayer). After
  // definitions were rebuilt (their prefabs reloaded).
  void RebuildWeaponViews();
  // Adds the views (and dropped-weapon shapes) of the definitions the
  // library gained since the last call; existing views are kept.
  void AddWeaponViews();
  // Finds the scene's weapon pick-ups (registering new weapon prefabs in
  // the library); call after every world build.
  void FindScenePickups();
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
  // Pushes the dynamic body (a scene instance or a dropped weapon) in the
  // middle of the view along the camera direction.
  void PushObject();
  // Runs the loadout for one frame after the player's view is final: weapon
  // selection (keys 2 / 3), trigger, the pose of the weapon in hand, bullet
  // spawns, recoil and its fire animation. Player view mode only.
  // While the inspect screen is open the weapon takes the inspect pose,
  // turned by `inspect_turn` ((yaw, pitch) radians), and no scope is used.
  void UpdateWeapon(const input::InputFrame& input, float step,
                    glm::vec2 inspect_turn);
  // Returns the starting loadout: a fresh weapon of each of
  // starting_weapons_, the last one ready in hand.
  [[nodiscard]] weapon::Loadout MakeStartingLoadout() const;
  // Brings the loadout back to a fresh start: the starting weapons full,
  // the rifle in hand, viewmodels and animations at rest, no dropped weapon
  // left in the world, the inspect screen closed and the stash re-seeded.
  void ResetLoadout();
  // Prepares the weapon that just came into the hands (Loadout's
  // weapon_changed, a drop or a pick-up): fresh viewmodel and animation,
  // its aim zoom; nothing but the motion history with empty hands.
  void OnWeaponChanged();
  // The entry point for changing the parts of the weapon in hand (the
  // inspect screen): makes it a weapon of `build` (a build of the same kind,
  // assembled on first sight), keeps its firearm state as the new parts allow
  // (weapon::RefitWeapon) and resets the viewmodel, fire animation and
  // motion history like a weapon change. Returns false (status_ says why)
  // with empty hands or when the build cannot be assembled; nothing changes
  // then.
  bool ChangeHeldWeaponBuild(const weapon::parts::WeaponBuild& build);
  // Applies what depends on the weapon in hand to the player (aim zoom,
  // plain look) and takes up its scope (scope_zoom_) at the magnification
  // and zero it remembers.
  void ApplyWeaponToPlayer();
  // After the weapon in hand is posed: steps its scope's magnification with
  // the wheel while aiming and its zero with kZeroUp / kZeroDown, slows the
  // look with the magnification as the aim comes up and sets scope_state_.
  // Without a scope, kZeroUp / kZeroDown step the zero within the weapon's
  // zero range (its sighting range), if it has one.
  void UpdateScope(const input::InputFrame& input, float step);
  // Steps the zero of the weapon in hand within its definition's zero range
  // by this frame's zero key presses.
  void UpdateSightZero(const input::InputFrame& input);
  // The definition of the weapon in hand; the hands must not be empty.
  [[nodiscard]] const weapon::WeaponDef& active_def() const {
    return weapon_library_.def(loadout_.active_item()->def);
  }
  // The view of weapon definition `def`.
  [[nodiscard]] WeaponView& view_of(weapon::WeaponDefId def) {
    return weapon_views_[static_cast<size_t>(def)];
  }
  [[nodiscard]] const WeaponView& view_of(weapon::WeaponDefId def) const {
    return weapon_views_[static_cast<size_t>(def)];
  }
  // The view of the weapon in hand; the hands must not be empty.
  [[nodiscard]] WeaponView& active_view() {
    return view_of(loadout_.active_item()->def);
  }
  [[nodiscard]] const WeaponView& active_view() const {
    return view_of(loadout_.active_item()->def);
  }
  [[nodiscard]] weapon::Viewmodel& active_viewmodel() {
    return active_view().viewmodel;
  }
  [[nodiscard]] const weapon::Viewmodel& active_viewmodel() const {
    return active_view().viewmodel;
  }
  // Finds what the player looks at (InteractionFocus over the dropped
  // weapons and the scene's weapon pick-ups, hidden ones rejected by a ray
  // from the eye), fades the interaction dot and handles kDropWeapon and
  // kInteract. Player view mode with a character only; otherwise it just
  // clears the focus.
  void UpdateInteraction(const input::InputFrame& input, float step);
  // Returns whether the line from the eye to `aim_point` on `candidate`
  // reaches it unblocked (the item's own body does not block).
  [[nodiscard]] bool IsInteractableVisible(
      const interact::Interactable& candidate, glm::vec3 eye,
      glm::vec3 aim_point) const;
  // Throws the weapon in hand away (G), from where its model is.
  void DropHeldWeapon();
  // Picks up dropped weapon `id` (F).
  void PickUpDroppedWeapon(uint32_t id);
  // Picks up the scene's weapon instance `instance` (F): the instance leaves
  // the running world (not the scene data) and a fresh weapon of its
  // definition goes into the hands.
  void PickUpSceneWeapon(scene::InstanceId instance);
  // Puts `weapon` into the slot of its kind; a weapon it replaces is tossed
  // in front of the player.
  void TakeWeapon(weapon::WeaponItem weapon);
  // Puts `weapon` into the world as a body leaving `held` (prefab space ->
  // world) the way `tuning` says, or dropped where the way is blocked.
  void ThrowWeapon(weapon::WeaponItem weapon, const glm::mat4& held,
                   const weapon::ThrowTuning& tuning);
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
  // Appends one draw per mesh of the animated weapon in hand as posed this
  // frame: the root model's meshes at their nodes, every other part at the
  // node it hangs on.
  void AppendAnimatedWeaponDraws();
  // Appends the draws of every part of weapon `def` (shown by `view`) at
  // rest, its root model placed by `root` (prefab space -> world).
  void AppendRestWeaponDraws(const weapon::WeaponDef& def,
                             const WeaponView& view, const glm::mat4& root);
  // Appends the dropped weapons' draws (at rest, at their bodies' poses) to
  // draws_ and the indices of the focused one's to selected_draws_.
  void AppendDroppedWeaponDraws();
  // Appends the bullet streaks and the impact marks to draws_.
  void AppendEffectDraws();
  // Feeds this frame's weapon draws (the transforms of `weapon_draws`, seen
  // through `camera` on a target of `extent` pixels) to weapon_motion_ and,
  // when the history is valid, sets their previous_transform and the
  // camera's previous_view_projection so the weapon is motion-blurred.
  void ApplyWeaponMotion(std::span<render::DrawItem> weapon_draws,
                         float frame_seconds, core::Extent2D extent,
                         render::Camera& camera);
  [[nodiscard]] ui::WeaponOverlayInfo MakeWeaponOverlayInfo() const;
  [[nodiscard]] ui::HitLogOverlayInfo MakeHitLogOverlayInfo() const;
  // Returns the reflex sight as posed this frame, nullopt outside the
  // player view.
  [[nodiscard]] std::optional<render::ReticleSight> CurrentSight() const;
  // Returns the scope eyepiece and picture of the weapon in hand, if it has
  // a scope (scope_state_).
  [[nodiscard]] std::optional<render::ScopeSight> CurrentScope() const;
  // Returns the frame to draw; `frame_seconds` is its duration (since the
  // previous frame), for the weapon motion blur.
  [[nodiscard]] render::SceneView BuildSceneView(float frame_seconds);

  // Declaration order is construction order and, reversed, destruction order:
  // the overlay and renderer must go before the window they render into; the
  // prefab cache borrows the GPU backend, the world borrows the cache;
  // world_physics_, dropped_weapons_ and player_ (which own bodies of the
  // physics world) go
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
  // The weapons the player threw away, with their bodies. Not part of the
  // scene: removed by every loadout reset (F9).
  weapon::DroppedWeapons dropped_weapons_;
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

  // The player's weapons. Every weapon the game has seen (the starting
  // weapons, the scene's pick-ups, every build of parts) has a definition in
  // the library: a weapon prefab keyed by its PrefabId, a build by its
  // parts; carried and dropped weapons refer to one. Bullets are not part of
  // the scene and are dropped on every scene reload.
  weapon::WeaponLibrary weapon_library_;  // Before the members using it.
  // The part catalogue (loaded at start; without one only the single-piece
  // weapons exist) and the definitions of builds; borrows the prefab cache.
  game::WeaponBuilds weapon_builds_;
  // The starting loadout's definitions; the last one starts in hand.
  std::vector<weapon::WeaponDefId> starting_weapons_;
  weapon::Loadout loadout_;
  // Indexed by weapon::WeaponDefId; one per definition of the library.
  std::vector<WeaponView> weapon_views_;
  weapon::ProjectileSystem projectiles_;
  // Spreads each shot by the accuracy of the weapon in hand.
  weapon::Dispersion dispersion_;
  // The scene's weapon pick-ups, found by every world build.
  game::ScenePickups scene_pickups_;
  // Last frame's weapon draws, for the weapon motion blur; invalidated
  // whenever the weapon jumps (respawn, a fresh viewmodel, a weapon reload)
  // or is not drawn.
  weapon::MotionHistory weapon_motion_;
  // The scope of the weapon in hand, at its chosen magnification; nullopt
  // without one.
  std::optional<weapon::ScopeZoom> scope_zoom_;
  // This frame's view through the scope of the weapon in hand
  // (UpdateScope); nullopt outside the player view and for weapons without
  // a scope.
  std::optional<weapon::ScopeViewState> scope_state_;
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

  // What the player can interact with, rebuilt every frame (reused).
  std::vector<interact::Interactable> interactables_;
  interact::InteractionFocus interaction_focus_;
  // The weapon inspect / modding screen (T): open or not, its session on
  // the weapon in hand (nullopt while closed or without a catalogue), what
  // the player clicked last frame (applied by ApplyInspectClicks) and why
  // the last change failed.
  bool inspect_open_ = false;
  std::optional<game::WeaponModding> modding_;
  ui::InspectScreenResult inspect_clicks_;
  std::string inspect_message_;
  // The player's loose parts; re-seeded by every loadout reset.
  game::PartStash part_stash_;

  // Opacity of the interaction dot, faded towards 1 while something has the
  // focus and towards 0 otherwise.
  float interaction_dot_opacity_ = 0.0f;
  // This frame's outlined draws (the focused item's); reused.
  std::vector<uint32_t> selected_draws_;
  // The focused scene weapon's instance, as a world selection; reused.
  std::vector<scene::InstanceId> scene_selection_;

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
