#ifndef BLUE_SCENE_SCENE_DATA_H_
#define BLUE_SCENE_SCENE_DATA_H_

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "scene/scene.h"

// The contents of a .mscn scene: placed prefab instances with per-instance
// overrides, plus the scene-wide lights, environment, player start and
// gravity. CPU data only; the runtime (blue_world) and the editor build their
// own state from it. See mscn_serializer.h for the file format.

namespace blue::scene {

// Identifies an instance within one scene. Ids are assigned by AddInstance()
// and DuplicateInstance() from SceneData::next_instance_id, never reused in
// that scene (also not after a removal) and stored in the file, so undo/redo
// snapshots and editor selections can refer to instances across edits, saves
// and loads.
using InstanceId = uint32_t;

// The "no instance" id: no instance ever has it.
inline constexpr InstanceId kNoInstance = 0;

// Index into SceneData::prefabs, or kNoPrefab.
inline constexpr int kNoPrefab = -1;

// Replaces or adjusts one material of an instance's prefab, matched by the
// material's name. Optionally the material is first replaced by
// `source_material` taken from the prefab `source_prefab` (a material
// library .pbo); then every set parameter is applied on top. An override with
// neither is valid but has no effect.
struct MaterialOverride {
  // Name of the material in the instance's prefab (and its nested prefabs)
  // that is overridden. Not empty; unique within one instance.
  std::string material_name;
  // Index into SceneData::prefabs of the prefab that provides the
  // replacement, or kNoPrefab for a parameter-only override.
  int source_prefab = kNoPrefab;
  // Name of the replacement material in `source_prefab`; empty exactly when
  // `source_prefab` is kNoPrefab.
  std::string source_material;
  // Parameter tweaks (meaning and ranges as in material::MaterialDesc).
  std::optional<glm::vec4> base_color;     // linear RGBA, each in [0, 1]
  std::optional<float> metallic;           // [0, 1]
  std::optional<float> roughness;          // [0, 1]
  std::optional<glm::vec3> emissive;       // linear, >= 0
  std::optional<float> emissive_strength;  // >= 0
  std::optional<glm::vec2> uv_scale;       // > 0

  friend bool operator==(const MaterialOverride&,
                         const MaterialOverride&) = default;
};

// Replaces parts of the physics settings of an instance's prefab. Unset
// fields keep the prefab's values.
struct PhysicsOverride {
  // No body is created for the instance (and its nested prefabs).
  bool disabled = false;
  std::optional<MotionType> motion;
  std::optional<float> mass;         // kg, > 0
  std::optional<float> friction;     // >= 0
  std::optional<float> restitution;  // [0, 1]

  friend bool operator==(const PhysicsOverride&,
                         const PhysicsOverride&) = default;
};

// One placed prefab, or a group node without a prefab.
//
// Instances can be grouped: `parent` names another instance and `transform`
// is then relative to it. Groups are kept in the scene (rather than flattened
// by the editor) because moving, duplicating or hiding a set of objects is a
// common editing operation and the runtime resolves the hierarchy once at
// load. Parents form a forest (no cycles); the order of `SceneData::instances`
// is free (a child may precede its parent).
struct Instance {
  InstanceId id = kNoInstance;
  std::string name;  // Display name; need not be unique.
  // Index into SceneData::prefabs, or kNoPrefab for a pure group node.
  int prefab = kNoPrefab;
  InstanceId parent = kNoInstance;  // kNoInstance = at the scene root.
  Transform transform;              // Relative to the parent.
  // Hidden instances (and their descendants) are neither drawn nor
  // simulated by the game; the editor still lists them.
  bool visible = true;
  std::vector<MaterialOverride> materials;
  std::optional<PhysicsOverride> physics;

  // Field-wise equality; floats compare with ==, so NaN never equals.
  friend bool operator==(const Instance& a, const Instance& b);
};

// A point light placed in the scene (in addition to the prefabs' lights).
struct ScenePointLight {
  std::string name;
  glm::vec3 position{0.0f};
  glm::vec3 color{1.0f};    // linear RGB, >= 0
  float intensity = 10.0f;  // >= 0
  float range = 10.0f;      // m, > 0; the light fades out to zero at range.

  friend bool operator==(const ScenePointLight&,
                         const ScenePointLight&) = default;
};

// Where and how the player spawns. Yaw rotates about +Y (0 looks along -Z),
// pitch about the camera's right axis (positive looks up).
struct PlayerStart {
  glm::vec3 position{0.0f};
  float yaw_radians = 0.0f;
  float pitch_radians = 0.0f;

  friend bool operator==(const PlayerStart&, const PlayerStart&) = default;
};

// A whole scene.
struct SceneData {
  // Paths of the referenced .pbo files: the prefabs of instances and the
  // sources of material overrides. Absolute (or relative to the working
  // directory) in memory, stored relative to the scene file. No duplicates;
  // AddPrefab() keeps it that way and CompactPrefabs() drops unused entries.
  std::vector<std::string> prefabs;
  std::vector<Instance> instances;
  SunLight sun;
  std::vector<ScenePointLight> point_lights;
  Environment environment;
  PlayerStart player_start;
  glm::vec3 gravity{0.0f, -9.81f, 0.0f};  // m/s^2
  // The id the next new instance receives; greater than every instance id.
  InstanceId next_instance_id = 1;

