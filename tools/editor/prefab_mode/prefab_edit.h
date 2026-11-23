#ifndef BLUE_EDITOR_PREFAB_MODE_PREFAB_EDIT_H_
#define BLUE_EDITOR_PREFAB_MODE_PREFAB_EDIT_H_

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/mat4x4.hpp>

#include "animation/transform.h"
#include "asset/mesh_data.h"
#include "cook/cook_error.h"
#include "cook/model_cook.h"
#include "cook/texture_cook.h"
#include "core/result.h"
#include "prefab/prefab.h"

// Editing operations of the editor's Prefab mode that need neither a GPU nor
// ImGui, so they are unit-tested. Every function either succeeds completely
// or leaves the prefab unchanged (it returns an error or false), and keeps
// every index of the prefab consistent: ValidatePrefab() holds afterwards if
// it held before. Prefab::bounds is not updated implicitly; call
// UpdatePrefabBounds() after an edit that moves geometry.
//
// Re-import scope (ReimportPrefab): the fresh cook replaces geometry, the
// hierarchy, textures and the clip channels. Kept from the edited prefab,
// matched by name: material parameters and UV sets (by material name), clip
// loop flags and events (by clip name, events clamped to the new duration),
// sockets, lights and child prefabs (their parent node re-found by node
// name, the root when it is gone), and the physics settings (the caller
// re-cooks the shape). Clips the user renamed or merged keep their edits only
// if the fresh import yields a clip of the same name; others come back as
// imported.

