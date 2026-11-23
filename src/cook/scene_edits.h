#ifndef BLUE_COOK_SCENE_EDITS_H_
#define BLUE_COOK_SCENE_EDITS_H_

#include <span>
#include <string>
#include <string_view>

#include <glm/vec3.hpp>

#include "core/result.h"
#include "scene/scene_data.h"

// Headless changes to a .mscn scene, used by `blue-pack edit <file.mscn>`;
// kept here so they can be unit tested.
//
// Command-line syntax:
//   --add-instance <name>=<prefab.pbo>@<x>,<y>,<z>[/<yaw degrees>]
//                         add a root-level instance of the prefab at the
//                         position, turned by yaw about +Y (0 looks along
//                         -Z), scale 1; the prefab joins the scene's prefab
//                         list if it is not there yet
//   --remove-instance <name>
//                         remove the instance and its descendants
// Instance names match exactly. A scene may hold several instances of one
// name (the editor allows it), but these edits refuse to make or act on
// ambiguous names: --add-instance fails if any instance already has the name,
// --remove-instance fails unless exactly one has it.

namespace blue::cook {

// One change to a scene. Which fields are used depends on `kind`.
struct SceneEdit {
  enum class Kind {
    kAddInstance,     // instance `name` of `prefab_path` at `position` / yaw
    kRemoveInstance,  // instance `name`
  };

  Kind kind = Kind::kAddInstance;
  std::string name;
  // As given; EditSceneFile() makes it absolute against the working
  // directory (the form the editor keeps in memory) before applying.
  std::string prefab_path;
  glm::vec3 position{0.0f};
  float yaw_degrees = 0.0f;
};

// Returns true if `option` (e.g. "--add-instance") names a SceneEdit.
[[nodiscard]] bool IsSceneEditOption(std::string_view option);

// Parses `option` with its `value`. The prefab path ends at the last '@', so
// it may contain '@' itself. Returns an error message naming the expected
// syntax on failure.
[[nodiscard]] core::Result<SceneEdit, std::string> ParseSceneEdit(
    std::string_view option, std::string_view value);

// Applies `edit` to `scene`. Removing an instance also drops prefab list
// entries nothing refers to any more (scene::RemoveInstance()). Fails,
// leaving the scene unchanged, with a message for: an empty name or prefab
// path, a duplicate name (add), no or several instances of the name
// (remove).
[[nodiscard]] core::Result<void, std::string> ApplySceneEdit(
    scene::SceneData& scene, const SceneEdit& edit);

// Applies `edits` in order to a copy of `scene` and returns it, or the first
// failure (prefixed with the failing edit's number, from 1) with `scene`
// untouched.
[[nodiscard]] core::Result<scene::SceneData, std::string> ApplySceneEdits(
    const scene::SceneData& scene, std::span<const SceneEdit> edits);

// Loads the .mscn at `input`, applies `edits` and saves the result to
// `output` (which may be `input`), replacing it atomically: on any failure
// both files are left as they were. Prefab paths of added instances are
// resolved against the working directory and must name existing files.
// Everything the edits do not touch is kept: instances (ids included),
// overrides, lights, environment, player start, gravity.
[[nodiscard]] core::Result<void, std::string> EditSceneFile(
    const std::string& input, const std::string& output,
    std::span<const SceneEdit> edits);

}  // namespace blue::cook

#endif  // BLUE_COOK_SCENE_EDITS_H_
