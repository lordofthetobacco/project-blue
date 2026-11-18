#include "editor/prefab_mode/prefab_edit.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/ext/quaternion_float.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "animation/clip.h"
#include "animation/hierarchy.h"
#include "animation/transform.h"
#include "asset/image_decoder.h"
#include "asset/mesh_data.h"
#include "cook/cook_error.h"
#include "cook/model_cook.h"
#include "cook/texture_cook.h"
#include "core/log.h"
#include "core/path.h"
#include "core/result.h"
#include "prefab/prefab.h"
#include "prefab/prefab_file.h"

namespace blue::editor {

namespace {

using prefab::Prefab;

// Rewrites every node reference of `prefab` through `new_index` (old index
// -> new index, -1 = removed) and installs `nodes` as the hierarchy. Things
// attached to removed nodes are dropped.
void ApplyNodeMapping(Prefab& prefab, std::span<const int> new_index,
                      std::vector<animation::Node> nodes) {
  const auto map = [&](int node) {
    return node < 0 ? -1 : new_index[static_cast<size_t>(node)];
  };
  const auto removed = [&](int node) { return node >= 0 && map(node) < 0; };
  for (animation::Node& node : nodes) {
    node.parent = map(node.parent);
  }
  prefab.hierarchy.nodes = std::move(nodes);

  std::erase_if(prefab.meshes,
                [&](const prefab::Mesh& mesh) { return removed(mesh.node); });
  for (prefab::Mesh& mesh : prefab.meshes) {
    mesh.node = map(mesh.node);
  }
  std::erase_if(prefab.sockets, [&](const prefab::Socket& socket) {
    return removed(socket.node);
  });
  for (prefab::Socket& socket : prefab.sockets) {
    socket.node = map(socket.node);
  }
  std::erase_if(prefab.lights, [&](const prefab::PointLight& light) {
    return removed(light.node);
  });
  for (prefab::PointLight& light : prefab.lights) {
    light.node = map(light.node);
  }
  std::erase_if(prefab.children, [&](const prefab::ChildPrefab& child) {
    return removed(child.node);
  });
  for (prefab::ChildPrefab& child : prefab.children) {
    child.node = map(child.node);
  }
  for (animation::Clip& clip : prefab.clips) {
    std::erase_if(clip.channels, [&](const animation::Channel& channel) {
      return channel.node < 0 || removed(channel.node);
    });
    for (animation::Channel& channel : clip.channels) {
      channel.node = map(channel.node);
    }
  }
}

// Returns `path` normalised for comparison: '/' separators, and lower case
// on Windows where the file system ignores case.
[[nodiscard]] std::string ComparablePath(std::string_view path) {
  std::string normal = core::NormalizePath(path);
#ifdef _WIN32
  for (char& c : normal) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
#endif
  return normal;
}

// Depth-first search for FindChildCycle(). `chain` holds the comparable
// paths from the owner to `path` (inclusive) and `labels` their file names.
[[nodiscard]] std::optional<std::string> VisitChild(
    const std::string& owner, const std::string& path,
    std::vector<std::string>& chain, std::vector<std::string>& labels,
    ChildPathReader read, void* user, int depth, int max_depth) {
  const auto describe = [&] {
    std::string text;
    for (const std::string& label : labels) {
      if (!text.empty()) {
        text += " -> ";
      }
      text += label;
    }
    return text;
  };
  const std::string comparable = ComparablePath(path);
  if (comparable == owner) {
    return "cycle: " + describe();
  }
  if (std::find(chain.begin(), chain.end() - 1, comparable) !=
      chain.end() - 1) {
    return "existing cycle: " + describe();
  }
  if (depth >= max_depth) {
    return std::format("nesting deeper than {} levels: {}", max_depth,
                       describe());
  }
  const std::optional<std::vector<std::string>> children = read(path, user);
  if (!children) {
    return std::nullopt;
  }
  for (const std::string& child : *children) {
    chain.push_back(ComparablePath(child));
    labels.push_back(core::FileName(child));
    std::optional<std::string> problem = VisitChild(
        owner, child, chain, labels, read, user, depth + 1, max_depth);
    chain.pop_back();
    labels.pop_back();
    if (problem) {
      return problem;
    }
  }
  return std::nullopt;
}

// Returns the index of the node of `to` named like node `node` of `from`
// (-1 stays -1 and a missing name gives -1).
[[nodiscard]] int MatchNode(const Prefab& from, int node, const Prefab& to) {
  if (node < 0) {
    return -1;
  }
  return animation::FindNode(
      to.hierarchy, from.hierarchy.nodes[static_cast<size_t>(node)].name);
}

// Expresses `local` (relative to node `node` of `from`) relative to node
// `new_node` of `to`: unchanged when the node was found by name, otherwise
// moved to the root keeping its model-space placement.
[[nodiscard]] animation::Transform CarryTransform(
    const Prefab& from, int node, int new_node,
    const animation::Transform& local) {
  if (node < 0 || new_node >= 0) {
    return local;
  }
  return DecomposeMatrix(NodeWorldMatrix(from, node) *
                         animation::ToMatrix(local));
}

}  // namespace

std::string MakeUniqueName(std::string_view base,
                           std::span<const std::string> taken) {
  const auto is_taken = [&](std::string_view name) {
    return std::find(taken.begin(), taken.end(), name) != taken.end();
  };
  if (!is_taken(base)) {
    return std::string(base);
  }
  for (int suffix = 2;; ++suffix) {
    std::string candidate = std::format("{} {}", base, suffix);
    if (!is_taken(candidate)) {
      return candidate;
    }
  }
}

std::vector<glm::mat4> RestWorldMatrices(const Prefab& prefab) {
  const size_t count = prefab.hierarchy.nodes.size();
  std::vector<animation::Transform> pose(count);
  animation::ResetToRest(prefab.hierarchy, pose);
  std::vector<glm::mat4> world(count, glm::mat4(1.0f));
  animation::ComputeWorldMatrices(prefab.hierarchy, pose, world);
  return world;
}

glm::mat4 NodeWorldMatrix(const Prefab& prefab, int node) {
  glm::mat4 world(1.0f);
  for (int current = node; current >= 0;
       current = prefab.hierarchy.nodes[static_cast<size_t>(current)].parent) {
    world = animation::ToMatrix(
                prefab.hierarchy.nodes[static_cast<size_t>(current)].rest) *
            world;
  }
  return world;
}

animation::Transform DecomposeMatrix(const glm::mat4& matrix) {
  animation::Transform transform;
  transform.translation = glm::vec3(matrix[3]);
  glm::mat3 basis(matrix);
  for (int axis = 0; axis < 3; ++axis) {
    const float length = glm::length(basis[axis]);
    transform.scale[axis] = length;
    if (length > 0.0f) {
      basis[axis] /= length;
    }
  }
  if (glm::determinant(basis) < 0.0f) {
    transform.scale.x = -transform.scale.x;
    basis[0] = -basis[0];
  }
  transform.rotation = glm::normalize(glm::quat_cast(basis));
  return transform;
}

void UpdatePrefabBounds(Prefab& prefab) {
  if (prefab.meshes.empty()) {
    prefab.bounds = {};
    return;
  }
  const std::vector<glm::mat4> world = RestWorldMatrices(prefab);
  glm::vec3 low(INFINITY);
  glm::vec3 high(-INFINITY);
  for (const prefab::Mesh& mesh : prefab.meshes) {
    const glm::mat4 matrix =
        mesh.node < 0 ? glm::mat4(1.0f) : world[static_cast<size_t>(mesh.node)];
    for (int corner = 0; corner < 8; ++corner) {
      const glm::vec3 local{
          (corner & 1) ? mesh.bounds.max.x : mesh.bounds.min.x,
          (corner & 2) ? mesh.bounds.max.y : mesh.bounds.min.y,
          (corner & 4) ? mesh.bounds.max.z : mesh.bounds.min.z};
      const glm::vec3 point = glm::vec3(matrix * glm::vec4(local, 1.0f));
      low = glm::min(low, point);
      high = glm::max(high, point);
    }
  }
  prefab.bounds = {.min = low, .max = high};
}

bool IsInSubtree(const Prefab& prefab, int node, int ancestor) {
  for (int current = node; current >= 0;
       current = prefab.hierarchy.nodes[static_cast<size_t>(current)].parent) {
    if (current == ancestor) {
      return true;
    }
  }
  return false;
}

bool RemoveNode(Prefab& prefab, int node) {
  const int count = static_cast<int>(prefab.hierarchy.nodes.size());
  if (node < 0 || node >= count) {
    return false;
  }
  std::vector<int> new_index(static_cast<size_t>(count), -1);
  std::vector<animation::Node> nodes;
  nodes.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    if (!IsInSubtree(prefab, i, node)) {
      new_index[static_cast<size_t>(i)] = static_cast<int>(nodes.size());
      nodes.push_back(prefab.hierarchy.nodes[static_cast<size_t>(i)]);
    }
  }
  ApplyNodeMapping(prefab, new_index, std::move(nodes));
  return true;
}

