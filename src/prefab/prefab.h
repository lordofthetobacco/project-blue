#ifndef BLUE_PREFAB_PREFAB_H_
#define BLUE_PREFAB_PREFAB_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <glm/ext/quaternion_float.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "animation/clip.h"
#include "animation/hierarchy.h"
#include "animation/transform.h"
#include "asset/mesh_data.h"
#include "material/material.h"

// The in-memory form of a prefab (.pbo): a self-contained object made of a
// node hierarchy, meshes, materials, GPU-ready textures, animation clips,
// sockets, point lights, nested prefab references and a physics body.
//
// Small metadata lives in ordinary vectors and strings. Bulk data (vertices,
// indices, texture mips, cooked collision) is viewed through spans into
// immutable byte blocks owned by Prefab::storage. A loaded prefab has a single
// block, the file itself mapped read-only (see LoadPrefab), so nothing is
// copied or parsed per element and the bulk costs no private memory; a cooked
// prefab has one heap block per array the cook stored. Copying a Prefab copies
// the metadata and shares the blocks, which is cheap (editor undo snapshots).
//
// Filling a prefab (cook, editor):
//
//   Prefab prefab;
//   Mesh mesh;
//   mesh.vertices = prefab.storage.StoreArray<asset::Vertex>(vertices);
//   mesh.indices = prefab.storage.StoreArray<asset::Index>(indices);
//   prefab.meshes.push_back(std::move(mesh));
//   if (auto problem = ValidatePrefab(prefab)) { ... }

namespace blue::prefab {

// Alignment of every block BulkStorage hands out and of every bulk array in a
// .pbo file, enough for any vertex attribute, index or SIMD copy.
inline constexpr size_t kBulkAlignment = 16;

// Where the bytes of a BulkStorage block live.
enum class BlockKind : uint8_t {
  kHeap,    // owned heap memory (cooked or edited data): private commit
  kMapped,  // a read-only file mapping (a loaded .pbo): backed by the file
};

// One block of a BulkStorage: `size` immutable bytes at `data`, kept alive
// by the shared pointer.
struct BulkBlock {
  std::shared_ptr<const std::byte> data;
  size_t size = 0;
  BlockKind kind = BlockKind::kHeap;

  [[nodiscard]] std::span<const std::byte> bytes() const {
    return {data.get(), size};
  }
};

// Shared immutable byte blocks backing the spans of a Prefab. Copying shares
// the blocks (reference counted): a block is freed when the last storage
// holding it is destroyed. Blocks never change after being stored, so views
// stay valid and copies may be used from several threads. Not thread-safe for
// concurrent Store calls on one object.
class BulkStorage {
 public:
  BulkStorage() = default;
  BulkStorage(const BulkStorage&) = default;
  BulkStorage& operator=(const BulkStorage&) = default;
  BulkStorage(BulkStorage&&) = default;
  BulkStorage& operator=(BulkStorage&&) = default;

  // Copies `bytes` into a new block and returns a view of it, aligned to
  // kBulkAlignment, valid while this storage or any copy of it lives. An
  // empty input returns an empty span without storing anything.
  std::span<const std::byte> Store(std::span<const std::byte> bytes);

  // Takes `bytes` over as a block without copying and returns a view of all of
  // it. Its data is aligned to kBulkAlignment (operator new guarantees it).
  std::span<const std::byte> Adopt(std::vector<std::byte> bytes);

  // Takes a block owned elsewhere (e.g. a file mapping kept alive by
  // `block.data`'s control block) and returns a view of all of it. Its data
  // must be aligned to kBulkAlignment and never change while referenced.
  std::span<const std::byte> AdoptBlock(BulkBlock block);

  // Copies `values` into a new block and returns a typed view of it.
  template <typename T>
  std::span<const T> StoreArray(std::span<const T> values) {
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(alignof(T) <= kBulkAlignment);
    const std::span<const std::byte> stored = Store(std::as_bytes(values));
    return {reinterpret_cast<const T*>(stored.data()), values.size()};
  }

