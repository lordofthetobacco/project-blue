#include "cook/model_cook.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/common.hpp>
#include <glm/ext/quaternion_trigonometric.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "animation/clip.h"
#include "animation/hierarchy.h"
#include "animation/transform.h"
#include "asset/animated_model.h"
#include "asset/image_decoder.h"
#include "asset/texture_loader.h"
#include "asset/mesh_data.h"
#include "core/log.h"
#include "core/path.h"
#include "material/material.h"

namespace blue::cook {
namespace {

using animation::Hierarchy;
using animation::Transform;
using asset::AnimatedModel;
using asset::MeshData;
using asset::ModelPart;
using material::MaterialDesc;
using material::TextureRef;
using Clock = std::chrono::steady_clock;

double MillisecondsSince(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start)
      .count();
}

CookError FromImportError(asset::ImportError error) {
  switch (error) {
    case asset::ImportError::kFileNotFound:
      return CookError::kFileNotFound;
    case asset::ImportError::kParseFailed:
      return CookError::kParseFailed;
    case asset::ImportError::kNoMeshes:
      return CookError::kNoMeshes;
    case asset::ImportError::kTooLarge:
      return CookError::kTooLarge;
  }
  return CookError::kParseFailed;
}

// ---------------------------------------------------------------------------
// Options text

std::string_view Trim(std::string_view text) {
  const auto is_space = [](char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  };
  while (!text.empty() && is_space(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && is_space(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

std::optional<bool> ParseBool(std::string_view text) {
  if (text == "true") {
    return true;
  }
  if (text == "false") {
    return false;
  }
  return std::nullopt;
}

template <typename T>
std::optional<T> ParseNumber(std::string_view text) {
  T value{};
  const auto [end, ec] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

// Applies one "key = value" line; returns false for a malformed value.
bool ApplyOption(ModelCookOptions& options, std::string_view key,
                 std::string_view value) {
  const auto set_bool = [&](bool& target) {
    const std::optional<bool> parsed = ParseBool(value);
    if (parsed.has_value()) {
      target = *parsed;
    }
    return parsed.has_value();
  };
  if (key == "scale") {
    const std::optional<float> scale = ParseNumber<float>(value);
    if (!scale.has_value() || !std::isfinite(*scale) || *scale <= 0.0f) {
      return false;
    }
    options.scale = *scale;
  } else if (key == "up_axis") {
    if (value != "y" && value != "z") {
      return false;
    }
    options.up_axis = value == "z" ? UpAxis::kZ : UpAxis::kY;
  } else if (key == "merge_static_nodes") {
    return set_bool(options.merge_static_nodes);
  } else if (key == "drop_node") {
    if (value.empty()) {
      return false;
    }
    options.drop_nodes.emplace_back(value);
  } else if (key == "use_material_overrides") {
    return set_bool(options.use_material_overrides);
  } else if (key == "optimize_meshes") {
    return set_bool(options.optimize_meshes);
  } else if (key == "texture_quality") {
    if (value != "fast" && value != "high") {
      return false;
    }
    options.texture_quality =
        value == "fast" ? CookQuality::kFast : CookQuality::kHigh;
  } else if (key == "compress_textures") {
    return set_bool(options.compress_textures);
  } else if (key == "max_texture_size") {
    const std::optional<int> size = ParseNumber<int>(value);
    if (!size.has_value() || *size < 0) {
      return false;
    }
    options.max_texture_size = *size;
  } else {
    core::log::Warn("model cook options: ignoring unknown key '{}'", key);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Hierarchy edits

// Removes the nodes named in `names` and their subtrees from `model`: nodes,
// parts and clip channels. Warns about names that match no node.
void DropNodes(AnimatedModel& model, const std::vector<std::string>& names,
               const std::string& path) {
  if (names.empty()) {
    return;
  }
  const std::vector<animation::Node>& nodes = model.hierarchy.nodes;
  std::vector<int> remap(nodes.size(), -1);
  std::vector<bool> matched(names.size(), false);
  Hierarchy kept;
  for (size_t i = 0; i < nodes.size(); ++i) {
    const animation::Node& node = nodes[i];
    const auto name = std::ranges::find(names, node.name);
    if (name != names.end()) {
      matched[static_cast<size_t>(name - names.begin())] = true;
      continue;
    }
    if (node.parent >= 0 && remap[static_cast<size_t>(node.parent)] < 0) {
      continue;  // inside a dropped subtree
    }
    remap[i] = static_cast<int>(kept.nodes.size());
    kept.nodes.push_back(
        {.name = node.name,
         .parent =
             node.parent < 0 ? -1 : remap[static_cast<size_t>(node.parent)],
         .rest = node.rest});
  }
  for (size_t i = 0; i < names.size(); ++i) {
    if (!matched[i]) {
      core::log::Warn("CookModel: {}: no node '{}' to drop", path, names[i]);
    }
  }

  std::vector<ModelPart> parts;
  for (ModelPart& part : model.parts) {
    const int node = remap[static_cast<size_t>(part.node)];
    if (node >= 0) {
      parts.push_back({.node = node, .mesh = std::move(part.mesh)});
    }
  }
  for (animation::Clip& clip : model.clips) {
    std::erase_if(clip.channels, [&](const animation::Channel& channel) {
      return channel.node < 0 ||
             static_cast<size_t>(channel.node) >= remap.size() ||
             remap[static_cast<size_t>(channel.node)] < 0;
    });
    for (animation::Channel& channel : clip.channels) {
      channel.node = remap[static_cast<size_t>(channel.node)];
    }
  }
  model.hierarchy = std::move(kept);
  model.parts = std::move(parts);
}

// Returns the transform that applies `options`' scale and up-axis fix.
Transform RootCorrection(const ModelCookOptions& options) {
  Transform correction{.scale = glm::vec3(options.scale)};
  if (options.up_axis == UpAxis::kZ) {
    correction.rotation =
        glm::angleAxis(-glm::half_pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f));
  }
  return correction;
}

bool NeedsCorrection(const ModelCookOptions& options) {
  return options.scale != 1.0f || options.up_axis != UpAxis::kY;
}

// Inserts a root node named `name` holding `correction` above every
// existing root of `model`, shifting all node indices by one.
void InsertRoot(AnimatedModel& model, const Transform& correction,
                std::string name) {
  std::vector<animation::Node>& nodes = model.hierarchy.nodes;
  for (animation::Node& node : nodes) {
    node.parent = node.parent < 0 ? 0 : node.parent + 1;
  }
  nodes.insert(nodes.begin(),
               {.name = std::move(name), .parent = -1, .rest = correction});
  for (ModelPart& part : model.parts) {
    ++part.node;
  }
  for (animation::Clip& clip : model.clips) {
    for (animation::Channel& channel : clip.channels) {
      ++channel.node;
    }
  }
}

std::vector<glm::mat4> RestWorldMatrices(const Hierarchy& hierarchy) {
  std::vector<Transform> pose(hierarchy.nodes.size());
  animation::ResetToRest(hierarchy, pose);
  std::vector<glm::mat4> world(pose.size());
  animation::ComputeWorldMatrices(hierarchy, pose, world);
  return world;
}

// Returns every part of `model` transformed by its rest world matrix times
// `correction` and merged into one mesh, one submesh per material in
// material order.
MeshData BakeStatic(const AnimatedModel& model, const glm::mat4& correction) {
  const std::vector<glm::mat4> world = RestWorldMatrices(model.hierarchy);
  MeshData merged;
  // Per part: the offset of its vertices in `merged`, and whether the
  // transform mirrors (flipping the winding).
  std::vector<uint32_t> base(model.parts.size());
  std::vector<bool> mirrored(model.parts.size());
  for (size_t p = 0; p < model.parts.size(); ++p) {
    const ModelPart& part = model.parts[p];
    const glm::mat4 matrix = correction * world[static_cast<size_t>(part.node)];
    const glm::mat3 linear(matrix);
    const glm::mat3 normal_matrix = glm::transpose(glm::inverse(linear));
    const float handedness = glm::determinant(linear) < 0.0f ? -1.0f : 1.0f;
    base[p] = static_cast<uint32_t>(merged.vertices.size());
    mirrored[p] = handedness < 0.0f;
    for (asset::Vertex vertex : part.mesh.vertices) {
      vertex.position = glm::vec3(matrix * glm::vec4(vertex.position, 1.0f));
      const glm::vec3 normal = normal_matrix * vertex.normal;
      vertex.normal = glm::length(normal) > 0.0f ? glm::normalize(normal)
                                                 : glm::vec3(0, 1, 0);
      const glm::vec3 tangent = linear * glm::vec3(vertex.tangent);
      vertex.tangent =
          glm::vec4(glm::length(tangent) > 0.0f ? glm::normalize(tangent)
                                                : glm::vec3(1, 0, 0),
                    vertex.tangent.w * handedness);
      merged.vertices.push_back(vertex);
    }
  }
  for (size_t material = 0; material < model.materials.size(); ++material) {
    const auto first = static_cast<uint32_t>(merged.indices.size());
    for (size_t p = 0; p < model.parts.size(); ++p) {
      const MeshData& mesh = model.parts[p].mesh;
      for (const asset::Submesh& submesh : mesh.submeshes) {
        if (submesh.material != static_cast<int>(material)) {
          continue;
        }
        for (uint32_t i = 0; i < submesh.index_count; i += 3) {
          const uint32_t* triangle = &mesh.indices[submesh.first_index + i];
          merged.indices.push_back(base[p] + triangle[0]);
          if (mirrored[p]) {
            merged.indices.push_back(base[p] + triangle[2]);
            merged.indices.push_back(base[p] + triangle[1]);
          } else {
            merged.indices.push_back(base[p] + triangle[1]);
            merged.indices.push_back(base[p] + triangle[2]);
          }
        }
      }
    }
    const auto count = static_cast<uint32_t>(merged.indices.size()) - first;
    if (count > 0) {
      merged.submeshes.push_back({.first_index = first,
                                  .index_count = count,
                                  .material = static_cast<int>(material)});
    }
  }
  merged.bounds = asset::ComputeBounds(merged.vertices);
  return merged;
}

// Reorders each submesh's triangles for the post-transform vertex cache,
// then the vertices for fetch locality (dropping unused ones).
void OptimizeMesh(MeshData& mesh) {
  for (const asset::Submesh& submesh : mesh.submeshes) {
    uint32_t* range = mesh.indices.data() + submesh.first_index;
    meshopt_optimizeVertexCache(range, range, submesh.index_count,
                                mesh.vertices.size());
  }
  std::vector<asset::Vertex> vertices(mesh.vertices.size());
  const size_t used = meshopt_optimizeVertexFetch(
      vertices.data(), mesh.indices.data(), mesh.indices.size(),
      mesh.vertices.data(), mesh.vertices.size(), sizeof(asset::Vertex));
  vertices.resize(used);
  mesh.vertices = std::move(vertices);
}

// ---------------------------------------------------------------------------
// Textures

std::string RefKey(const TextureRef& ref) {
  return ref.embedded() ? std::format("{}#{}", ref.path, ref.embedded_index)
                        : ref.path;
}

// Source images of a model, decoded once each.
class ImageTable {
 public:
  // Decodes every texture `materials` reference (embedded ones with one
  // parse of their model file). Fails with kTextureLoadFailed if any cannot
  // be decoded.
  core::Result<void, CookError> Load(
      const std::vector<MaterialDesc>& materials) {
    std::vector<TextureRef> refs;
    for (const MaterialDesc& desc : materials) {
      for (const TextureRef* ref :
           {&desc.base_color_texture, &desc.metallic_roughness_texture,
            &desc.roughness_texture, &desc.metallic_texture,
            &desc.normal_texture, &desc.occlusion_texture,
            &desc.emissive_texture}) {
        if (!ref->empty() && !index_.contains(RefKey(*ref))) {
          index_.emplace(RefKey(*ref), refs.size());
          refs.push_back(*ref);
        }
      }
    }
    auto loaded = asset::LoadTextures(refs);
    for (size_t i = 0; i < loaded.size(); ++i) {
      if (!loaded[i].ok()) {
        core::log::Error("CookModel: cannot load texture '{}': {}",
                         RefKey(refs[i]), asset::ToString(loaded[i].error()));
        return core::Result<void, CookError>::Error(
            CookError::kTextureLoadFailed);
      }
      images_.push_back(std::move(loaded[i]).value());
    }
    return core::Result<void, CookError>::Ok();
  }

  // Returns the image of `ref`, or nullptr for an empty ref.
  const asset::ImageData* Find(const TextureRef& ref) const {
    if (ref.empty()) {
      return nullptr;
    }
    const auto it = index_.find(RefKey(ref));
    return it == index_.end() ? nullptr : &images_[it->second];
  }

 private:
  std::map<std::string, size_t> index_;
  std::vector<asset::ImageData> images_;
};

// Cooks the textures of the materials, sharing each distinct (sources,
// usage) combination, and records their sources for EditInfo.
class TextureCooker {
 public:
  TextureCooker(const ImageTable& images, const ModelCookOptions& options,
                std::string model_stem, prefab::Prefab& prefab)
      : images_(images),
        options_(options),
        model_stem_(std::move(model_stem)),
        prefab_(prefab) {}

  // Returns the texture index of `ref` cooked for `usage`, or -1 for an
  // empty ref.
  core::Result<int, CookError> Single(const TextureRef& ref,
                                      TextureUsage usage) {
    if (ref.empty()) {
      return core::Result<int, CookError>::Ok(-1);
    }
    const std::string key =
        std::format("{}:{}", static_cast<int>(usage), RefKey(ref));
    if (const auto it = cooked_.find(key); it != cooked_.end()) {
      return core::Result<int, CookError>::Ok(it->second);
    }
    const asset::ImageData* image = images_.Find(ref);
    if (image == nullptr) {
      return core::Result<int, CookError>::Error(CookError::kTextureLoadFailed);
    }
    const std::string name =
        ref.embedded() ? std::format("{}_{}", model_stem_, ref.embedded_index)
                       : core::FileStem(ref.path);
    return Cook(key, *image, usage, name, {RefKey(ref)});
  }

  // Returns the packed ORM texture index of `desc`, or -1 if it has no
  // occlusion, roughness or metallic texture.
  core::Result<int, CookError> Orm(const MaterialDesc& desc) {
    OrmSources sources{.occlusion = images_.Find(desc.occlusion_texture)};
    std::vector<std::string> source_keys;
    if (!desc.occlusion_texture.empty()) {
      source_keys.push_back(RefKey(desc.occlusion_texture));
    }
    if (!desc.metallic_roughness_texture.empty()) {
      const asset::ImageData* packed =
          images_.Find(desc.metallic_roughness_texture);
      sources.roughness = packed;
      sources.roughness_channel = 1;
      sources.metallic = packed;
      sources.metallic_channel = 2;
      source_keys.push_back(RefKey(desc.metallic_roughness_texture));
    } else {
      sources.roughness = images_.Find(desc.roughness_texture);
      sources.metallic = images_.Find(desc.metallic_texture);
      for (const TextureRef* ref :
           {&desc.roughness_texture, &desc.metallic_texture}) {
        if (!ref->empty()) {
          source_keys.push_back(RefKey(*ref));
        }
      }
    }
    if (sources.occlusion == nullptr && sources.roughness == nullptr &&
        sources.metallic == nullptr) {
      return core::Result<int, CookError>::Ok(-1);
    }
    std::string key = "orm";
    for (const std::string& source : source_keys) {
      key += "|" + source;
    }
    // Separate maps and the glTF texture fill different channels.
    key += desc.metallic_roughness_texture.empty() ? "|separate" : "|gltf";
    if (const auto it = cooked_.find(key); it != cooked_.end()) {
      return core::Result<int, CookError>::Ok(it->second);
    }
    return Cook(key, PackOrm(sources), TextureUsage::kLinear,
                desc.name + "_orm", std::move(source_keys));
  }

 private:
  core::Result<int, CookError> Cook(const std::string& key,
                                    const asset::ImageData& image,
                                    TextureUsage usage, std::string name,
                                    std::vector<std::string> sources) {
    using Result = core::Result<int, CookError>;
    // Textures already in the prefab (an earlier AddMaterialToPrefab() call,
    // or a loaded prefab) are reused when made from the same sources in the
    // same format.
    const prefab::TextureFormat format = FormatFor(usage);
    for (size_t i = 0;
         i < prefab_.textures.size() && i < prefab_.edit.texture_sources.size();
         ++i) {
      if (prefab_.textures[i].format == format &&
          prefab_.edit.texture_sources[i] == sources) {
        cooked_.emplace(key, static_cast<int>(i));
        return Result::Ok(static_cast<int>(i));
      }
    }
    auto texture = CookTexture(image,
                               {.usage = usage,
                                .quality = options_.texture_quality,
                                .compress = options_.compress_textures,
                                .max_size = options_.max_texture_size},
                               prefab_.storage);
    if (!texture.ok()) {
      return Result::Error(texture.error());
    }
    const auto index = static_cast<int>(prefab_.textures.size());
    prefab::Texture cooked = std::move(texture).value();
    cooked.name = std::move(name);
    prefab_.textures.push_back(std::move(cooked));
    prefab_.edit.texture_sources.push_back(std::move(sources));
    cooked_.emplace(key, index);
    return Result::Ok(index);
  }

  prefab::TextureFormat FormatFor(TextureUsage usage) const {
    const bool compress = options_.compress_textures;
    switch (usage) {
      case TextureUsage::kColor:
        return compress ? prefab::TextureFormat::kBc7Srgb
                        : prefab::TextureFormat::kRgba8Srgb;
      case TextureUsage::kLinear:
        return compress ? prefab::TextureFormat::kBc7Unorm
                        : prefab::TextureFormat::kRgba8Unorm;
      case TextureUsage::kNormal:
        return compress ? prefab::TextureFormat::kBc5Unorm
                        : prefab::TextureFormat::kRgba8Unorm;
    }
    return prefab::TextureFormat::kRgba8Unorm;
  }

  const ImageTable& images_;
  const ModelCookOptions& options_;
  std::string model_stem_;
  prefab::Prefab& prefab_;
  std::map<std::string, int> cooked_;
};

// Converts `desc` into a prefab material, cooking its textures.
core::Result<prefab::Material, CookError> ConvertMaterial(
    const MaterialDesc& desc, TextureCooker& textures,
    const std::string& path) {
  using Result = core::Result<prefab::Material, CookError>;
  prefab::Material material{.name = desc.name,
                            .base_color = desc.base_color,
                            .metallic = desc.metallic,
                            .roughness = desc.roughness,
                            .emissive = desc.emissive,
                            .emissive_strength = desc.emissive_strength,
                            .normal_scale = desc.normal_scale,
                            .occlusion_strength = desc.occlusion_strength,
                            .uv_scale = desc.uv_scale,
                            .alpha_mode = desc.alpha_mode,
                            .alpha_cutoff = desc.alpha_cutoff,
                            .double_sided = desc.double_sided};
  const auto uv_of = [&](material::TextureSlot slot) {
    return static_cast<int>(desc.uv_set[static_cast<size_t>(slot)]);
  };
  const auto assign = [&](prefab::TextureSlot slot,
                          core::Result<int, CookError> index, int uv_set) {
    if (!index.ok()) {
      return false;
    }
    material.textures[static_cast<size_t>(slot)] = index.value();
    material.uv_sets[static_cast<size_t>(slot)] = uv_set;
    return true;
  };

  using material::TextureSlot;
  const int orm_uv = !desc.metallic_roughness_texture.empty() ||
                             !desc.roughness_texture.empty() ||
                             !desc.metallic_texture.empty()
                         ? uv_of(TextureSlot::kMetallicRoughness)
                         : uv_of(TextureSlot::kOcclusion);
  if (!desc.occlusion_texture.empty() &&
      uv_of(TextureSlot::kOcclusion) != orm_uv) {
    core::log::Warn(
        "CookModel: {}: material '{}': occlusion uses UV set {} but is packed "
        "with roughness/metallic on UV set {}",
        path, desc.name, uv_of(TextureSlot::kOcclusion), orm_uv);
  }
  const bool ok =
      assign(prefab::TextureSlot::kBaseColor,
             textures.Single(desc.base_color_texture, TextureUsage::kColor),
             uv_of(TextureSlot::kBaseColor)) &&
      assign(prefab::TextureSlot::kOrm, textures.Orm(desc), orm_uv) &&
      assign(prefab::TextureSlot::kNormal,
             textures.Single(desc.normal_texture, TextureUsage::kNormal),
             uv_of(TextureSlot::kNormal)) &&
      assign(prefab::TextureSlot::kEmissive,
             textures.Single(desc.emissive_texture, TextureUsage::kColor),
             uv_of(TextureSlot::kEmissive));
  if (!ok) {
    return Result::Error(CookError::kTextureLoadFailed);
  }
  return Result::Ok(std::move(material));
}

}  // namespace

asset::Bounds ComputePrefabBounds(const prefab::Prefab& prefab) {
  const std::vector<glm::mat4> world = RestWorldMatrices(prefab.hierarchy);
  bool empty = true;
  asset::Bounds result;
  for (const prefab::Mesh& mesh : prefab.meshes) {
    const glm::mat4 matrix = mesh.node >= 0
                                 ? world[static_cast<size_t>(mesh.node)]
                                 : glm::mat4(1.0f);
    for (int corner = 0; corner < 8; ++corner) {
      const glm::vec3 point{
          (corner & 1) != 0 ? mesh.bounds.max.x : mesh.bounds.min.x,
          (corner & 2) != 0 ? mesh.bounds.max.y : mesh.bounds.min.y,
          (corner & 4) != 0 ? mesh.bounds.max.z : mesh.bounds.min.z};
      const glm::vec3 moved = glm::vec3(matrix * glm::vec4(point, 1.0f));
      result.min = empty ? moved : glm::min(result.min, moved);
      result.max = empty ? moved : glm::max(result.max, moved);
      empty = false;
    }
  }
  return result;
}

void AddMeshToPrefab(prefab::Prefab& prefab, MeshData mesh, int node,
                     std::string name, bool optimize) {
  if (optimize) {
    OptimizeMesh(mesh);
  }
  mesh.bounds = asset::ComputeBounds(mesh.vertices);
  prefab::Mesh stored{.name = std::move(name),
                      .node = node,
                      .bounds = mesh.bounds,
                      .submeshes = std::move(mesh.submeshes)};
  stored.vertices =
      prefab.storage.StoreArray<asset::Vertex>(std::span(mesh.vertices));
  stored.indices =
      prefab.storage.StoreArray<asset::Index>(std::span(mesh.indices));
  prefab.meshes.push_back(std::move(stored));
}

core::Result<int, CookError> AddMaterialToPrefab(
    prefab::Prefab& prefab, const material::MaterialDesc& desc,
    const ModelCookOptions& options) {
  using Result = core::Result<int, CookError>;
  ImageTable images;
  if (auto loaded = images.Load({desc}); !loaded.ok()) {
    return Result::Error(loaded.error());
  }
  const std::string stem = desc.base_color_texture.empty()
                               ? desc.name
                               : core::FileStem(desc.base_color_texture.path);
  TextureCooker textures(images, options, stem, prefab);
  auto material =
      ConvertMaterial(material::Sanitize(desc), textures, desc.name);
  if (!material.ok()) {
    return Result::Error(material.error());
  }
  prefab.materials.push_back(std::move(material).value());
  return Result::Ok(static_cast<int>(prefab.materials.size()) - 1);
}

std::string FormatModelCookOptions(const ModelCookOptions& options) {
  std::string text = std::format(
      "scale = {}\n"
      "up_axis = {}\n"
      "merge_static_nodes = {}\n"
      "use_material_overrides = {}\n"
      "optimize_meshes = {}\n"
      "texture_quality = {}\n"
      "compress_textures = {}\n"
      "max_texture_size = {}\n",
      options.scale, options.up_axis == UpAxis::kZ ? "z" : "y",
      options.merge_static_nodes, options.use_material_overrides,
      options.optimize_meshes,
      options.texture_quality == CookQuality::kFast ? "fast" : "high",
      options.compress_textures, options.max_texture_size);
  for (const std::string& node : options.drop_nodes) {
    text += std::format("drop_node = {}\n", node);
  }
  return text;
}

core::Result<ModelCookOptions, CookError> ParseModelCookOptions(
    std::string_view text) {
  using Result = core::Result<ModelCookOptions, CookError>;
  ModelCookOptions options;
  while (!text.empty()) {
    const size_t end = text.find('\n');
    const std::string_view line = Trim(text.substr(0, end));
    text = end == std::string_view::npos ? std::string_view()
                                         : text.substr(end + 1);
    if (line.empty() || line.front() == '#') {
      continue;
    }
    const size_t equals = line.find('=');
    if (equals == std::string_view::npos) {
      return Result::Error(CookError::kInvalidArgument);
    }
    const std::string_view key = Trim(line.substr(0, equals));
    const std::string_view value = Trim(line.substr(equals + 1));
    if (!ApplyOption(options, key, value)) {
      core::log::Warn("model cook options: invalid {} '{}'", key, value);
      return Result::Error(CookError::kInvalidArgument);
    }
  }
  return Result::Ok(std::move(options));
}

core::Result<prefab::Prefab, CookError> CookModel(
    const std::string& path, const ModelCookOptions& options) {
  using Result = core::Result<prefab::Prefab, CookError>;
  if (!std::isfinite(options.scale) || options.scale <= 0.0f ||
      options.max_texture_size < 0) {
    return Result::Error(CookError::kInvalidArgument);
  }
  const Clock::time_point start = Clock::now();

  auto imported = asset::ImportAnimatedModel(
      path, {.use_material_overrides = options.use_material_overrides});
  if (!imported.ok()) {
    return Result::Error(FromImportError(imported.error()));
  }
  AnimatedModel model = std::move(imported).value();
  const double import_ms = MillisecondsSince(start);

  const Clock::time_point mesh_start = Clock::now();
  DropNodes(model, options.drop_nodes, path);
  if (model.parts.empty()) {
    return Result::Error(CookError::kNoMeshes);
  }

  prefab::Prefab prefab;
  const std::string stem = core::FileStem(path);
  if (options.merge_static_nodes) {
    if (!model.clips.empty()) {
      core::log::Warn("CookModel: {}: merging static nodes drops {} clips",
                      path, model.clips.size());
    }
    AddMeshToPrefab(
        prefab, BakeStatic(model, animation::ToMatrix(RootCorrection(options))),
        -1, stem, options.optimize_meshes);
  } else {
    if (NeedsCorrection(options)) {
      InsertRoot(model, RootCorrection(options), stem);
    }
    for (ModelPart& part : model.parts) {
      std::string name =
          model.hierarchy.nodes[static_cast<size_t>(part.node)].name;
      AddMeshToPrefab(prefab, std::move(part.mesh), part.node, std::move(name),
                      options.optimize_meshes);
    }
    prefab.hierarchy = std::move(model.hierarchy);
    prefab.clips = std::move(model.clips);
    for (animation::Clip& clip : prefab.clips) {
      clip.loop = false;
    }
  }
  prefab.bounds = ComputePrefabBounds(prefab);
  const double mesh_ms = MillisecondsSince(mesh_start);

  const Clock::time_point texture_start = Clock::now();
  ImageTable images;
  if (auto loaded = images.Load(model.materials); !loaded.ok()) {
    return Result::Error(loaded.error());
  }
  TextureCooker textures(images, options, stem, prefab);
  prefab.materials.reserve(model.materials.size());
  for (const MaterialDesc& desc : model.materials) {
    auto material = ConvertMaterial(desc, textures, path);
    if (!material.ok()) {
      return Result::Error(material.error());
    }
    prefab.materials.push_back(std::move(material).value());
  }
  const double texture_ms = MillisecondsSince(texture_start);

  prefab.edit.source_model = path;
  prefab.edit.import_settings = FormatModelCookOptions(options);

  if (const std::optional<std::string> problem =
          prefab::ValidatePrefab(prefab)) {
    core::log::Error("CookModel: {}: invalid result: {}", path, *problem);
    return Result::Error(CookError::kInvalidArgument);
  }

  size_t vertex_count = 0;
  size_t triangle_count = 0;
  for (const prefab::Mesh& mesh : prefab.meshes) {
    vertex_count += mesh.vertices.size();
    triangle_count += mesh.indices.size() / 3;
  }
  core::log::Info(
      "cooked {}: {} nodes, {} meshes, {} vertices, {} triangles, {} "
      "materials, {} textures, {} clips in {:.1f} ms (import {:.1f}, meshes "
      "{:.1f}, textures {:.1f})",
      core::FileName(path), prefab.hierarchy.nodes.size(), prefab.meshes.size(),
      vertex_count, triangle_count, prefab.materials.size(),
      prefab.textures.size(), prefab.clips.size(), MillisecondsSince(start),
      import_ms, mesh_ms, texture_ms);
  return Result::Ok(std::move(prefab));
}

}  // namespace blue::cook