bool ReparentNode(Prefab& prefab, int node, int new_parent) {
  const int count = static_cast<int>(prefab.hierarchy.nodes.size());
  if (node < 0 || node >= count || new_parent < -1 || new_parent >= count ||
      (new_parent >= 0 && IsInSubtree(prefab, new_parent, node))) {
    return false;
  }
  const glm::mat4 world = NodeWorldMatrix(prefab, node);
  const glm::mat4 parent_world = NodeWorldMatrix(prefab, new_parent);
  animation::Node& moved = prefab.hierarchy.nodes[static_cast<size_t>(node)];
  moved.rest = DecomposeMatrix(glm::inverse(parent_world) * world);
  moved.parent = new_parent;

  // Pre-order walk from the roots, children in their old order: parents
  // come first again.
  std::vector<std::vector<int>> children(static_cast<size_t>(count));
  std::vector<int> roots;
  for (int i = 0; i < count; ++i) {
    const int parent = prefab.hierarchy.nodes[static_cast<size_t>(i)].parent;
    (parent < 0 ? roots : children[static_cast<size_t>(parent)]).push_back(i);
  }
  std::vector<int> order;
  order.reserve(static_cast<size_t>(count));
  std::vector<int> stack(roots.rbegin(), roots.rend());
  while (!stack.empty()) {
    const int current = stack.back();
    stack.pop_back();
    order.push_back(current);
    const std::vector<int>& kids = children[static_cast<size_t>(current)];
    stack.insert(stack.end(), kids.rbegin(), kids.rend());
  }
  std::vector<int> new_index(static_cast<size_t>(count), -1);
  std::vector<animation::Node> nodes;
  nodes.reserve(static_cast<size_t>(count));
  for (const int old : order) {
    new_index[static_cast<size_t>(old)] = static_cast<int>(nodes.size());
    nodes.push_back(prefab.hierarchy.nodes[static_cast<size_t>(old)]);
  }
  ApplyNodeMapping(prefab, new_index, std::move(nodes));
  return true;
}