  // Drops every block that none of `views` points into (views may be empty
  // or point elsewhere) and returns the bytes released from this storage.
  // Copies of the storage keep their blocks.
  size_t DropUnviewed(std::span<const std::span<const std::byte>> views);

  [[nodiscard]] std::span<const BulkBlock> blocks() const { return blocks_; }
  // Returns the number of blocks held.
  [[nodiscard]] size_t block_count() const { return blocks_.size(); }
  // Returns the total size of the blocks held, in bytes.
  [[nodiscard]] size_t byte_size() const;
  // Returns the size of the blocks of `kind`, in bytes.
  [[nodiscard]] size_t byte_size(BlockKind kind) const;

 private:
  std::vector<BulkBlock> blocks_;
};

// GPU formats of cooked textures. BC formats use 4x4 blocks of 16 bytes;
// RGBA8 is kept for tiny or odd textures not worth compressing. The disk
// value of each enumerator is its integer value: append only.
enum class TextureFormat {
  kBc7Srgb,     // colour (base colour, emissive)
  kBc7Unorm,    // linear data, e.g. the packed ORM texture
  kBc5Unorm,    // two-channel normal map, z reconstructed in the shader
  kRgba8Srgb,   // uncompressed colour
  kRgba8Unorm,  // uncompressed linear data or RGB normal map
};

// Returns true for the BC (4x4 block) formats.
[[nodiscard]] bool IsBlockCompressed(TextureFormat format);

// Returns the bytes one mip level of `width` x `height` texels takes in
// `format` (BC levels are padded to whole 4x4 blocks). Dimensions must be
// >= 1.
[[nodiscard]] size_t MipByteSize(TextureFormat format, int width, int height);

// Returns the number of levels of a full mip chain for a base level of
// `width` x `height`: floor(log2(max(width, height))) + 1.
[[nodiscard]] int FullMipCount(int width, int height);

// One level of a texture; level i is max(1, width >> i) x max(1, height >>
// i) texels and `data` holds exactly MipByteSize() bytes, rows top to bottom.
struct TextureMip {
  int width = 0;
  int height = 0;
  std::span<const std::byte> data;
};

// A cooked texture ready for upload: `mips` holds levels 0..n-1 (n >= 1,
// at most FullMipCount()).
struct Texture {
  std::string name;
  TextureFormat format = TextureFormat::kRgba8Unorm;
  int width = 0;
  int height = 0;
  std::vector<TextureMip> mips;

  [[nodiscard]] int mip_count() const { return static_cast<int>(mips.size()); }
};

// Texture inputs of a material. The disk value of each enumerator is its
// integer value.
enum class TextureSlot {
  kBaseColor,  // sRGB RGBA
  kOrm,        // linear: R occlusion, G roughness, B metallic
  kNormal,     // tangent space, +Y up; BC5 (two channels) or RGBA8
  kEmissive,   // sRGB RGB
  kCount,
};

inline constexpr int kTextureSlotCount = static_cast<int>(TextureSlot::kCount);

// Number of UV sets per vertex (asset::Vertex::uv0 and uv1).
inline constexpr int kUvSetCount = 2;

// A material of the prefab: the scalar parameters of material::MaterialDesc
// plus, per texture slot, an index into Prefab::textures (-1 = no texture,
// behaves like white / flat normal) and the UV set the slot samples (0 or 1).
struct Material {
  std::string name;
  glm::vec4 base_color{1.0f};  // linear RGBA factor; alpha used by kMask
  float metallic = 0.0f;       // [0, 1]
  float roughness = 0.5f;      // [0, 1], perceptual
  glm::vec3 emissive{0.0f};    // linear
  float emissive_strength = 1.0f;
  float normal_scale = 1.0f;
  float occlusion_strength = 1.0f;
  glm::vec2 uv_scale{1.0f};
  material::AlphaMode alpha_mode = material::AlphaMode::kOpaque;
  float alpha_cutoff = 0.5f;
  bool double_sided = false;
  std::array<int, kTextureSlotCount> textures{-1, -1, -1, -1};
  std::array<int, kTextureSlotCount> uv_sets{0, 0, 0, 0};

