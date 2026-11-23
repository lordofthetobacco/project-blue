#include "cook/model_cook.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
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
#include "asset/mesh_data.h"
#include "asset/texture_loader.h"
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

// Upper bound on the estimated working set of the textures cooked at once
// (decoded sources, ORM packing, the filter's next level and the output):
// two 4k textures at a time, or one with several sources.
constexpr size_t kTextureMemoryBudget = size_t{512} * 1024 * 1024;

// Threads cooking textures at once, each encode using every core itself;
// more only overlap the single-threaded image decoding.
constexpr int kMaxTextureThreads = 3;

// The encoded source images of a cook, decoded one at a time on demand and
// freed after their last use. Embedded images come from the import (or one
// parse per other model file a sidecar material names); image files are read
// when needed. Once every AddUse() call is made, Probe(), Decode() and Done()
// may run concurrently.
class ImageSources {
 public:
  // `embedded` holds the embedded images of the model at `model_path`; an
  // empty path means there is no model.
  ImageSources(std::string model_path, asset::EmbeddedImages embedded) {
    if (!model_path.empty()) {
      models_.emplace(std::move(model_path),
                      ModelImages{.images = std::move(embedded)});
    }
  }

  // Registers one use of `ref` by a texture job, opening model files not
  // seen yet. Call for every use before the jobs run.
  void AddUse(const TextureRef& ref) {
    if (!ref.embedded()) {
      return;
    }
    auto found = models_.find(ref.path);
    if (found == models_.end()) {
      auto loaded = asset::LoadEmbeddedImages(ref.path);
      const bool opened = loaded.ok();
      found =
          models_
              .emplace(ref.path,
                       ModelImages{.images = opened ? std::move(loaded).value()
                                                    : asset::EmbeddedImages(),
                                   .opened = opened})
              .first;
    }
    ++found->second.uses[ref.embedded_index];
  }

  // Returns the decoded size of `ref`.
  core::Result<asset::ImageSize, asset::ImageError> Probe(
      const TextureRef& ref) const {
    if (!ref.embedded()) {
      return asset::ProbeImageFile(ref.path);
    }
    const ModelImages& model = models_.at(ref.path);
    if (!model.opened) {
      return core::Result<asset::ImageSize, asset::ImageError>::Error(
          asset::ImageError::kFileNotFound);
    }
    return model.images.Probe(ref.embedded_index);
  }

  core::Result<asset::ImageData, asset::ImageError> Decode(
      const TextureRef& ref) const {
    if (!ref.embedded()) {
      return asset::LoadImageFile(ref.path);
    }
    const ModelImages& model = models_.at(ref.path);
    if (!model.opened) {
      return core::Result<asset::ImageData, asset::ImageError>::Error(
          asset::ImageError::kFileNotFound);
    }
    return model.images.Decode(ref.embedded_index);
  }

  // Ends one use of `ref`; frees an embedded image after its last use.
  void Done(const TextureRef& ref) {
    if (!ref.embedded()) {
      return;
    }
    ModelImages& model = models_.at(ref.path);
    const std::lock_guard lock(mutex_);
    int& uses = model.uses.at(ref.embedded_index);
    if (--uses == 0) {
      model.images.Release(ref.embedded_index);
    }
  }

 private:
  struct ModelImages {
    asset::EmbeddedImages images;
    bool opened = true;
    std::map<int, int> uses;  // embedded index -> uses not done yet
  };

  std::map<std::string, ModelImages, std::less<>> models_;
  std::mutex mutex_;
};

// Holds texture jobs to the memory budget: Acquire() blocks until the
// estimate fits next to the jobs already running, or nothing else runs.
class MemoryBudget {
 public:
  explicit MemoryBudget(size_t limit) : limit_(limit) {}

  void Acquire(size_t bytes) {
    std::unique_lock lock(mutex_);
    released_.wait(lock,
                   [&] { return in_use_ == 0 || in_use_ + bytes <= limit_; });
    in_use_ += bytes;
    peak_ = std::max(peak_, in_use_);
  }

  void Release(size_t bytes) {
    {
      const std::lock_guard lock(mutex_);
      in_use_ -= bytes;
    }
    released_.notify_all();
  }

  // Returns the highest total estimate held at once.
  size_t peak() const {
    const std::lock_guard lock(mutex_);
    return peak_;
  }

 private:
  const size_t limit_;
  mutable std::mutex mutex_;
  std::condition_variable released_;
  size_t in_use_ = 0;
  size_t peak_ = 0;
};