  // Field-wise equality (including the order of every list); floats
  // compare with ==.
  friend bool operator==(const SceneData& a, const SceneData& b);
};

// Returns the index of `path` in `scene.prefabs`, appending it if it is not
// there yet. Paths compare after core::NormalizePath(), so "a/./b.pbo" and
// "a/b.pbo" are the same entry.
int AddPrefab(SceneData& scene, std::string_view path);

// Adds `instance` to `scene` with a fresh id, which is returned. The incoming
// `instance.id` is ignored. Returns kNoInstance and leaves `scene` unchanged
// if `instance.prefab` is neither kNoPrefab nor a valid index or
// `instance.parent` names a missing instance.
InstanceId AddInstance(SceneData& scene, Instance instance);

// Removes the instance `id` and all its descendants, then compacts the
// prefab table (see CompactPrefabs()). Returns false if no instance has `id`.
bool RemoveInstance(SceneData& scene, InstanceId id);

// Copies the instance `id` and all its descendants with fresh ids; the copy
// of `id` gets the same parent and name and is appended after the existing
// instances. Returns the copy's id, or kNoInstance if no instance has `id`.
InstanceId DuplicateInstance(SceneData& scene, InstanceId id);

// Removes the prefab table entries that no instance and no material override
// refers to, keeping the order of the others, and renumbers the references.
void CompactPrefabs(SceneData& scene);

// Returns the instance with `id`, or nullptr. The pointer is invalidated by
// any change to `scene.instances`.
Instance* FindInstance(SceneData& scene, InstanceId id);
const Instance* FindInstance(const SceneData& scene, InstanceId id);

// Returns the world matrix of the instance `id` (its transform composed with
// its ancestors'), or std::nullopt if no instance has `id` or the parent
// chain is broken (missing parent or a cycle).
std::optional<glm::mat4> WorldMatrix(const SceneData& scene, InstanceId id);

// Returns the override of `instance` for its prefab's material
// `material_name`, or nullptr if the material is not overridden. Points into
// `instance`.
const MaterialOverride* FindMaterialOverride(const Instance& instance,
                                             std::string_view material_name);

// Returns the override of the instance `id` for `material_name`, or nullptr
// if the instance does not exist or the material is not overridden.
const MaterialOverride* FindMaterialOverride(const SceneData& scene,
                                             InstanceId id,
                                             std::string_view material_name);

// Returns the ids of the direct children of `parent` (kNoInstance = the root
// instances), in the order of `scene.instances`.
std::vector<InstanceId> ChildrenOf(const SceneData& scene, InstanceId parent);

// Returns true if `ancestor` is `id` itself or one of its ancestors.
bool IsSelfOrAncestor(const SceneData& scene, InstanceId ancestor,
                      InstanceId id);

// Returns the transform whose matrix best matches `matrix`: translation,
// rotation and (signed) per-axis scale; shear is dropped. Used to turn world
// matrices back into instance transforms.
Transform TransformFromMatrix(const glm::mat4& matrix);

// Moves the instance `id` under `new_parent` (kNoInstance = scene root). With
// `keep_world_transform` its local transform is recomputed so it does not
// move in the world (exact unless the new parent chain has non-uniform scale
// combined with rotation). Returns false and changes nothing if `id` does not
// exist, `new_parent` does not exist, or `new_parent` is `id` or one of its
// descendants.
bool ReparentInstance(SceneData& scene, InstanceId id, InstanceId new_parent,
                      bool keep_world_transform);

// Creates a group node (no prefab) named `name` and moves the instances `ids`
// into it, keeping their world transforms. Ids that are missing or that are
// descendants of another listed id are ignored. The group is placed under the
// first remaining instance's parent if all remaining instances share that
// parent, otherwise at the root; its position is the centroid of their world
// positions, with no rotation and unit scale. Returns the group's id, or
// kNoInstance (scene unchanged) if no instance remains.
InstanceId GroupInstances(SceneData& scene, std::span<const InstanceId> ids,
                          std::string name);

// Moves the children of `group` to the group's parent, keeping their world
// transforms, and removes `group` if it has no prefab (a pure group node).
// Returns false if `group` does not exist.
bool UngroupInstance(SceneData& scene, InstanceId group);

// The sun's direction as angles: `elevation` above the horizon (radians,
// positive = sun above the horizon, i.e. light travelling downwards) and
// `azimuth` about +Y of the direction towards the sun, measured from +Z
// towards +X (radians).
struct SunAngles {
  float elevation = 0.0f;
  float azimuth = 0.0f;
};

// Returns the angles of the light travel direction `direction` (need not be
// normalized; must not be zero).
SunAngles SunAnglesFromDirection(const glm::vec3& direction);

// Returns the normalized light travel direction for `angles` (the inverse of
// SunAnglesFromDirection()).
glm::vec3 SunDirectionFromAngles(const SunAngles& angles);

// A problem found by ValidateAgainstPrefabs(); not fatal: the runtime ignores
// the affected override.
struct SceneIssue {
  InstanceId instance = kNoInstance;
  std::string message;  // Human-readable, e.g. for the editor's log.
};

// Checks the material overrides of every instance against the prefabs'
// material names: `material_names[i]` lists the materials of
// `scene.prefabs[i]` (for an instance's prefab including those of its nested
// prefabs). Reports overrides of materials the instance's prefab does not
// have and replacement materials missing from their source prefab. Instances
// and overrides whose prefab index has no entry in `material_names` are not
// checked.
std::vector<SceneIssue> ValidateAgainstPrefabs(
    const SceneData& scene,
    std::span<const std::vector<std::string>> material_names);

// Returns a readable description of the first structural error of `scene`,
// or std::nullopt if it is consistent: instance ids non-zero, unique and
// below next_instance_id; prefab indices valid; parents existing and acyclic;
// every number finite and in its documented range; override material names
// non-empty and unique per instance and source fields consistent; prefab
// paths non-empty and unique; sun and environment valid (ValidateSunLight(),
// ValidateEnvironment()). SaveMscn() refuses scenes that fail this check.
std::optional<std::string> ValidateSceneData(const SceneData& scene);

}  // namespace blue::scene

#endif  // BLUE_SCENE_SCENE_DATA_H_