bool SetSubmeshMaterial(Prefab& prefab, int mesh, int submesh, int material) {
  if (mesh < 0 || mesh >= static_cast<int>(prefab.meshes.size()) ||
      material < 0 || material >= static_cast<int>(prefab.materials.size())) {
    return false;
  }
  std::vector<asset::Submesh>& submeshes =
      prefab.meshes[static_cast<size_t>(mesh)].submeshes;
  if (submesh < 0 || submesh >= static_cast<int>(submeshes.size())) {
    return false;
  }
  submeshes[static_cast<size_t>(submesh)].material = material;
  return true;
}

int CountTextureUses(const Prefab& prefab, int texture) {
  int uses = 0;
  for (const prefab::Material& material : prefab.materials) {
    uses += static_cast<int>(std::count(material.textures.begin(),
                                        material.textures.end(), texture));
  }
  return uses;
}

bool RemoveTexture(Prefab& prefab, int texture) {
  if (texture < 0 || texture >= static_cast<int>(prefab.textures.size())) {
    return false;
  }
  for (prefab::Material& material : prefab.materials) {
    for (int& index : material.textures) {
      if (index == texture) {
        index = -1;
      } else if (index > texture) {
        --index;
      }
    }
  }
  prefab.textures.erase(prefab.textures.begin() + texture);
  std::vector<std::vector<std::string>>& sources = prefab.edit.texture_sources;
  if (texture < static_cast<int>(sources.size())) {
    sources.erase(sources.begin() + texture);
  }
  return true;
}