namespace blue::editor {

// Returns `base` if no entry of `taken` equals it, otherwise `base` followed
// by " 2", " 3", ... (the first free one).
[[nodiscard]] std::string MakeUniqueName(std::string_view base,
                                         std::span<const std::string> taken);

// Returns the rest-pose model-space matrix of every node (parents first, as
// animation::ComputeWorldMatrices).
[[nodiscard]] std::vector<glm::mat4> RestWorldMatrices(
    const prefab::Prefab& prefab);

// Returns the rest-pose model-space matrix of `node` (identity for -1).
// `node` must be -1 or a valid node index.
[[nodiscard]] glm::mat4 NodeWorldMatrix(const prefab::Prefab& prefab, int node);

// Splits `matrix` (translation * rotation * positive scale, no shear) into a
// transform. Used to keep a node's or socket's placement when it moves to
// another parent.
[[nodiscard]] animation::Transform DecomposeMatrix(const glm::mat4& matrix);

// Recomputes prefab.bounds from every mesh's bounds at the rest pose (a
// point at the origin if there are no meshes).
void UpdatePrefabBounds(prefab::Prefab& prefab);

// Returns true if `node` is `ancestor` or lies in its subtree.
[[nodiscard]] bool IsInSubtree(const prefab::Prefab& prefab, int node,
                               int ancestor);

// Removes node `node` with its whole subtree, together with the meshes,
// sockets, lights and child prefabs attached to removed nodes and the clip
// channels animating them; every other node index is remapped. Returns false
// for a bad index.
[[nodiscard]] bool RemoveNode(prefab::Prefab& prefab, int node);

// Moves node `node` under `new_parent` (-1 = a root), keeping its rest pose
// in model space, and reorders the nodes so parents still precede their
// children (all indices remapped). Returns false for a bad index or when
// `new_parent` lies in the subtree of `node`.
[[nodiscard]] bool ReparentNode(prefab::Prefab& prefab, int node,
                                int new_parent);

// Assigns material `material` to submesh `submesh` of mesh `mesh`. Returns
// false for a bad index.
[[nodiscard]] bool SetSubmeshMaterial(prefab::Prefab& prefab, int mesh,
                                      int submesh, int material);

// Returns how many material slots reference texture `texture`.
[[nodiscard]] int CountTextureUses(const prefab::Prefab& prefab, int texture);

// Removes texture `texture`: slots that used it become untextured, later
// texture indices (and EditInfo::texture_sources) shift down. Returns false
// for a bad index.
[[nodiscard]] bool RemoveTexture(prefab::Prefab& prefab, int texture);

// Removes every texture no material slot references; returns how many.
int RemoveUnusedTextures(prefab::Prefab& prefab);

// Returns how a texture in `slot` is cooked: kColor for base colour and
// emissive, kLinear for ORM, kNormal for the normal map.
[[nodiscard]] cook::TextureUsage UsageOfSlot(prefab::TextureSlot slot);

// Settings for cooking a texture from a source file in the editor.
struct TextureImportSettings {
  cook::CookQuality quality = cook::CookQuality::kHigh;
  bool compress = true;
  int max_size = 0;  // > 0: halved until it fits
};

// Puts `texture` (already cooked into prefab.storage, see CookTexture) into
// slot `slot` of material `material`, named after `sources[0]` and recorded
// with `sources` in EditInfo::texture_sources. When the slot already holds a
// texture that no other slot uses, that texture is replaced in place;
// otherwise the texture is appended. Returns the texture's index, or -1 for
// a bad material index.
int AssignTexture(prefab::Prefab& prefab, int material,
                  prefab::TextureSlot slot, prefab::Texture texture,
                  std::vector<std::string> sources);

// Decodes the image at `path` and cooks it for `slot` into prefab.storage,
// then AssignTexture()s it. Returns the texture index. Fails with
// kTextureLoadFailed if the file cannot be decoded, or the cook's error.
[[nodiscard]] core::Result<int, cook::CookError> ImportSlotTexture(
    prefab::Prefab& prefab, int material, prefab::TextureSlot slot,
    const std::string& path, const TextureImportSettings& settings);

// One channel source of a packed ORM texture: an image file and the channel
// (0 = R ... 3 = A) read from it. An empty path means "no texture" (255).
struct OrmChannelSource {
  std::string path;
  int channel = 0;
};

// Sources of the ORM packing dialog.
struct OrmImportSources {
  OrmChannelSource occlusion;
  OrmChannelSource roughness;
  OrmChannelSource metallic;
};

// Loads the given images (each file once), packs them with cook::PackOrm,
// cooks the result as linear data into the ORM slot of `material` and returns
// the texture index. Fails with kInvalidArgument if every path is empty or a
// channel is outside 0..3, kTextureLoadFailed if a file cannot be decoded.
[[nodiscard]] core::Result<int, cook::CookError> ImportOrmTexture(
    prefab::Prefab& prefab, int material, const OrmImportSources& sources,
    const TextureImportSettings& settings);

// Sizes and counts shown in the Info panel.
struct PrefabSummary {
  int node_count = 0;
  int mesh_count = 0;
  int submesh_count = 0;
  size_t vertex_count = 0;
  size_t triangle_count = 0;
  int material_count = 0;
  int texture_count = 0;
  int clip_count = 0;
  int socket_count = 0;
  int light_count = 0;
  int child_count = 0;
  size_t vertex_bytes = 0;
  size_t index_bytes = 0;
  size_t texture_bytes = 0;  // all mips
  size_t collision_bytes = 0;
  size_t storage_bytes = 0;  // BulkStorage, including superseded blocks
};

[[nodiscard]] PrefabSummary Summarize(const prefab::Prefab& prefab);

// Returns the bytes of all mips of `texture`.
[[nodiscard]] size_t TextureByteSize(const prefab::Texture& texture);

// Returns a short label for `format`, e.g. "BC7 sRGB".
[[nodiscard]] const char* FormatLabel(prefab::TextureFormat format);

// Reads the child prefab paths of the .pbo at `path`; nullopt if it cannot
// be loaded. Injected so cycle checks are testable without files.
using ChildPathReader = std::optional<std::vector<std::string>> (*)(
    const std::string& path, void* user);

// Returns a readable description of the cycle that adding `child_path` as a
// child of the prefab saved at `owner_path` would create (e.g. "a.pbo ->
// b.pbo -> a.pbo"), or nullopt if there is none. Follows the children of
// `child_path` recursively through `read` (unreadable files end that branch)
// up to `max_depth` levels, which also counts as a problem when exceeded.
// Paths are compared after normalisation (separators, case on Windows).
[[nodiscard]] std::optional<std::string> FindChildCycle(
    const std::string& owner_path, const std::string& child_path,
    ChildPathReader read, void* user, int max_depth = 16);

// ChildPathReader that loads the .pbo with prefab::LoadPrefab.
std::optional<std::vector<std::string>> ReadChildPathsFromFile(
    const std::string& path, void* user);

// Returns `fresh` (a new cook of the edited prefab's source) with the edits
// of `edited` carried over as described at the top of this file.
[[nodiscard]] prefab::Prefab ReimportPrefab(const prefab::Prefab& edited,
                                            prefab::Prefab fresh);

// Reads the import options stored in prefab.edit.import_settings (defaults
// when there are none or they do not parse).
[[nodiscard]] cook::ModelCookOptions StoredImportOptions(
    const prefab::Prefab& prefab);

// Adds a socket named `base_name` (made unique) at the origin of `node` and
// returns its index. `node` must be -1 or a valid node.
int AddSocket(prefab::Prefab& prefab, int node,
              std::string_view base_name = "socket");

// Sets tag `key` of `prefab` to `value` (changes the value in place, or
// appends a new tag). Returns the problem instead for a key that is not 1 to
// prefab::kMaxTagKeyLength of [a-z0-9_.], a value longer than
// prefab::kMaxTagValueLength, or a new tag beyond prefab::kMaxTagCount.
[[nodiscard]] std::optional<std::string> SetPrefabTag(prefab::Prefab& prefab,
                                                      std::string_view key,
                                                      std::string_view value);

// Removes tag `key`; returns the problem instead if there is none.
[[nodiscard]] std::optional<std::string> RemovePrefabTag(prefab::Prefab& prefab,
                                                         std::string_view key);

// Adds a white point light (made-unique name "light", 10 m range) at the
// origin of `node` and returns its index.
int AddPointLight(prefab::Prefab& prefab, int node);

// Adds `child_path` as a child prefab at the origin of `node` after
// FindChildCycle(`owner_path`, ...) with ReadChildPathsFromFile; returns
// the problem instead when there is a cycle or `node` is invalid.
[[nodiscard]] std::optional<std::string> AddChildPrefab(
    prefab::Prefab& prefab, const std::string& owner_path,
    std::string child_path, int node);

// What MoveToNode() moves.
enum class AttachmentKind { kNode, kSocket, kLight, kChild };

// Attaches item `index` of `kind` to `node` (-1 = the root) keeping its
// model-space placement: nodes are reparented (ReparentNode), sockets,
// lights and child prefabs get a new parent and a compensated local
// transform. Returns false for a bad index or a node moved into its own
// subtree.
[[nodiscard]] bool MoveToNode(prefab::Prefab& prefab, AttachmentKind kind,
                              int index, int node);

// Returns a readable reason why `prefab` cannot be saved, or nullopt.
// prefab::ValidatePrefab() plus editor checks: duplicate socket or clip
// names, a dynamic body with mass <= 0, a mesh collider without a cooked
// shape.
[[nodiscard]] std::optional<std::string> CheckBeforeSave(
    const prefab::Prefab& prefab);

}  // namespace blue::editor

#endif  // BLUE_EDITOR_PREFAB_MODE_PREFAB_EDIT_H_