  // Returns the texture index of `slot`, or -1.
  [[nodiscard]] int texture(TextureSlot slot) const {
    return textures[static_cast<size_t>(slot)];
  }
};

// A triangle mesh attached to a node. `vertices` use the GPU layout
// (asset::Vertex), every three `indices` form a triangle, and `submeshes`
// partition the indices into material ranges (Submesh::material indexes
// Prefab::materials). `node` is an index into Prefab::hierarchy, or -1 for
// the prefab root. `bounds` encloses the vertices in the node's space.
struct Mesh {
  std::string name;
  int node = -1;
  asset::Bounds bounds;
  std::span<const asset::Vertex> vertices;
  std::span<const asset::Index> indices;
  std::vector<asset::Submesh> submeshes;
};

// A named attachment point, e.g. "muzzle", "sight", "grip" or "lens".
// `local` is relative to `node` (-1 = the prefab root).
struct Socket {
  std::string name;
  int node = -1;
  animation::Transform local;
};

// A point light carried by the prefab. `position` is relative to `node`
// (-1 = the prefab root); `color` is linear RGB; `range` > 0 is the distance
// in metres where the light fades out.
struct PointLight {
  std::string name;
  int node = -1;
  glm::vec3 position{0.0f};
  glm::vec3 color{1.0f};
  float intensity = 1.0f;
  float range = 10.0f;
};

// Another prefab placed inside this one. In memory `path` is resolved like
// any other path (absolute, or relative to the working directory); the file
// stores it relative to the .pbo. `local` is relative to `node` (-1 = root).
struct ChildPrefab {
  std::string path;
  int node = -1;
  animation::Transform local;
};

// Collision shape of the prefab's body. The disk value of each enumerator is
// its integer value.
enum class ColliderKind {
  kNone,          // no body
  kBox,           // half_extents
  kSphere,        // radius
  kCapsule,       // radius + half_height, along the shape's local Y
  kConvexHull,    // cooked_shape
  kTriangleMesh,  // cooked_shape, static only
};

// The prefab's rigid body. Primitive shapes are described by their
// parameters, placed at `center` / `rotation` in prefab space; hulls and
// triangle meshes (and optionally primitives) carry Jolt's binary shape state
// in `cooked_shape`, produced and restored by physics/shape_cooking.
struct Physics {
  ColliderKind kind = ColliderKind::kNone;
  glm::vec3 center{0.0f};
  glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
  glm::vec3 half_extents{0.5f};
  float radius = 0.5f;
  float half_height = 0.5f;
  bool dynamic = false;
  float mass = 1.0f;  // kg, used when dynamic
  float friction = 0.5f;
  float restitution = 0.0f;
  std::span<const std::byte> cooked_shape;
};

// A `key = value` label on a prefab, e.g. "weapon" = "glock17" marks a weapon
// prefab (see weapon/weapon_def.h). Keys are 1 to kMaxTagKeyLength
// characters of [a-z0-9_.] and unique within a prefab; values are free text
// of at most kMaxTagValueLength bytes (may be empty).
struct Tag {
  std::string key;
  std::string value;