int RemoveUnusedTextures(Prefab& prefab) {
  int removed = 0;
  for (int texture = static_cast<int>(prefab.textures.size()) - 1; texture >= 0;
       --texture) {
    if (CountTextureUses(prefab, texture) == 0) {
      (void)RemoveTexture(prefab, texture);
      ++removed;
    }
  }
  return removed;
}

cook::TextureUsage UsageOfSlot(prefab::TextureSlot slot) {
  switch (slot) {
    case prefab::TextureSlot::kOrm:
      return cook::TextureUsage::kLinear;
    case prefab::TextureSlot::kNormal:
      return cook::TextureUsage::kNormal;
    case prefab::TextureSlot::kBaseColor:
    case prefab::TextureSlot::kEmissive:
    case prefab::TextureSlot::kCount:
      break;
  }
  return cook::TextureUsage::kColor;
}

int AssignTexture(Prefab& prefab, int material, prefab::TextureSlot slot,
                  prefab::Texture texture, std::vector<std::string> sources) {
  if (material < 0 || material >= static_cast<int>(prefab.materials.size()) ||
      slot == prefab::TextureSlot::kCount) {
    return -1;
  }
  texture.name = sources.empty() ? std::string("texture")
                                 : core::FileStem(sources.front());
  int& index = prefab.materials[static_cast<size_t>(material)]
                   .textures[static_cast<size_t>(slot)];
  if (index < 0 || CountTextureUses(prefab, index) > 1) {
    prefab.textures.push_back(std::move(texture));
    index = static_cast<int>(prefab.textures.size()) - 1;
  } else {
    prefab.textures[static_cast<size_t>(index)] = std::move(texture);
  }
  std::vector<std::vector<std::string>>& stored = prefab.edit.texture_sources;
  if (stored.size() <= static_cast<size_t>(index)) {
    stored.resize(static_cast<size_t>(index) + 1);
  }
  stored[static_cast<size_t>(index)] = std::move(sources);
  return index;
}

core::Result<int, cook::CookError> ImportSlotTexture(
    Prefab& prefab, int material, prefab::TextureSlot slot,
    const std::string& path, const TextureImportSettings& settings) {
  using Result = core::Result<int, cook::CookError>;
  if (material < 0 || material >= static_cast<int>(prefab.materials.size()) ||
      slot == prefab::TextureSlot::kCount) {
    return Result::Error(cook::CookError::kInvalidArgument);
  }
  core::Result<asset::ImageData, asset::ImageError> image =
      asset::LoadImageFile(path);
  if (!image.ok()) {
    core::log::Error("Cannot load texture {}: {}", path,
                     asset::ToString(image.error()));
    return Result::Error(cook::CookError::kTextureLoadFailed);
  }
  const cook::TextureCookOptions options{.usage = UsageOfSlot(slot),
                                         .quality = settings.quality,
                                         .compress = settings.compress,
                                         .generate_mips = true,
                                         .max_size = settings.max_size};
  core::Result<prefab::Texture, cook::CookError> cooked =
      cook::CookTexture(image.value(), options, prefab.storage);
  if (!cooked.ok()) {
    return Result::Error(cooked.error());
  }
  return Result::Ok(
      AssignTexture(prefab, material, slot, std::move(cooked).value(), {path}));
}

