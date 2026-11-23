#ifndef BLUE_WEAPON_WEAPON_DEF_H_
#define BLUE_WEAPON_WEAPON_DEF_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/mat4x4.hpp>

#include "animation/clip.h"
#include "animation/hierarchy.h"
#include "asset/mesh_data.h"
#include "core/result.h"
#include "prefab/prefab.h"
#include "weapon/firearm.h"
#include "weapon/parts/part_catalog.h"
#include "weapon/parts/weapon_build.h"
#include "weapon/parts/weapon_stats.h"
#include "weapon/parts/weapon_tuning.h"
#include "weapon/scope.h"
#include "weapon/viewmodel.h"
#include "weapon/weapon_animator.h"
#include "weapon/weapon_geometry.h"
#include "weapon/weapon_item.h"

// Weapon definitions: what makes a prefab a weapon the game can carry.
//
// A prefab is a weapon when it carries the tag `weapon = <type id>`. The
// type id names one of the weapon types the code defines (FindWeaponType():
// the firearm spec, the viewmodel feel, the aim zoom, a scope); the prefab
// supplies the model, its calibration (sockets "muzzle", "sight", "grip",
// optional "lens") and, optionally, its fire animation (clip "fire" with
// events "shot" and "slide_lock"). Socket, clip and event names match
// ignoring case. A weapon without a fire clip is static (drawn at rest). For
// a scoped weapon "sight" is the centre of the eyepiece's rear face with +Z
// along the scope's axis (downrange), and the optional "eyepiece" the
// centre of the eyepiece glass, where the scope's picture is drawn.
//
// A weapon built from parts (weapon/parts/) gets its definition from the
// part catalogue and the part prefabs instead (weapon/parts/
// weapon_assembly.h); both kinds share WeaponDef and the WeaponLibrary.

namespace blue::weapon {

// The tag that marks a weapon prefab; its value is a weapon type id.
inline constexpr std::string_view kWeaponTag = "weapon";
// The clip a weapon's fire animation is read from.
inline constexpr std::string_view kFireClip = "fire";

// What the code defines for one weapon type.
struct WeaponType {
  std::string_view id;  // the `weapon` tag value, e.g. "glock17"
  FirearmSpec spec;
  ViewmodelTuning viewmodel;
  // Fully aimed vertical FOV as a fraction of the base one; nullopt keeps
  // the player's default. A scoped weapon uses it while the scope comes
  // up, until the scope view takes over (see scope.h).
  std::optional<float> aim_fov_scale;
  // The weapon's magnifying scope; nullopt for iron sights or a red dot.
  std::optional<ScopeSpec> scope;
};

// Returns the weapon type `id` ("glock17", "m16", "spr_a3g"), or nullopt.
[[nodiscard]] std::optional<WeaponType> FindWeaponType(std::string_view id);

// Returns every known type id, for messages.
[[nodiscard]] std::span<const std::string_view> WeaponTypeIds();

// A weapon's fire animation, copied out of its prefab so animators can
// borrow it for the definition's life.
struct FireAnimation {
  animation::Hierarchy hierarchy;
  animation::Clip clip;
  WeaponAnimatorTuning tuning;  // slide_node keeps its default
};

// Returns the fire animation of `prefab` (clip kFireClip): nullopt without
// that clip, an error when the clip lacks one of its events.
[[nodiscard]] core::Result<std::optional<FireAnimation>, std::string>
ReadFireAnimation(const prefab::Prefab& prefab);

// One model of a weapon: the root (the whole model of a single-piece weapon)
// or an installed part, hung on its parent's slot socket. Matrices map the
// part's prefab space; prefabs are named by the caller's handle (as
// WeaponLibrary keys are: the value of a world::PrefabId).
struct PartPlacement {
  parts::PartId part;  // invalid for a single-piece weapon's model
  // The part's prefab; nullopt for a part without a model (nothing drawn).
  std::optional<uint32_t> prefab_key;
  int parent = -1;       // index into WeaponDef::parts; -1 for the root
  int parent_node = -1;  // node of the parent's prefab carrying the socket
  // Part prefab space -> parent_node's space: the parent's slot socket
  // times the inverse of the part's `mount` socket (identity without one).
  glm::mat4 local{1.0f};
  // For drawing: the root prefab's node the part moves with (-1: the root's
  // origin) and the part in that node's space. A part on a part on an
  // animated node follows that node too.
  int root_node = -1;
  glm::mat4 root_local{1.0f};
  // Part prefab space -> root prefab space at rest.
  glm::mat4 rest{1.0f};
};

// Everything the game needs to carry, draw, aim, fire, animate and drop
// one weapon: a weapon prefab, or a build of parts.
struct WeaponDef {
  WeaponType type;
  WeaponGeometry geometry;  // in the root prefab's space, at rest
  asset::Bounds bounds;     // every part at rest, for a dropped weapon's body
  // The root's (parts[0]'s) fire animation; nullopt: a static weapon.
  std::optional<FireAnimation> fire;
  // parts[0] is the root; parents come before their children.
  std::vector<PartPlacement> parts;
  // The parts it is made of; empty for a single-piece weapon from a prefab
  // the part catalogue does not describe.
  parts::WeaponBuild build;
  // The build's stats; nullopt without a build.
  std::optional<parts::WeaponStats> stats;
  // Storage of type.spec.display_name for a part-built weapon (a prefab's
  // name is a literal); see WeaponLibrary::AddBuild.
  std::string name;
  // Shot dispersion (weapon/dispersion.h); 0: none.
  float accuracy_moa = 0.0f;
  // How far the zero keys can move the zero of a weapon without a scope;
  // nullopt: they do nothing (a scope has its own range).
  std::optional<parts::ZeroRange> zero_range;
  float switch_time_scale = 1.0f;  // WeaponItem::switch_time_scale
  // Overlay text: parts::StatsSummary() of the stats and
  // parts::MissingSummary() (empty for a functional weapon).
  std::string stats_text;
  std::string missing_text;
};

// Returns the definition built from `prefab`: nullopt (not an error) when
// it has no `weapon` tag; an error naming the problem for an unknown type
// id, a missing socket, a degenerate sight, or a fire clip without one of
// its events.
[[nodiscard]] core::Result<std::optional<WeaponDef>, std::string> MakeWeaponDef(
    const prefab::Prefab& prefab);

// Returns a fresh weapon of definition `id`: a full magazine, a round
// chambered and the spec's spare magazines (Firearm's starting state), its
// scope (if any) at the start magnification and zero, a zero range's zero at
// the tuning's, the definition's build and switch time scale. The one policy
// for weapons that enter the game new (starting loadout, scene pick-ups).
[[nodiscard]] WeaponItem MakeFreshWeapon(WeaponDefId id, const WeaponDef& def);

// Makes `item` a weapon of definition `id` after its parts changed (the
// definition of its new build): the build, the firearm refitted to the new
// spec (Firearm::Refit), the switch time scale, and the zero clamped to the
// new zero range (or the scope's); the tuning's zero when the old weapon had
// none. Allocates nothing beyond copying the build.
void RefitWeapon(WeaponItem& item, WeaponDefId id, const WeaponDef& def);

// The weapon definitions of the game: single-piece weapons keyed by prefab
// (the caller's prefab handle, the value of a world::PrefabId, is the key)
// and part-built weapons keyed by their build and root model. Weapons with
// equal builds share one definition; a weapon whose parts change gets the
// definition of its new build. Definitions are added on first sight and
// never removed, so a WeaponDefId and the address of its WeaponDef (and of
// what an animator borrows from it) stay valid for the library's life;
// Rebuild() and ReplaceBuild() replace a definition's contents in place.
//
// Allocates when a prefab or build is seen for the first time and on
// rebuilds, not per frame. Neither copyable nor movable. Not thread-safe.
class WeaponLibrary {
 public:
  WeaponLibrary() = default;
  WeaponLibrary(const WeaponLibrary&) = delete;
  WeaponLibrary& operator=(const WeaponLibrary&) = delete;