// One texture to cook: a single source image, or up to three packed into an
// ORM texture (R occlusion, G roughness, B metallic, each from a channel of
// its source; an empty ref leaves the channel 255).
struct TextureJob {
  int index = -1;  // into prefab.textures
  TextureUsage usage = TextureUsage::kColor;
  bool orm = false;
  std::array<TextureRef, 3> refs;  // single: refs[0] only
  std::array<int, 3> channels{};
};

// Returns true if job.refs[i] is not empty and not a repeat of an earlier
// ref of the job (the distinct sources to decode).
bool IsFirstUse(const TextureJob& job, size_t i) {
  const auto begin = job.refs.begin();
  const auto end = begin + static_cast<std::ptrdiff_t>(i);
  return !job.refs[i].empty() && std::find(begin, end, job.refs[i]) == end;
}

// Cooks the textures of the materials, sharing each distinct (sources,
// usage) combination, and records their sources for EditInfo. Single() and
// Orm() assign texture indices in call order and queue the work; Run()
// cooks the queue with bounded memory and stores the results in index
// order, so the output does not depend on thread timing.
class TextureCooker {
 public:
  TextureCooker(ImageSources& sources, const ModelCookOptions& options,
                std::string model_stem, prefab::Prefab& prefab)
      : sources_(sources),
        options_(options),
        model_stem_(std::move(model_stem)),
        prefab_(prefab) {}

  // Returns the texture index of `ref` cooked for `usage`, or -1 for an
  // empty ref.
  int Single(const TextureRef& ref, TextureUsage usage) {
    if (ref.empty()) {
      return -1;
    }
    const std::string key =
        std::format("{}:{}", static_cast<int>(usage), RefKey(ref));
    if (const auto it = cooked_.find(key); it != cooked_.end()) {
      return it->second;
    }
    std::string name =
        ref.embedded() ? std::format("{}_{}", model_stem_, ref.embedded_index)
                       : core::FileStem(ref.path);
    return Queue(key, TextureJob{.usage = usage, .refs = {ref}},
                 std::move(name), {RefKey(ref)});
  }

  // Returns the packed ORM texture index of `desc`, or -1 if it has no
  // occlusion, roughness or metallic texture.
  int Orm(const MaterialDesc& desc) {
    TextureJob job{.usage = TextureUsage::kLinear, .orm = true};
    job.refs[0] = desc.occlusion_texture;
    std::vector<std::string> source_keys;
    if (!desc.occlusion_texture.empty()) {
      source_keys.push_back(RefKey(desc.occlusion_texture));
    }
    if (!desc.metallic_roughness_texture.empty()) {
      job.refs[1] = desc.metallic_roughness_texture;
      job.refs[2] = desc.metallic_roughness_texture;
      job.channels[1] = 1;
      job.channels[2] = 2;
      source_keys.push_back(RefKey(desc.metallic_roughness_texture));
    } else {
      job.refs[1] = desc.roughness_texture;
      job.refs[2] = desc.metallic_texture;
      for (const TextureRef* ref :
           {&desc.roughness_texture, &desc.metallic_texture}) {
        if (!ref->empty()) {
          source_keys.push_back(RefKey(*ref));
        }
      }
    }
    if (source_keys.empty()) {
      return -1;
    }
    std::string key = "orm";
    for (const std::string& source : source_keys) {
      key += "|" + source;
    }
    // Separate maps and the glTF texture fill different channels.
    key += desc.metallic_roughness_texture.empty() ? "|separate" : "|gltf";
    if (const auto it = cooked_.find(key); it != cooked_.end()) {
      return it->second;
    }
    return Queue(key, std::move(job), desc.name + "_orm",
                 std::move(source_keys));
  }