core::Result<int, cook::CookError> ImportOrmTexture(
    Prefab& prefab, int material, const OrmImportSources& sources,
    const TextureImportSettings& settings) {
  using Result = core::Result<int, cook::CookError>;
  const std::array<const OrmChannelSource*, 3> channels{
      &sources.occlusion, &sources.roughness, &sources.metallic};
  bool any = false;
  for (const OrmChannelSource* channel : channels) {
    if (channel->channel < 0 || channel->channel > 3) {
      return Result::Error(cook::CookError::kInvalidArgument);
    }
    any = any || !channel->path.empty();
  }
  if (!any || material < 0 ||
      material >= static_cast<int>(prefab.materials.size())) {
    return Result::Error(cook::CookError::kInvalidArgument);
  }
  // Each distinct file is decoded once; images[i] belongs to paths[i].
  std::vector<std::string> paths;
  std::vector<asset::ImageData> images;
  std::array<int, 3> image_of{-1, -1, -1};
  for (size_t i = 0; i < channels.size(); ++i) {
    const std::string& path = channels[i]->path;
    if (path.empty()) {
      continue;
    }
    const auto found = std::find(paths.begin(), paths.end(), path);
    if (found != paths.end()) {
      image_of[i] = static_cast<int>(found - paths.begin());
      continue;
    }
    core::Result<asset::ImageData, asset::ImageError> image =
        asset::LoadImageFile(path);
    if (!image.ok()) {
      core::log::Error("Cannot load ORM source {}: {}", path,
                       asset::ToString(image.error()));
      return Result::Error(cook::CookError::kTextureLoadFailed);
    }
    image_of[i] = static_cast<int>(paths.size());
    paths.push_back(path);
    images.push_back(std::move(image).value());
  }
  const auto image_at = [&](size_t i) -> const asset::ImageData* {
    return image_of[i] < 0 ? nullptr
                           : &images[static_cast<size_t>(image_of[i])];
  };
  const asset::ImageData packed =
      cook::PackOrm({.occlusion = image_at(0),
                     .occlusion_channel = sources.occlusion.channel,
                     .roughness = image_at(1),
                     .roughness_channel = sources.roughness.channel,
                     .metallic = image_at(2),
                     .metallic_channel = sources.metallic.channel});
  const cook::TextureCookOptions options{.usage = cook::TextureUsage::kLinear,
                                         .quality = settings.quality,
                                         .compress = settings.compress,
                                         .generate_mips = true,
                                         .max_size = settings.max_size};
  core::Result<prefab::Texture, cook::CookError> cooked =
      cook::CookTexture(packed, options, prefab.storage);
  if (!cooked.ok()) {
    return Result::Error(cooked.error());
  }
  // Recorded in channel order so the dialog can be refilled; the texture is
  // named after the first source.
  std::vector<std::string> recorded;
  for (const OrmChannelSource* channel : channels) {
    recorded.push_back(
        channel->path.empty()
            ? std::string()
            : std::format("{}#{}", channel->path, channel->channel));
  }
  int index = AssignTexture(prefab, material, prefab::TextureSlot::kOrm,
                            std::move(cooked).value(), std::move(recorded));
  prefab.textures[static_cast<size_t>(index)].name =
      core::FileStem(paths.front()) + "_orm";
  return Result::Ok(index);
}

size_t TextureByteSize(const prefab::Texture& texture) {
  size_t bytes = 0;
  for (const prefab::TextureMip& mip : texture.mips) {
    bytes += mip.data.size();
  }
  return bytes;
}

PrefabSummary Summarize(const Prefab& prefab) {
  PrefabSummary summary;
  summary.node_count = static_cast<int>(prefab.hierarchy.nodes.size());
  summary.mesh_count = static_cast<int>(prefab.meshes.size());
  for (const prefab::Mesh& mesh : prefab.meshes) {
    summary.submesh_count += static_cast<int>(mesh.submeshes.size());
    summary.vertex_count += mesh.vertices.size();
    summary.triangle_count += mesh.indices.size() / 3;
    summary.vertex_bytes += mesh.vertices.size_bytes();
    summary.index_bytes += mesh.indices.size_bytes();
  }
  summary.material_count = static_cast<int>(prefab.materials.size());
  summary.texture_count = static_cast<int>(prefab.textures.size());
  for (const prefab::Texture& texture : prefab.textures) {
    summary.texture_bytes += TextureByteSize(texture);
  }
  summary.clip_count = static_cast<int>(prefab.clips.size());
  summary.socket_count = static_cast<int>(prefab.sockets.size());
  summary.light_count = static_cast<int>(prefab.lights.size());
  summary.child_count = static_cast<int>(prefab.children.size());
  summary.collision_bytes = prefab.physics.cooked_shape.size();
  summary.storage_bytes = prefab.storage.byte_size();
  return summary;
}

const char* FormatLabel(prefab::TextureFormat format) {
  switch (format) {
    case prefab::TextureFormat::kBc7Srgb:
      return "BC7 sRGB";
    case prefab::TextureFormat::kBc7Unorm:
      return "BC7 linear";
    case prefab::TextureFormat::kBc5Unorm:
      return "BC5";
    case prefab::TextureFormat::kRgba8Srgb:
      return "RGBA8 sRGB";
    case prefab::TextureFormat::kRgba8Unorm:
      return "RGBA8 linear";
  }
  return "unknown";
}

