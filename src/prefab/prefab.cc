#include "prefab/prefab.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/ext/quaternion_float.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "animation/clip.h"
#include "animation/hierarchy.h"
#include "animation/transform.h"
#include "asset/mesh_data.h"

namespace blue::prefab {
namespace {

static_assert(__STDCPP_DEFAULT_NEW_ALIGNMENT__ >= kBulkAlignment,
              "BulkStorage relies on operator new aligning to 16 bytes");

using Problem = std::optional<std::string>;

bool IsFinite(float value) { return std::isfinite(value); }

bool IsFinite(const glm::vec2& v) { return IsFinite(v.x) && IsFinite(v.y); }

bool IsFinite(const glm::vec3& v) {
  return IsFinite(v.x) && IsFinite(v.y) && IsFinite(v.z);
}

bool IsFinite(const glm::vec4& v) {
  return IsFinite(v.x) && IsFinite(v.y) && IsFinite(v.z) && IsFinite(v.w);
}

bool IsFinite(const glm::quat& q) {
  return IsFinite(q.x) && IsFinite(q.y) && IsFinite(q.z) && IsFinite(q.w);
}

bool IsFinite(const animation::Transform& t) {
  return IsFinite(t.translation) && IsFinite(t.rotation) && IsFinite(t.scale);
}

bool IsFinite(const asset::Bounds& b) {
  return IsFinite(b.min) && IsFinite(b.max);
}

bool InUnitRange(float value) { return value >= 0.0f && value <= 1.0f; }

// Returns true if `node` is -1 (the prefab root) or a node of `prefab`.
bool IsNodeRef(const Prefab& prefab, int node) {
  return node >= -1 && node < static_cast<int>(prefab.hierarchy.nodes.size());
}

bool IsShortString(std::string_view text) {
  return text.size() <= kMaxStringLength;
}

Problem ValidateNodes(const Prefab& prefab) {
  if (!animation::IsValid(prefab.hierarchy)) {
    return "hierarchy: a parent does not precede its child";
  }
  for (const animation::Node& node : prefab.hierarchy.nodes) {
    if (!IsShortString(node.name) || !IsFinite(node.rest)) {
      return std::format("node '{}': name too long or non-finite transform",
                         node.name);
    }
  }
  return std::nullopt;
}

Problem ValidateMesh(const Prefab& prefab, const Mesh& mesh) {
  if (!IsShortString(mesh.name) || !IsNodeRef(prefab, mesh.node) ||
      !IsFinite(mesh.bounds)) {
    return std::format("mesh '{}': bad name, node or bounds", mesh.name);
  }
  if (mesh.indices.size() % 3 != 0) {
    return std::format("mesh '{}': index count not a multiple of 3", mesh.name);
  }
  const size_t vertex_count = mesh.vertices.size();
  if (!std::ranges::all_of(mesh.indices, [vertex_count](asset::Index index) {
        return index < vertex_count;
      })) {
    return std::format("mesh '{}': index out of range", mesh.name);
  }
  for (const asset::Submesh& submesh : mesh.submeshes) {
    const uint64_t end =
        uint64_t{submesh.first_index} + uint64_t{submesh.index_count};
    if (submesh.index_count % 3 != 0 || end > mesh.indices.size() ||
        submesh.material < 0 ||
        submesh.material >= static_cast<int>(prefab.materials.size())) {
      return std::format("mesh '{}': submesh range or material out of range",
                         mesh.name);
    }
  }
  return std::nullopt;
}

Problem ValidateMaterial(const Prefab& prefab, const Material& material) {
  const bool finite =
      IsFinite(material.base_color) && IsFinite(material.metallic) &&
      IsFinite(material.roughness) && IsFinite(material.emissive) &&
      IsFinite(material.emissive_strength) && IsFinite(material.normal_scale) &&
      IsFinite(material.occlusion_strength) && IsFinite(material.uv_scale) &&
      IsFinite(material.alpha_cutoff);
  if (!IsShortString(material.name) || !finite) {
    return std::format("material '{}': name too long or non-finite value",
                       material.name);
  }
  if (material.alpha_mode != material::AlphaMode::kOpaque &&
      material.alpha_mode != material::AlphaMode::kMask) {
    return std::format("material '{}': unknown alpha mode", material.name);
  }
  for (int slot = 0; slot < kTextureSlotCount; ++slot) {
    const int texture = material.textures[static_cast<size_t>(slot)];
    const int uv_set = material.uv_sets[static_cast<size_t>(slot)];
    if (texture < -1 || texture >= static_cast<int>(prefab.textures.size()) ||
        uv_set < 0 || uv_set >= kUvSetCount) {
      return std::format(
          "material '{}': slot {} texture or UV set out of range",
          material.name, slot);
    }
  }
  return std::nullopt;
}

Problem ValidateTexture(const Texture& texture) {
  constexpr int kMaxTextureSize = 16384;
  if (!IsShortString(texture.name) || static_cast<int>(texture.format) < 0 ||
      texture.format > TextureFormat::kRgba8Unorm || texture.width < 1 ||
      texture.height < 1 || texture.width > kMaxTextureSize ||
      texture.height > kMaxTextureSize) {
    return std::format("texture '{}': bad name, format or size", texture.name);
  }
  if (texture.mips.empty() ||
      texture.mip_count() > FullMipCount(texture.width, texture.height)) {
    return std::format("texture '{}': bad mip count", texture.name);
  }
  for (int level = 0; level < texture.mip_count(); ++level) {
    const TextureMip& mip = texture.mips[static_cast<size_t>(level)];
    const int width = std::max(1, texture.width >> level);
    const int height = std::max(1, texture.height >> level);
    if (mip.width != width || mip.height != height ||
        mip.data.size() != MipByteSize(texture.format, width, height)) {
      return std::format("texture '{}': mip {} size mismatch", texture.name,
                         level);
    }
  }
  return std::nullopt;
}

Problem ValidateClip(const Prefab& prefab, const animation::Clip& clip) {
  if (!IsShortString(clip.name) || !animation::IsValid(clip)) {
    return std::format("clip '{}': invalid channel, duration or events",
                       clip.name);
  }
  const int node_count = static_cast<int>(prefab.hierarchy.nodes.size());
  for (const animation::Channel& channel : clip.channels) {
    if (channel.node >= node_count ||
        !std::ranges::all_of(channel.times,
                             [](float t) { return IsFinite(t); }) ||
        !std::ranges::all_of(channel.values,
                             [](float v) { return IsFinite(v); })) {
      return std::format("clip '{}': channel node out of range or non-finite",
                         clip.name);
    }
  }
  for (const animation::ClipEvent& event : clip.events) {
    if (!IsShortString(event.name)) {
      return std::format("clip '{}': event name too long", clip.name);
    }
  }
  return std::nullopt;
}

Problem ValidatePlacements(const Prefab& prefab) {
  for (const Socket& socket : prefab.sockets) {
    if (!IsShortString(socket.name) || !IsNodeRef(prefab, socket.node) ||
        !IsFinite(socket.local)) {
      return std::format("socket '{}': bad name, node or transform",
                         socket.name);
    }
  }
  for (const PointLight& light : prefab.lights) {
    if (!IsShortString(light.name) || !IsNodeRef(prefab, light.node) ||
        !IsFinite(light.position) || !IsFinite(light.color) ||
        !IsFinite(light.intensity) || !IsFinite(light.range) ||
        light.intensity < 0.0f || light.range <= 0.0f) {
      return std::format("light '{}': bad name, node or parameters",
                         light.name);
    }
  }
  for (const ChildPrefab& child : prefab.children) {
    if (child.path.empty() || !IsShortString(child.path) ||
        !IsNodeRef(prefab, child.node) || !IsFinite(child.local)) {
      return std::format("child '{}': bad path, node or transform", child.path);
    }
  }
  return std::nullopt;
}

Problem ValidatePhysics(const Physics& physics) {
  if (static_cast<int>(physics.kind) < 0 ||
      physics.kind > ColliderKind::kTriangleMesh) {
    return "physics: unknown collider kind";
  }
  const bool finite =
      IsFinite(physics.center) && IsFinite(physics.rotation) &&
      IsFinite(physics.half_extents) && IsFinite(physics.radius) &&
      IsFinite(physics.half_height) && IsFinite(physics.mass) &&
      IsFinite(physics.friction) && IsFinite(physics.restitution);
  if (!finite || physics.half_extents.x < 0.0f ||
      physics.half_extents.y < 0.0f || physics.half_extents.z < 0.0f ||
      physics.radius < 0.0f || physics.half_height < 0.0f ||
      physics.mass < 0.0f || physics.friction < 0.0f ||
      !InUnitRange(physics.restitution)) {
    return "physics: non-finite or negative parameter";
  }
  if ((physics.kind == ColliderKind::kConvexHull ||
       physics.kind == ColliderKind::kTriangleMesh) &&
      physics.cooked_shape.empty()) {
    return "physics: hull and triangle mesh need a cooked shape";
  }
  if (physics.kind == ColliderKind::kTriangleMesh && physics.dynamic) {
    return "physics: triangle meshes cannot be dynamic";
  }
  return std::nullopt;
}

Problem ValidateEdit(const EditInfo& edit) {
  if (!IsShortString(edit.source_model) ||
      edit.import_settings.size() > kMaxSettingsLength) {
    return "edit info: source path or import settings too long";
  }
  for (const std::vector<std::string>& sources : edit.texture_sources) {
    if (!std::ranges::all_of(sources, [](const std::string& path) {
          return IsShortString(path);
        })) {
      return "edit info: texture source path too long";
    }
  }
  return std::nullopt;
}

}  // namespace

std::span<const std::byte> BulkStorage::Store(
    std::span<const std::byte> bytes) {
  if (bytes.empty()) {
    return {};
  }
  return Adopt(std::vector<std::byte>(bytes.begin(), bytes.end()));
}

std::span<const std::byte> BulkStorage::Adopt(std::vector<std::byte> bytes) {
  auto owner = std::make_shared<const std::vector<std::byte>>(std::move(bytes));
  const std::byte* data = owner->data();
  const size_t size = owner->size();
  // Aliasing constructor: the block points at the bytes, owns the vector.
  return AdoptBlock({.data = std::shared_ptr<const std::byte>(owner, data),
                     .size = size,
                     .kind = BlockKind::kHeap});
}

std::span<const std::byte> BulkStorage::AdoptBlock(BulkBlock block) {
  const std::span<const std::byte> view = block.bytes();
  blocks_.push_back(std::move(block));
  return view;
}

size_t BulkStorage::DropUnviewed(
    std::span<const std::span<const std::byte>> views) {
  const auto viewed = [views](const BulkBlock& block) {
    const auto begin = reinterpret_cast<uintptr_t>(block.data.get());
    const uintptr_t end = begin + block.size;
    return std::ranges::any_of(views, [begin, end](auto view) {
      const auto at = reinterpret_cast<uintptr_t>(view.data());
      return !view.empty() && at >= begin && at < end;
    });
  };
  size_t released = 0;
  std::erase_if(blocks_, [&](const BulkBlock& block) {
    if (viewed(block)) {
      return false;
    }
    released += block.size;
    return true;
  });
  return released;
}

size_t BulkStorage::byte_size() const {
  size_t total = 0;
  for (const BulkBlock& block : blocks_) {
    total += block.size;
  }
  return total;
}

size_t BulkStorage::byte_size(BlockKind kind) const {
  size_t total = 0;
  for (const BulkBlock& block : blocks_) {
    total += block.kind == kind ? block.size : 0;
  }
  return total;
}

void CollectBulkViews(const Prefab& prefab,
                      std::vector<std::span<const std::byte>>& views) {
  for (const Mesh& mesh : prefab.meshes) {
    views.push_back(std::as_bytes(mesh.vertices));
    views.push_back(std::as_bytes(mesh.indices));
  }
  for (const Texture& texture : prefab.textures) {
    for (const TextureMip& mip : texture.mips) {
      views.push_back(mip.data);
    }
  }
  views.push_back(prefab.physics.cooked_shape);
}

size_t CompactStorage(Prefab& prefab) {
  std::vector<std::span<const std::byte>> views;
  CollectBulkViews(prefab, views);
  return prefab.storage.DropUnviewed(views);
}

bool RebaseBulk(Prefab& prefab, const Prefab& source) {
  std::vector<std::span<const std::byte>> from;
  std::vector<std::span<const std::byte>> to;
  CollectBulkViews(prefab, from);
  CollectBulkViews(source, to);
  // Equal shapes list the arrays in the same order.
  const bool same = prefab.meshes.size() == source.meshes.size() &&
                    std::ranges::equal(prefab.textures, source.textures,
                                       [](const Texture& a, const Texture& b) {
                                         return a.mips.size() == b.mips.size();
                                       }) &&
                    std::ranges::equal(from, to, [](auto a, auto b) {
                      return a.size() == b.size();
                    });
  if (!same) {
    return false;
  }
  size_t next = 0;
  const auto take = [&to, &next]() { return to[next++]; };
  for (Mesh& mesh : prefab.meshes) {
    const std::span<const std::byte> vertices = take();
    const std::span<const std::byte> indices = take();
    mesh.vertices = {reinterpret_cast<const asset::Vertex*>(vertices.data()),
                     mesh.vertices.size()};
    mesh.indices = {reinterpret_cast<const asset::Index*>(indices.data()),
                    mesh.indices.size()};
  }
  for (Texture& texture : prefab.textures) {
    for (TextureMip& mip : texture.mips) {
      mip.data = take();
    }
  }
  prefab.physics.cooked_shape = take();
  prefab.storage = source.storage;
  return true;
}

bool IsBlockCompressed(TextureFormat format) {
  switch (format) {
    case TextureFormat::kBc7Srgb:
    case TextureFormat::kBc7Unorm:
    case TextureFormat::kBc5Unorm:
      return true;
    case TextureFormat::kRgba8Srgb:
    case TextureFormat::kRgba8Unorm:
      return false;
  }
  return false;
}

size_t MipByteSize(TextureFormat format, int width, int height) {
  const auto w = static_cast<size_t>(std::max(width, 1));
  const auto h = static_cast<size_t>(std::max(height, 1));
  if (IsBlockCompressed(format)) {
    constexpr size_t kBlockBytes = 16;
    return ((w + 3) / 4) * ((h + 3) / 4) * kBlockBytes;
  }
  constexpr size_t kRgba8Bytes = 4;
  return w * h * kRgba8Bytes;
}

int FullMipCount(int width, int height) {
  const auto largest = static_cast<unsigned>(std::max({width, height, 1}));
  return std::bit_width(largest);
}

std::optional<std::string> ValidatePrefab(const Prefab& prefab) {
  if (Problem problem = ValidateNodes(prefab)) {
    return problem;
  }
  if (!IsFinite(prefab.bounds)) {
    return "prefab bounds are not finite";
  }
  for (const Mesh& mesh : prefab.meshes) {
    if (Problem problem = ValidateMesh(prefab, mesh)) {
      return problem;
    }
  }
  for (const Material& material : prefab.materials) {
    if (Problem problem = ValidateMaterial(prefab, material)) {
      return problem;
    }
  }
  for (const Texture& texture : prefab.textures) {
    if (Problem problem = ValidateTexture(texture)) {
      return problem;
    }
  }
  for (const animation::Clip& clip : prefab.clips) {
    if (Problem problem = ValidateClip(prefab, clip)) {
      return problem;
    }
  }
  if (Problem problem = ValidatePlacements(prefab)) {
    return problem;
  }
  if (Problem problem = ValidatePhysics(prefab.physics)) {
    return problem;
  }
  return ValidateEdit(prefab.edit);
}

int FindSocket(const Prefab& prefab, std::string_view name) {
  const auto found = std::ranges::find(prefab.sockets, name, &Socket::name);
  return found == prefab.sockets.end()
             ? -1
             : static_cast<int>(found - prefab.sockets.begin());
}

int FindClip(const Prefab& prefab, std::string_view name) {
  const auto found =
      std::ranges::find(prefab.clips, name, &animation::Clip::name);
  return found == prefab.clips.end()
             ? -1
             : static_cast<int>(found - prefab.clips.begin());
}

int FindMaterial(const Prefab& prefab, std::string_view name) {
  const auto found = std::ranges::find(prefab.materials, name, &Material::name);
  return found == prefab.materials.end()
             ? -1
             : static_cast<int>(found - prefab.materials.begin());
}

}  // namespace blue::prefab