  friend bool operator==(const Tag&, const Tag&) = default;
};

inline constexpr size_t kMaxTagKeyLength = 64;
inline constexpr size_t kMaxTagValueLength = 256;
inline constexpr size_t kMaxTagCount = 64;

// What the editor needs to reopen and re-import a prefab. Paths are resolved
// in memory and stored relative to the .pbo, like ChildPrefab::path.
// `import_settings` is opaque text owned by the cook (its options);
// `texture_sources[i]` lists the source images textures[i] was cooked from
// (several for a packed ORM texture; may be shorter than textures).
struct EditInfo {
  std::string source_model;
  std::string import_settings;
  std::vector<std::vector<std::string>> texture_sources;
};

// A prefab. Passive data: every index (node, material, texture) refers into
// the vectors of the same prefab and ValidatePrefab() checks them all. The
// bulk spans normally view `storage`; they may view any memory that outlives
// every use of the prefab, e.g. while saving.
struct Prefab {
  animation::Hierarchy hierarchy;
  std::vector<Mesh> meshes;
  std::vector<Material> materials;
  std::vector<Texture> textures;
  std::vector<animation::Clip> clips;  // channel nodes index `hierarchy`
  std::vector<Socket> sockets;
  std::vector<PointLight> lights;
  std::vector<ChildPrefab> children;
  Physics physics;
  // Ordered as authored; ValidatePrefab() checks keys, lengths, uniqueness
  // and at most kMaxTagCount entries.
  std::vector<Tag> tags;
  EditInfo edit;
  asset::Bounds bounds;  // all meshes at rest, in prefab space
  BulkStorage storage;
};

// Appends a view of every bulk array of `prefab` (vertex and index arrays,
// texture mips, the cooked shape) to `views`.
void CollectBulkViews(const Prefab& prefab,
                      std::vector<std::span<const std::byte>>& views);

// Drops the blocks of prefab.storage that no bulk array of `prefab` views any
// more (superseded by edits, e.g. a replaced texture) and returns the bytes
// released. Other copies of the prefab keep their blocks.
size_t CompactStorage(Prefab& prefab);

// Points every bulk array of `prefab` at the array in the same place of
// `source` and makes `prefab` share source.storage instead of its own: e.g.
// `source` is `prefab` saved and loaded again, so its data is a mapped file
// rather than heap blocks. Returns false and changes nothing unless both have
// the same meshes, textures, mips and cooked shape of equal byte sizes.
// Contents are not compared (that would read every page of the file): the
// caller guarantees they are equal. Metadata (names, paths, parameters) is
// left alone.
bool RebaseBulk(Prefab& prefab, const Prefab& source);

// Returns a description of the first problem that makes `prefab` unsavable,
// or nullopt if it is valid: an index out of range, an invalid hierarchy or
// clip, a non-finite or out-of-range number, a texture whose mips do not
// match its format and size, a mesh index >= its vertex count, a submesh
// outside the indices, a string longer than kMaxStringLength. LoadPrefab()
// returns only prefabs that pass.
[[nodiscard]] std::optional<std::string> ValidatePrefab(const Prefab& prefab);

// Upper bound on any string in a prefab (names, paths, import settings are
// allowed kMaxSettingsLength).
inline constexpr size_t kMaxStringLength = 4096;
inline constexpr size_t kMaxSettingsLength = 1024 * 1024;

// Returns the index of the first socket called `name`, or -1.
[[nodiscard]] int FindSocket(const Prefab& prefab, std::string_view name);

// Returns the index of the first clip called `name`, or -1.
[[nodiscard]] int FindClip(const Prefab& prefab, std::string_view name);

// Returns the index of the first material called `name`, or -1.
[[nodiscard]] int FindMaterial(const Prefab& prefab, std::string_view name);

// Returns true if `key` is a valid tag key: 1 to kMaxTagKeyLength
// characters, each a lower-case ASCII letter, a digit, '_' or '.'.
[[nodiscard]] bool IsValidTagKey(std::string_view key);

// Returns the value of tag `key`, or nullopt if the prefab has no such tag.
// The view is valid until `prefab.tags` changes.
[[nodiscard]] std::optional<std::string_view> FindTag(const Prefab& prefab,
                                                      std::string_view key);

// Sets tag `key` to `value`: replaces the value of an existing tag in place,
// otherwise appends the tag. Does not validate (ValidatePrefab() does).
void SetTag(Prefab& prefab, std::string_view key, std::string_view value);

// Removes tag `key`, keeping the order of the others. Returns false if the
// prefab had no such tag.
bool RemoveTag(Prefab& prefab, std::string_view key);

}  // namespace blue::prefab

#endif  // BLUE_PREFAB_PREFAB_H_