std::optional<std::string> FindChildCycle(const std::string& owner_path,
                                          const std::string& child_path,
                                          ChildPathReader read, void* user,
                                          int max_depth) {
  // A copy: `chain` grows during the search.
  const std::string owner = ComparablePath(owner_path);
  std::vector<std::string> chain{owner, ComparablePath(child_path)};
  std::vector<std::string> labels{core::FileName(owner_path),
                                  core::FileName(child_path)};
  // `chain` already holds the child: VisitChild compares against the
  // entries before it.
  return VisitChild(owner, child_path, chain, labels, read, user,
                    /*depth=*/1, max_depth);
}

std::optional<std::vector<std::string>> ReadChildPathsFromFile(
    const std::string& path, void* /*user*/) {
  core::Result<Prefab, prefab::PrefabError> loaded = prefab::LoadPrefab(path);
  if (!loaded.ok()) {
    return std::nullopt;
  }
  std::vector<std::string> paths;
  for (const prefab::ChildPrefab& child : loaded.value().children) {
    paths.push_back(child.path);
  }
  return paths;
}

Prefab ReimportPrefab(const Prefab& edited, Prefab fresh) {
  for (prefab::Material& material : fresh.materials) {
    const int old = prefab::FindMaterial(edited, material.name);
    if (old < 0) {
      continue;
    }
    // Parameters and UV sets come from the edit; textures from the cook.
    const std::array<int, prefab::kTextureSlotCount> textures =
        material.textures;
    material = edited.materials[static_cast<size_t>(old)];
    material.textures = textures;
  }
  for (animation::Clip& clip : fresh.clips) {
    const int old = prefab::FindClip(edited, clip.name);
    if (old < 0) {
      continue;
    }
    const animation::Clip& before = edited.clips[static_cast<size_t>(old)];
    clip.loop = before.loop;
    clip.events.clear();
    for (animation::ClipEvent event : before.events) {
      event.time = std::clamp(event.time, 0.0f, clip.duration);
      animation::AddEvent(clip, std::move(event));
    }
  }
  fresh.sockets.clear();
  for (const prefab::Socket& socket : edited.sockets) {
    const int node = MatchNode(edited, socket.node, fresh);
    fresh.sockets.push_back(
        {.name = socket.name,
         .node = node,
         .local = CarryTransform(edited, socket.node, node, socket.local)});
  }
  fresh.lights.clear();
  for (prefab::PointLight light : edited.lights) {
    const int node = MatchNode(edited, light.node, fresh);
    if (light.node >= 0 && node < 0) {
      light.position = glm::vec3(NodeWorldMatrix(edited, light.node) *
                                 glm::vec4(light.position, 1.0f));
    }
    light.node = node;
    fresh.lights.push_back(std::move(light));
  }
  fresh.children.clear();
  for (const prefab::ChildPrefab& child : edited.children) {
    const int node = MatchNode(edited, child.node, fresh);
    fresh.children.push_back(
        {.path = child.path,
         .node = node,
         .local = CarryTransform(edited, child.node, node, child.local)});
  }
  fresh.physics = edited.physics;
  fresh.physics.cooked_shape = {};
  return fresh;
}

cook::ModelCookOptions StoredImportOptions(const Prefab& prefab) {
  if (prefab.edit.import_settings.empty()) {
    return {};
  }
  core::Result<cook::ModelCookOptions, cook::CookError> parsed =
      cook::ParseModelCookOptions(prefab.edit.import_settings);
  if (!parsed.ok()) {
    core::log::Warn("Stored import settings do not parse ({}); using defaults",
                    cook::ToString(parsed.error()));
    return {};
  }
  return std::move(parsed).value();
}

int AddSocket(Prefab& prefab, int node, std::string_view base_name) {
  std::vector<std::string> taken;
  for (const prefab::Socket& socket : prefab.sockets) {
    taken.push_back(socket.name);
  }
  prefab.sockets.push_back(
      {.name = MakeUniqueName(base_name, taken), .node = node, .local = {}});
  return static_cast<int>(prefab.sockets.size()) - 1;
}