  // Cooks every queued texture into its slot of prefab.textures. Fails with
  // kTextureLoadFailed when a source cannot be decoded, or the error of
  // EncodeTexture(). Logs timings.
  core::Result<void, CookError> Run() {
    using Result = core::Result<void, CookError>;
    if (jobs_.empty()) {
      return Result::Ok();
    }
    const Clock::time_point start = Clock::now();
    for (const TextureJob& job : jobs_) {
      for (size_t i = 0; i < job.refs.size(); ++i) {
        if (IsFirstUse(job, i)) {
          sources_.AddUse(job.refs[i]);
        }
      }
    }
    std::vector<std::optional<EncodedTexture>> results(jobs_.size());
    MemoryBudget budget(kTextureMemoryBudget);
    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    std::atomic<CookError> error{CookError::kTextureLoadFailed};
    std::atomic<int64_t> decode_us{0};
    const auto work = [&] {
      for (size_t i = next.fetch_add(1); i < jobs_.size() && !failed;
           i = next.fetch_add(1)) {
        auto encoded = RunJob(jobs_[i], budget, decode_us);
        if (!encoded.ok()) {
          error = encoded.error();
          failed = true;
          return;
        }
        results[i] = std::move(encoded).value();
      }
    };
    const int thread_count =
        std::min(kMaxTextureThreads, static_cast<int>(jobs_.size()));
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(thread_count - 1));
    for (int t = 1; t < thread_count; ++t) {
      threads.emplace_back(work);
    }
    work();
    for (std::thread& thread : threads) {
      thread.join();
    }
    if (failed) {
      jobs_.clear();
      return Result::Error(error.load());
    }
    for (size_t i = 0; i < jobs_.size(); ++i) {
      prefab::Texture& slot =
          prefab_.textures[static_cast<size_t>(jobs_[i].index)];
      std::string name = std::move(slot.name);
      slot = StoreTexture(std::move(*results[i]), prefab_.storage);
      slot.name = std::move(name);
    }
    core::log::Info(
        "cooked {} textures on {} threads in {:.1f} ms (decoding {:.1f} ms "
        "summed over threads, peak budget estimate {} MiB)",
        jobs_.size(), thread_count, MillisecondsSince(start),
        static_cast<double>(decode_us.load()) / 1000.0,
        budget.peak() / (size_t{1024} * 1024));
    jobs_.clear();
    return Result::Ok();
  }

 private:
  // Reserves the next texture index for `job` unless a texture already in
  // the prefab (an earlier AddMaterialToPrefab() call, or a loaded prefab)
  // was made from the same sources in the same format.
  int Queue(const std::string& key, TextureJob job, std::string name,
            std::vector<std::string> sources) {
    const prefab::TextureFormat format = FormatFor(job.usage);
    for (size_t i = 0;
         i < prefab_.textures.size() && i < prefab_.edit.texture_sources.size();
         ++i) {
      if (prefab_.textures[i].format == format &&
          prefab_.edit.texture_sources[i] == sources) {
        cooked_.emplace(key, static_cast<int>(i));
        return static_cast<int>(i);
      }
    }
    job.index = static_cast<int>(prefab_.textures.size());
    prefab_.textures.push_back({.name = std::move(name), .format = format});
    prefab_.edit.texture_sources.push_back(std::move(sources));
    cooked_.emplace(key, job.index);
    jobs_.push_back(std::move(job));
    return jobs_.back().index;
  }

  // Returns the budget estimate of `job`: its decoded sources plus the
  // decoder's copy of the largest, the packed ORM image, and the encoder's
  // next level as floats and output (about 1.5 times the RGBA8 target).
  size_t Estimate(const TextureJob& job) const {
    size_t sources = 0;
    size_t largest = 0;
    for (size_t i = 0; i < job.refs.size(); ++i) {
      if (!IsFirstUse(job, i)) {
        continue;
      }
      const auto size = sources_.Probe(job.refs[i]);
      if (!size.ok()) {
        return 0;  // the decode fails and reports it
      }
      const size_t bytes = size_t{4} * static_cast<size_t>(size.value().width) *
                           static_cast<size_t>(size.value().height);
      sources += bytes;
      largest = std::max(largest, bytes);
    }
    return sources + largest + (job.orm ? largest : 0) + largest * 3 / 2;
  }

  // Decodes the sources of `job` within the memory budget, packs them for
  // ORM and encodes the result.
  core::Result<EncodedTexture, CookError> RunJob(
      const TextureJob& job, MemoryBudget& budget,
      std::atomic<int64_t>& decode_us) {
    using Result = core::Result<EncodedTexture, CookError>;
    const size_t estimate = Estimate(job);
    budget.Acquire(estimate);

    const Clock::time_point decode_start = Clock::now();
    std::array<asset::ImageData, 3> images;
    bool decoded = true;
    for (size_t i = 0; i < job.refs.size(); ++i) {
      if (!IsFirstUse(job, i)) {
        continue;
      }
      auto image = sources_.Decode(job.refs[i]);
      sources_.Done(job.refs[i]);
      if (!image.ok()) {
        core::log::Error("CookModel: cannot load texture '{}': {}",
                         RefKey(job.refs[i]), asset::ToString(image.error()));
        decoded = false;
        break;
      }
      images[i] = std::move(image).value();
    }
    decode_us += std::chrono::duration_cast<std::chrono::microseconds>(
                     Clock::now() - decode_start)
                     .count();
    if (!decoded) {
      budget.Release(estimate);
      return Result::Error(CookError::kTextureLoadFailed);
    }

    asset::ImageData image;
    if (job.orm) {
      // The image of channel c: its own, or that of the earlier channel with
      // the same source.
      const auto source_of = [&](size_t c) -> const asset::ImageData* {
        if (job.refs[c].empty()) {
          return nullptr;
        }
        const auto first =
            std::find(job.refs.begin(), job.refs.end(), job.refs[c]);
        return &images[static_cast<size_t>(first - job.refs.begin())];
      };
      image = PackOrm({.occlusion = source_of(0),
                       .occlusion_channel = job.channels[0],
                       .roughness = source_of(1),
                       .roughness_channel = job.channels[1],
                       .metallic = source_of(2),
                       .metallic_channel = job.channels[2]});
      images = {};
    } else {
      image = std::move(images[0]);
    }
    auto encoded = EncodeTexture(std::move(image),
                                 {.usage = job.usage,
                                  .quality = options_.texture_quality,
                                  .compress = options_.compress_textures,
                                  .max_size = options_.max_texture_size});
    budget.Release(estimate);
    return encoded;
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

  ImageSources& sources_;
  const ModelCookOptions& options_;
  std::string model_stem_;
  prefab::Prefab& prefab_;
  std::map<std::string, int> cooked_;
  std::vector<TextureJob> jobs_;
};

