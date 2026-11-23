#ifndef BLUE_COOK_PREFAB_SPLIT_H_
#define BLUE_COOK_PREFAB_SPLIT_H_

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/ext/quaternion_float.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "animation/transform.h"
#include "asset/mesh_data.h"
#include "cook/prefab_edits.h"
#include "core/result.h"
#include "prefab/prefab.h"

// Cutting a weapon part (or any sub-object) out of a cooked prefab: used by
// `blue-pack split` and the sample cook; kept here so it can be unit tested.
//
// Command-line syntax (`blue-pack split <in.pbo> -o <out.pbo> <options>`):
//   --nodes A,B,...        the nodes whose meshes the part keeps, with their
//                          descendants (required)
//   --exclude C,D,...      nodes (and their descendants) left out even when
//                          below a chosen node, unless chosen themselves
//   --origin <node>|x,y,z  the point that becomes the part's origin: a
//                          node's rest position or a point, in the input's
//                          prefab space (default: the first chosen node;
//                          with --keep-skeleton the input's origin)
//   --forward <axis>       +x|-x|+z|-z, the input's bore (forward) direction;
//                          the part is turned about +Y so it points along +Z
//                          (default +z: not turned)
//   --keep-skeleton        keep the whole hierarchy, the clips and their
//                          events (for the root / frame part)
// plus every PrefabEdit option (--tag, --socket, ...), applied to the result.
//
// The part frame: weapon parts share one convention so parts cut from
// different models fit each other's slots: +Y up, bore along +Z, origin at
// a meaningful point of the part (e.g. the slide's rear face on the bore
// axis). A slot socket on the parent is placed at the same point with the
// parent's axes; the child's origin goes onto it.

namespace blue::cook {

struct SplitOptions {
  std::vector<std::string> nodes;
  std::vector<std::string> exclude;
  // At most one of the two; neither = the default described above.
  std::optional<std::string> origin_node;
  std::optional<glm::vec3> origin_point;
  // The input's forward direction, one of (+-1, 0, 0) or (0, 0, +-1).
  glm::vec3 forward{0.0f, 0.0f, 1.0f};
  bool keep_skeleton = false;
};

// Returns true if `option` (e.g. "--nodes") is a split option that takes a
// value; "--keep-skeleton" takes none (see ApplySplitFlag()).
[[nodiscard]] bool IsSplitOption(std::string_view option);

// Parses `option` with its `value` into `options`. Returns an error message
// naming the expected syntax on failure.
[[nodiscard]] core::Result<void, std::string> ParseSplitOption(
    SplitOptions& options, std::string_view option, std::string_view value);

// Applies a value-less split flag ("--keep-skeleton"). Returns false if
// `option` is not one.
bool ApplySplitFlag(SplitOptions& options, std::string_view option);

// Returns the rest transform of every node of `prefab` in prefab space.
[[nodiscard]] std::vector<glm::mat4> RestWorldMatrices(
    const prefab::Prefab& prefab);

// Returns the box enclosing mesh `mesh` of `prefab` at the rest pose, in
// prefab space (its node-space bounds' corners transformed).
[[nodiscard]] asset::Bounds MeshBoundsInPrefabSpace(
    const prefab::Prefab& prefab, int mesh);

// Splits `matrix` into translation, rotation and scale. Returns nullopt if
// it has shear, a mirror or a zero scale (no Transform reproduces it).
[[nodiscard]] std::optional<animation::Transform> DecomposeMatrix(
    const glm::mat4& matrix);

// Returns a socket called `name` on node `node` (-1 = root) whose transform
// in prefab space is `frame` (scale 1): the node's rest transform, including
// any scale, is compensated in the socket's local transform. Fails if the
// node is out of range or its rest transform has shear.
[[nodiscard]] core::Result<prefab::Socket, std::string> SocketAtPrefabFrame(
    const prefab::Prefab& prefab, std::string name, int node,
    const animation::Transform& frame);

// Makes a part out of `source`:
//   - the meshes of the chosen nodes (and of their descendants, minus
//     excluded subtrees), with only the materials and textures they use;
//   - expressed relative to the origin and turned so `forward` becomes +Z;
//   - without --keep-skeleton: only the kept nodes (a node whose parent is
//     not kept becomes a root placed at its former rest transform), the
//     sockets, lights and child prefabs on kept nodes, no clips;
//   - with --keep-skeleton: every node, clip, event, socket, light and child
//     prefab (root nodes, their animation channels and root-attached items
//     re-expressed), only the chosen meshes; other nodes stay as empty
//     (possibly animated) nodes that can carry slot sockets;
//   - no tags (a part gets its own, e.g. part = <id>), no physics body (the
//     whole model's collider does not fit a part), no source model or import
//     settings (re-importing would bring the whole model back); texture
//     sources are kept; bounds recomputed.
// Fails with a message for an unknown or missing node, no meshes left, an
// unknown origin node or a re-rooted node with shear.
[[nodiscard]] core::Result<prefab::Prefab, std::string> SplitPrefab(
    const prefab::Prefab& source, const SplitOptions& options);

// Loads the .pbo at `input`, splits it, applies `edits` (in order, to the
// part) and saves the part to `output`, atomically (temporary file, then
// rename). Never modifies `input`: an `output` naming the same file is an
// error.
[[nodiscard]] core::Result<void, std::string> SplitPrefabFile(
    const std::string& input, const std::string& output,
    const SplitOptions& options, std::span<const PrefabEdit> edits);

}  // namespace blue::cook

#endif  // BLUE_COOK_PREFAB_SPLIT_H_