int AddPointLight(Prefab& prefab, int node) {
  std::vector<std::string> taken;
  for (const prefab::PointLight& light : prefab.lights) {
    taken.push_back(light.name);
  }
  prefab.lights.push_back({.name = MakeUniqueName("light", taken),
                           .node = node,
                           .position = glm::vec3(0.0f),
                           .color = glm::vec3(1.0f),
                           .intensity = 1.0f,
                           .range = 10.0f});
  return static_cast<int>(prefab.lights.size()) - 1;
}

std::optional<std::string> AddChildPrefab(Prefab& prefab,
                                          const std::string& owner_path,
                                          std::string child_path, int node) {
  if (node < -1 || node >= static_cast<int>(prefab.hierarchy.nodes.size())) {
    return std::string("no such node");
  }
  if (std::optional<std::string> cycle = FindChildCycle(
          owner_path, child_path, ReadChildPathsFromFile, nullptr)) {
    return std::format("cannot add {}: {}", core::FileName(child_path), *cycle);
  }
  prefab.children.push_back(
      {.path = std::move(child_path), .node = node, .local = {}});
  return std::nullopt;
}

bool MoveToNode(Prefab& prefab, AttachmentKind kind, int index, int node) {
  if (node < -1 || node >= static_cast<int>(prefab.hierarchy.nodes.size())) {
    return false;
  }
  const glm::mat4 to_new_parent = glm::inverse(NodeWorldMatrix(prefab, node));
  // Re-expresses `local` (relative to `old_node`) relative to `node`.
  const auto carry = [&](int old_node, const animation::Transform& local) {
    return DecomposeMatrix(to_new_parent * NodeWorldMatrix(prefab, old_node) *
                           animation::ToMatrix(local));
  };
  switch (kind) {
    case AttachmentKind::kNode:
      return ReparentNode(prefab, index, node);
    case AttachmentKind::kSocket: {
      if (index < 0 || index >= static_cast<int>(prefab.sockets.size())) {
        return false;
      }
      prefab::Socket& socket = prefab.sockets[static_cast<size_t>(index)];
      socket.local = carry(socket.node, socket.local);
      socket.node = node;
      return true;
    }
    case AttachmentKind::kLight: {
      if (index < 0 || index >= static_cast<int>(prefab.lights.size())) {
        return false;
      }
      prefab::PointLight& light = prefab.lights[static_cast<size_t>(index)];
      light.position =
          glm::vec3(to_new_parent * NodeWorldMatrix(prefab, light.node) *
                    glm::vec4(light.position, 1.0f));
      light.node = node;
      return true;
    }
    case AttachmentKind::kChild: {
      if (index < 0 || index >= static_cast<int>(prefab.children.size())) {
        return false;
      }
      prefab::ChildPrefab& child = prefab.children[static_cast<size_t>(index)];
      child.local = carry(child.node, child.local);
      child.node = node;
      return true;
    }
  }
  return false;
}

std::optional<std::string> CheckBeforeSave(const Prefab& prefab) {
  if (std::optional<std::string> problem = prefab::ValidatePrefab(prefab)) {
    return problem;
  }
  for (size_t i = 0; i < prefab.sockets.size(); ++i) {
    if (prefab.sockets[i].name.empty()) {
      return std::format("socket {} has no name", i);
    }
    if (prefab::FindSocket(prefab, prefab.sockets[i].name) !=
        static_cast<int>(i)) {
      return std::format("two sockets are called \"{}\"",
                         prefab.sockets[i].name);
    }
  }
  for (size_t i = 0; i < prefab.clips.size(); ++i) {
    if (prefab::FindClip(prefab, prefab.clips[i].name) != static_cast<int>(i)) {
      return std::format("two clips are called \"{}\"", prefab.clips[i].name);
    }
  }
  const prefab::Physics& physics = prefab.physics;
  if (physics.kind != prefab::ColliderKind::kNone && physics.dynamic &&
      !(physics.mass > 0.0f)) {
    return std::string("a dynamic body needs a mass above 0");
  }
  if (physics.kind == prefab::ColliderKind::kTriangleMesh && physics.dynamic) {
    return std::string("a triangle-mesh collider must be static");
  }
  if ((physics.kind == prefab::ColliderKind::kConvexHull ||
       physics.kind == prefab::ColliderKind::kTriangleMesh) &&
      physics.cooked_shape.empty()) {
    return std::string("the collider shape is not cooked (press Re-cook)");
  }
  return std::nullopt;
}

}  // namespace blue::editor
