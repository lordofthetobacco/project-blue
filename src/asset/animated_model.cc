#include "asset/animated_model.h"

#include <assimp/anim.h>
#include <assimp/mesh.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <assimp/Importer.hpp>
#include <glm/common.hpp>
#include <glm/ext/quaternion_geometric.hpp>
#include <glm/vec4.hpp>
#include <glm/vector_relational.hpp>

#include "asset/internal/material_import.h"
#include "asset/internal/mesh_import.h"
#include "core/log.h"
#include "core/path.h"
#include "platform/file_system.h"

namespace blue::asset {
namespace {

using animation::Channel;
using animation::Clip;
using animation::Hierarchy;
using animation::Interpolation;
using animation::TargetPath;
using animation::Transform;

// Like ImportModel() minus aiProcess_PreTransformVertices, which would bake
// the node transforms and drop the hierarchy.
constexpr unsigned kPostProcess = internal::kBasePostProcess;

// Assimp's convention for files that do not state a tick rate.
constexpr double kDefaultTicksPerSecond = 25.0;

// Tolerance for recognising a key that only repeats the rest transform.
constexpr float kRestTolerance = 1.0e-5f;

Transform Decompose(const aiMatrix4x4& matrix) {
  aiVector3D scale;
  aiQuaternion rotation;
  aiVector3D translation;
  matrix.Decompose(scale, rotation, translation);
  return {.translation = {translation.x, translation.y, translation.z},
          .rotation = glm::normalize(
              glm::quat(rotation.w, rotation.x, rotation.y, rotation.z)),
          .scale = {scale.x, scale.y, scale.z}};
}

// Returns the node hierarchy in depth-first pre-order, so every parent
// precedes its children. `ai_nodes` receives the assimp node of each entry.
Hierarchy BuildHierarchy(const aiNode& root,
                         std::vector<const aiNode*>& ai_nodes) {
  Hierarchy hierarchy;
  std::vector<std::pair<const aiNode*, int>> stack{{&root, -1}};
  while (!stack.empty()) {
    const auto [node, parent] = stack.back();
    stack.pop_back();
    const auto index = static_cast<int>(hierarchy.nodes.size());
    hierarchy.nodes.push_back({.name = node->mName.C_Str(),
                               .parent = parent,
                               .rest = Decompose(node->mTransformation)});
    ai_nodes.push_back(node);
    // Reversed so children keep their file order.
    for (unsigned i = node->mNumChildren; i > 0; --i) {
      stack.emplace_back(node->mChildren[i - 1], index);
    }
  }
  return hierarchy;
}

// Returns the mesh of the usable meshes of `node`, nullopt if it has none,
// or kTooLarge if they overflow Index.
core::Result<std::optional<MeshData>, ImportError> ImportNodeMesh(
    const std::string& path, const aiScene& scene, const aiNode& node,
    internal::MaterialTable& materials) {
  using Result = core::Result<std::optional<MeshData>, ImportError>;
  std::vector<const aiMesh*> candidates;
  candidates.reserve(node.mNumMeshes);
  for (const unsigned mesh_index : std::span(node.mMeshes, node.mNumMeshes)) {
    candidates.push_back(scene.mMeshes[mesh_index]);
  }
  const std::vector<const aiMesh*> meshes =
      internal::UsableMeshes(candidates, "ImportAnimatedModel: " + path);
  if (meshes.empty()) {
    return Result::Ok(std::nullopt);
  }
  if (internal::VertexCount(meshes) > std::numeric_limits<Index>::max()) {
    return Result::Error(ImportError::kTooLarge);
  }
  return Result::Ok(internal::BuildMesh(meshes, materials));
}

// Returns true if `values` (one element) equals the `path` component of
// `rest`. Assimp's glTF importer adds such a single key for every
// untargeted component that the node sets explicitly; importing it would
// override the caller's pose for no reason.
bool RepeatsRest(TargetPath path, std::span<const float> values,
                 const Transform& rest) {
  if (path == TargetPath::kRotation) {
    const glm::quat q(values[3], values[0], values[1], values[2]);
    return std::abs(std::abs(glm::dot(q, rest.rotation)) - 1.0f) <=
           kRestTolerance;
  }
  const glm::vec3 expected =
      path == TargetPath::kTranslation ? rest.translation : rest.scale;
  const glm::vec3 actual{values[0], values[1], values[2]};
  return glm::all(glm::lessThanEqual(glm::abs(actual - expected),
                                     glm::vec3(kRestTolerance)));
}

// Converts the keys of one component of an assimp node animation. Key
// values are read through `read`, which appends one element per key.
template <typename Key, typename ReadValue>
std::optional<Channel> MakeChannel(int node, TargetPath path,
                                   std::span<const Key> keys,
                                   double ticks_per_second,
                                   const Transform& rest, ReadValue read) {
  Channel channel{
      .node = node, .path = path, .interpolation = Interpolation::kLinear};
  channel.times.reserve(keys.size());
  channel.values.reserve(keys.size() *
                         static_cast<size_t>(animation::ComponentCount(path)));
  for (const Key& key : keys) {
    channel.times.push_back(static_cast<float>(key.mTime / ticks_per_second));
    read(key.mValue, channel.values);
  }
  if (keys.empty() || (keys.size() == 1 && channel.times[0] == 0.0f &&
                       RepeatsRest(path, channel.values, rest))) {
    return std::nullopt;
  }
  return channel;
}

void ReadVector(const aiVector3D& value, std::vector<float>& values) {
  values.insert(values.end(), {value.x, value.y, value.z});
}

void ReadQuaternion(const aiQuaternion& value, std::vector<float>& values) {
  values.insert(values.end(), {value.x, value.y, value.z, value.w});
}

// Converts one assimp animation. Channels for nodes not in `hierarchy` and
// channels with invalid keys are dropped with a warning.
Clip ImportClip(const std::string& path, const aiAnimation& animation,
                const Hierarchy& hierarchy) {
  const double ticks_per_second = animation.mTicksPerSecond > 0.0
                                      ? animation.mTicksPerSecond
                                      : kDefaultTicksPerSecond;
  Clip clip{
      .name = animation.mName.C_Str(),
      .duration = static_cast<float>(animation.mDuration / ticks_per_second)};
  for (const aiNodeAnim* node_anim :
       std::span(animation.mChannels, animation.mNumChannels)) {
    const int node =
        animation::FindNode(hierarchy, node_anim->mNodeName.C_Str());
    if (node < 0) {
      core::log::Warn(
          "ImportAnimatedModel: {}: clip '{}' targets unknown node "
          "'{}'",
          path, clip.name, node_anim->mNodeName.C_Str());
      continue;
    }
    const Transform& rest = hierarchy.nodes[static_cast<size_t>(node)].rest;
    std::array<std::optional<Channel>, 3> channels = {
        MakeChannel(node, TargetPath::kTranslation,
                    std::span<const aiVectorKey>(node_anim->mPositionKeys,
                                                 node_anim->mNumPositionKeys),
                    ticks_per_second, rest, ReadVector),
        MakeChannel(node, TargetPath::kRotation,
                    std::span<const aiQuatKey>(node_anim->mRotationKeys,
                                               node_anim->mNumRotationKeys),
                    ticks_per_second, rest, ReadQuaternion),
        MakeChannel(node, TargetPath::kScale,
                    std::span<const aiVectorKey>(node_anim->mScalingKeys,
                                                 node_anim->mNumScalingKeys),
                    ticks_per_second, rest, ReadVector)};
    for (std::optional<Channel>& channel : channels) {
      if (!channel.has_value()) {
        continue;
      }
      if (!animation::IsValid(*channel)) {
        core::log::Warn(
            "ImportAnimatedModel: {}: clip '{}': dropping a "
            "channel of node '{}' with invalid keys",
            path, clip.name, node_anim->mNodeName.C_Str());
        continue;
      }
      clip.channels.push_back(std::move(*channel));
    }
  }
  return clip;
}

// Returns the box around every part's vertices at the rest pose.
Bounds RestBounds(const AnimatedModel& model) {
  std::vector<Transform> pose(model.hierarchy.nodes.size());
  animation::ResetToRest(model.hierarchy, pose);
  std::vector<glm::mat4> world(pose.size());
  animation::ComputeWorldMatrices(model.hierarchy, pose, world);

  bool empty = true;
  Bounds bounds;
  for (const ModelPart& part : model.parts) {
    const glm::mat4& matrix = world[static_cast<size_t>(part.node)];
    for (const Vertex& vertex : part.mesh.vertices) {
      const glm::vec3 point = glm::vec3(matrix * glm::vec4(vertex.position, 1));
      bounds.min = empty ? point : glm::min(bounds.min, point);
      bounds.max = empty ? point : glm::max(bounds.max, point);
      empty = false;
    }
  }
  return bounds;
}

}  // namespace

int FindClip(const AnimatedModel& model, std::string_view name) {
  for (size_t i = 0; i < model.clips.size(); ++i) {
    if (model.clips[i].name == name) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

Bounds PoseBounds(std::span<const ModelPart> parts,
                  std::span<const glm::mat4> world) {
  bool empty = true;
  Bounds result;
  for (const ModelPart& part : parts) {
    if (part.node < 0 || static_cast<size_t>(part.node) >= world.size()) {
      continue;
    }
    const glm::mat4& matrix = world[static_cast<size_t>(part.node)];
    const Bounds& local = part.mesh.bounds;
    for (int corner = 0; corner < 8; ++corner) {
      const glm::vec3 point{(corner & 1) != 0 ? local.max.x : local.min.x,
                            (corner & 2) != 0 ? local.max.y : local.min.y,
                            (corner & 4) != 0 ? local.max.z : local.min.z};
      const glm::vec3 moved = glm::vec3(matrix * glm::vec4(point, 1.0f));
      result.min = empty ? moved : glm::min(result.min, moved);
      result.max = empty ? moved : glm::max(result.max, moved);
      empty = false;
    }
  }
  return result;
}

core::Result<AnimatedModel, ImportError> ImportAnimatedModel(
    const std::string& path, const ImportOptions& options) {
  using Result = core::Result<AnimatedModel, ImportError>;
  if (!platform::IsRegularFile(path)) {
    return Result::Error(ImportError::kFileNotFound);
  }

  const auto start = std::chrono::steady_clock::now();

  Assimp::Importer importer;
  importer.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE,
                              aiPrimitiveType_POINT | aiPrimitiveType_LINE);
  const aiScene* scene = importer.ReadFile(path.c_str(), kPostProcess);
  if (scene == nullptr || scene->mRootNode == nullptr) {
    core::log::Error("ImportAnimatedModel: {}: {}", path,
                     importer.GetErrorString());
    return Result::Error(ImportError::kParseFailed);
  }

  AnimatedModel model;
  std::vector<const aiNode*> ai_nodes;
  model.hierarchy = BuildHierarchy(*scene->mRootNode, ai_nodes);

  internal::MaterialTable materials(*scene, path,
                                    options.use_material_overrides);
  for (size_t i = 0; i < ai_nodes.size(); ++i) {
    auto mesh = ImportNodeMesh(path, *scene, *ai_nodes[i], materials);
    if (!mesh.ok()) {
      return Result::Error(mesh.error());
    }
    if (mesh.value().has_value()) {
      model.parts.push_back(
          {.node = static_cast<int>(i), .mesh = std::move(*mesh.value())});
    }
  }
  if (model.parts.empty()) {
    return Result::Error(ImportError::kNoMeshes);
  }
  model.materials = materials.Release();

  for (const aiAnimation* animation :
       std::span(scene->mAnimations, scene->mNumAnimations)) {
    model.clips.push_back(ImportClip(path, *animation, model.hierarchy));
  }
  model.rest_bounds = RestBounds(model);

  const auto elapsed = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - start);
  core::log::Info(
      "imported {}: {} nodes, {} parts, {} clips, {} materials in {:.1f} ms",
      core::FileName(path), model.hierarchy.nodes.size(), model.parts.size(),
      model.clips.size(), model.materials.size(), elapsed.count());
  return Result::Ok(std::move(model));
}

}  // namespace blue::asset
