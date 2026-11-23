#ifndef BLUE_COOK_PREFAB_EDITS_H_
#define BLUE_COOK_PREFAB_EDITS_H_

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <glm/vec3.hpp>

#include "core/result.h"
#include "prefab/prefab.h"

// Changes to a cooked prefab that need no re-import: tags, sockets, clip
// events and clip names. Used by `blue-pack edit` (and `blue-pack import`
// for its --tag / --socket options); kept here so it can be unit tested.
//
// Command-line syntax, shared by both commands:
//   --tag <key>=<value>               set (add or replace) a tag
//   --remove-tag <key>                remove a tag
//   --socket <name>=<node>@<x>,<y>,<z>[/<dx>,<dy>,<dz>]
//                                     add or replace a socket; node '-' is
//                                     the prefab root; the optional
//                                     direction is where the socket's +Z
//                                     points (default: the node's +Z)
//   --remove-socket <name>            remove a socket
//   --event <clip>=<name>@<time>      add an event (seconds)
//   --rename-clip <old>=<new>         rename a clip
// Names (sockets, nodes, clips) match exactly.

namespace blue::cook {

// One change to a prefab. Which fields are used depends on `kind`.
struct PrefabEdit {
  enum class Kind {
    kSetTag,        // tag `target` = `text`
    kRemoveTag,     // tag `target`
    kSetSocket,     // socket `target` on node `text` at `position` / `aim`
    kRemoveSocket,  // socket `target`
    kAddEvent,      // event `text` at `time` in clip `target`
    kRenameClip,    // clip `target` renamed to `text`
  };

  Kind kind = Kind::kSetTag;
  std::string target;  // tag key, socket name or clip name
  std::string text;    // tag value, socket node, event name or new clip name
  glm::vec3 position{0.0f};      // socket translation, in the node's space
  std::optional<glm::vec3> aim;  // socket +Z direction, any length > 0
  float time = 0.0f;             // event time, seconds
};

// Returns true if `option` (e.g. "--tag") names a PrefabEdit.
[[nodiscard]] bool IsPrefabEditOption(std::string_view option);

// Parses `option` with its `value` (e.g. "--socket", "sight=Slide@0,1,2").
// Returns an error message naming the expected syntax on failure.
[[nodiscard]] core::Result<PrefabEdit, std::string> ParsePrefabEdit(
    std::string_view option, std::string_view value);

// Applies `edit` to `prefab`. Fails, leaving the prefab unchanged, with a
// message for: an invalid tag key or too long a value, a missing tag,
// socket, node or clip, a degenerate socket direction, an event time outside
// the clip, an empty name.
[[nodiscard]] core::Result<void, std::string> ApplyPrefabEdit(
    prefab::Prefab& prefab, const PrefabEdit& edit);

// Applies `edits` in order to a copy of `prefab` and returns it, or the
// first failure (prefixed with the failing edit's number, from 1) with
// `prefab` untouched.
[[nodiscard]] core::Result<prefab::Prefab, std::string> ApplyPrefabEdits(
    const prefab::Prefab& prefab, std::span<const PrefabEdit> edits);

// Loads the .pbo at `input`, applies `edits` and saves the result to
// `output` (which may be `input`). The file is replaced atomically (written
// to a temporary file, then renamed), so a failure at any step leaves both
// files as they were. Everything the edits do not touch keeps its meaning:
// meshes, textures, clips, collision, import settings and source paths
// (re-expressed relative to `output`'s directory).
[[nodiscard]] core::Result<void, std::string> EditPrefabFile(
    const std::string& input, const std::string& output,
    std::span<const PrefabEdit> edits);

}  // namespace blue::cook

#endif  // BLUE_COOK_PREFAB_EDITS_H_