  // Returns the definition of prefab `key`, building it from `prefab` the
  // first time the key is seen. Nullopt for a prefab that is not a weapon:
  // untagged (silently) or failing MakeWeaponDef() (logged with `name`, once
  // per key). Later calls with the same key do not look at `prefab`.
  std::optional<WeaponDefId> Register(uint32_t key,
                                      const prefab::Prefab& prefab,
                                      std::string_view name);

  // Rebuilds the definition of prefab `key` after the prefab changed. A
  // known definition that fails to rebuild (or lost its tag) keeps its
  // previous contents (logged); a key that was not a weapon is looked at
  // afresh. Returns the definition if one exists afterwards. Animators that
  // borrow the rebuilt definition's fire animation must be rebuilt too.
  std::optional<WeaponDefId> Rebuild(uint32_t key, const prefab::Prefab& prefab,
                                     std::string_view name);

  // Returns the definition of prefab `key`, or nullopt.
  [[nodiscard]] std::optional<WeaponDefId> Find(uint32_t key) const;

  // Adds `def`, made from a build (weapon/parts/weapon_assembly.h), and
  // returns its id; type.spec.display_name is pointed at the stored def's
  // `name` when that is not empty. Does not look for an equal build
  // (FindBuild does).
  WeaponDefId AddBuild(WeaponDef def);
  // Returns the definition made from `build` with root model `root_key`
  // (its parts[0].prefab_key), or nullopt.
  [[nodiscard]] std::optional<WeaponDefId> FindBuild(
      const parts::WeaponBuild& build, uint32_t root_key) const;
  // Replaces the contents of build definition `id` (e.g. after its part
  // prefabs were reloaded), like AddBuild. Animators that borrow its fire
  // animation must be rebuilt.
  void ReplaceBuild(WeaponDefId id, WeaponDef def);
  // True for a definition added by AddBuild.
  [[nodiscard]] bool is_build(WeaponDefId id) const {
    return !entries_[static_cast<size_t>(id)].key.has_value();
  }

  // Returns the first definition of type `type_id`, or nullopt.
  [[nodiscard]] std::optional<WeaponDefId> FindType(
      std::string_view type_id) const;

  // `id` must come from this library.
  [[nodiscard]] const WeaponDef& def(WeaponDefId id) const {
    return *entries_[static_cast<size_t>(id)].def;
  }
  // The prefab of the root model (parts[0]) of definition `id`.
  [[nodiscard]] uint32_t prefab_key(WeaponDefId id) const;
  [[nodiscard]] size_t size() const { return entries_.size(); }

 private:
  struct Entry {
    std::optional<uint32_t> key;     // the prefab; nullopt for a build
    std::unique_ptr<WeaponDef> def;  // never null; stable address
  };

  std::vector<Entry> entries_;      // indexed by WeaponDefId
  std::vector<uint32_t> rejected_;  // keys seen that are not weapons
};

}  // namespace blue::weapon

#endif  // BLUE_WEAPON_WEAPON_DEF_H_