// Converts `desc` into a prefab material, queueing its textures on
// `textures` (cooked by TextureCooker::Run()).
prefab::Material ConvertMaterial(const MaterialDesc& desc,
                                 TextureCooker& textures,
                                 const std::string& path) {
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
  const auto assign = [&](prefab::TextureSlot slot, int index, int uv_set) {
    material.textures[static_cast<size_t>(slot)] = index;
    material.uv_sets[static_cast<size_t>(slot)] = uv_set;
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
  // The call order (base colour, ORM, normal, emissive) decides the texture
  // indices.
  assign(prefab::TextureSlot::kBaseColor,
         textures.Single(desc.base_color_texture, TextureUsage::kColor),
         uv_of(TextureSlot::kBaseColor));
  assign(prefab::TextureSlot::kOrm, textures.Orm(desc), orm_uv);
  assign(prefab::TextureSlot::kNormal,
         textures.Single(desc.normal_texture, TextureUsage::kNormal),
         uv_of(TextureSlot::kNormal));
  assign(prefab::TextureSlot::kEmissive,
         textures.Single(desc.emissive_texture, TextureUsage::kColor),
         uv_of(TextureSlot::kEmissive));
  return material;
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
  const std::string stem = desc.base_color_texture.empty()
                               ? desc.name
                               : core::FileStem(desc.base_color_texture.path);
  ImageSources sources("", asset::EmbeddedImages());
  TextureCooker textures(sources, options, stem, prefab);
  const size_t texture_count = prefab.textures.size();
  const size_t source_count = prefab.edit.texture_sources.size();
  prefab::Material material =
      ConvertMaterial(material::Sanitize(desc), textures, desc.name);
  if (auto cooked = textures.Run(); !cooked.ok()) {
    // Drop the slots reserved for this material's textures.
    prefab.textures.resize(texture_count);
    prefab.edit.texture_sources.resize(source_count);
    return Result::Error(cooked.error());
  }
  prefab.materials.push_back(std::move(material));
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

  // The import hands over the embedded images still encoded: they are
  // decoded one texture at a time below, without parsing the file again.
  asset::EmbeddedImages embedded;
  auto imported = asset::ImportAnimatedModel(
      path, {.use_material_overrides = options.use_material_overrides},
      &embedded);
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
  model.parts = {};  // the meshes now live in prefab.storage
  const double mesh_ms = MillisecondsSince(mesh_start);

  const Clock::time_point texture_start = Clock::now();
  const size_t embedded_bytes = embedded.byte_size();
  ImageSources sources(path, std::move(embedded));
  TextureCooker textures(sources, options, stem, prefab);
  prefab.materials.reserve(model.materials.size());
  for (const MaterialDesc& desc : model.materials) {
    prefab.materials.push_back(ConvertMaterial(desc, textures, path));
  }
  if (auto cooked = textures.Run(); !cooked.ok()) {
    return Result::Error(cooked.error());
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
      "{:.1f}, textures {:.1f}; {} MiB of encoded images held after import)",
      core::FileName(path), prefab.hierarchy.nodes.size(), prefab.meshes.size(),
      vertex_count, triangle_count, prefab.materials.size(),
      prefab.textures.size(), prefab.clips.size(), MillisecondsSince(start),
      import_ms, mesh_ms, texture_ms, embedded_bytes / (size_t{1024} * 1024));
  return Result::Ok(std::move(prefab));
}

}  // namespace blue::cook
