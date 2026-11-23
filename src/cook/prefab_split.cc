#include "cook/prefab_split.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <glm/ext/quaternion_float.hpp>
#include <glm/ext/quaternion_trigonometric.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "animation/clip.h"
#include "animation/hierarchy.h"
#include "animation/transform.h"
#include "cook/model_cook.h"
#include "core/path.h"
#include "prefab/prefab_file.h"

namespace blue::cook {
namespace {

using Status = core::Result<void, std::string>;
using PrefabResult = core::Result<prefab::Prefab, std::string>;

constexpr std::string_view kNodesOption = "--nodes";
constexpr std::string_view kExcludeOption = "--exclude";
constexpr std::string_view kOriginOption = "--origin";
constexpr std::string_view kForwardOption = "--forward";
constexpr std::string_view kKeepSkeletonFlag = "--keep-skeleton";

// Tolerance on a decomposed basis: |dot| of two unit axes above this is
// shear.
constexpr float kShearTolerance = 1.0e-3f;

// Splits "a,b,c" into its non-empty items.
std::vector<std::string> SplitList(std::string_view text) {
  std::vector<std::string> items;
  while (!text.empty()) {
    const size_t comma = text.find(',');
    const std::string_view item = text.substr(0, comma);
    if (!item.empty()) {
      items.emplace_back(item);
    }
    if (comma == std::string_view::npos) {
      break;
    }
    text.remove_prefix(comma + 1);
  }
  return items;
}

std::optional<float> ParseFloat(std::string_view text) {
  float value = 0.0f;
  const auto [end, ec] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || end != text.data() + text.size() ||
      !std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

// Parses "<x>,<y>,<z>" (exactly three numbers).
std::optional<glm::vec3> ParsePoint(std::string_view text) {
  const std::vector<std::string> items = SplitList(text);
  if (items.size() != 3 || std::count(text.begin(), text.end(), ',') != 2) {
    return std::nullopt;
  }
  glm::vec3 point(0.0f);
  for (int i = 0; i < 3; ++i) {
    const std::optional<float> value =
        ParseFloat(items[static_cast<size_t>(i)]);
    if (!value) {
      return std::nullopt;
    }
    point[i] = *value;
  }
  return point;
}

std::optional<glm::vec3> ParseAxis(std::string_view text) {
  if (text == "+z" || text == "z") {
    return glm::vec3(0.0f, 0.0f, 1.0f);
  }
  if (text == "-z") {
    return glm::vec3(0.0f, 0.0f, -1.0f);
  }
  if (text == "+x" || text == "x") {
    return glm::vec3(1.0f, 0.0f, 0.0f);
  }
  if (text == "-x") {
    return glm::vec3(-1.0f, 0.0f, 0.0f);
  }
  return std::nullopt;
}

// The turn about +Y that maps `forward` (a horizontal axis) onto +Z.
glm::quat TurnToPlusZ(glm::vec3 forward) {
  const float angle = std::atan2(-forward.x, forward.z);
  return glm::angleAxis(angle, glm::vec3(0.0f, 1.0f, 0.0f));
}

// What the part keeps of the source: the part frame (origin and turn) and
// per source node whether it is kept.
struct SplitPlan {
  glm::vec3 origin{0.0f};
  glm::quat turn{1.0f, 0.0f, 0.0f, 0.0f};
  std::vector<bool> keep;  // per source node
};

// Applies the part frame to a point / transform in source prefab space.
glm::vec3 ToPart(const SplitPlan& plan, glm::vec3 point) {
  return plan.turn * (point - plan.origin);
}
animation::Transform ToPart(const SplitPlan& plan,
                            const animation::Transform& transform) {
  return {.translation = ToPart(plan, transform.translation),
          .rotation = plan.turn * transform.rotation,
          .scale = transform.scale};
}
glm::mat4 PartMatrix(const SplitPlan& plan) {
  return glm::mat4_cast(plan.turn) *
         glm::translate(glm::mat4(1.0f), -plan.origin);
}

core::Result<SplitPlan, std::string> MakePlan(
    const prefab::Prefab& source, const SplitOptions& options,
    std::span<const glm::mat4> world) {
  using PlanResult = core::Result<SplitPlan, std::string>;
  const animation::Hierarchy& hierarchy = source.hierarchy;
  if (options.nodes.empty()) {
    return PlanResult::Error("no nodes chosen (--nodes A,B,...)");
  }
  const size_t count = hierarchy.nodes.size();
  std::vector<bool> chosen(count, false);
  std::vector<bool> excluded(count, false);
  int first = -1;
  for (const std::string& name : options.nodes) {
    const int node = animation::FindNode(hierarchy, name);
    if (node < 0) {
      return PlanResult::Error(std::format("no node '{}'", name));
    }
    chosen[static_cast<size_t>(node)] = true;
    if (first < 0) {
      first = node;
    }
  }
  for (const std::string& name : options.exclude) {
    const int node = animation::FindNode(hierarchy, name);
    if (node < 0) {
      return PlanResult::Error(std::format("--exclude: no node '{}'", name));
    }
    excluded[static_cast<size_t>(node)] = true;
  }
  SplitPlan plan;
  plan.keep.assign(count, false);
  for (size_t i = 0; i < count; ++i) {
    const int parent = hierarchy.nodes[i].parent;
    const bool under_kept =
        parent >= 0 && plan.keep[static_cast<size_t>(parent)];
    plan.keep[i] = !excluded[i] && (chosen[i] || under_kept);
  }
  if (options.origin_node) {
    const int node = animation::FindNode(hierarchy, *options.origin_node);
    if (node < 0) {
      return PlanResult::Error(
          std::format("--origin: no node '{}'", *options.origin_node));
    }
    plan.origin = glm::vec3(world[static_cast<size_t>(node)][3]);
  } else if (options.origin_point) {
    plan.origin = *options.origin_point;
  } else if (!options.keep_skeleton) {
    plan.origin = glm::vec3(world[static_cast<size_t>(first)][3]);
  }
  plan.turn = TurnToPlusZ(options.forward);
  return PlanResult::Ok(std::move(plan));
}

// Transforms the channels of `clip` that drive root nodes into the part
// frame (scale channels are unaffected: T R S with the turn applied left).
void ReexpressRootChannels(const SplitPlan& plan,
                           const animation::Hierarchy& hierarchy,
                           animation::Clip& clip) {
  for (animation::Channel& channel : clip.channels) {
    if (channel.node < 0 ||
        hierarchy.nodes[static_cast<size_t>(channel.node)].parent >= 0 ||
        channel.path == animation::TargetPath::kScale) {
      continue;
    }
    const bool cubic =
        channel.interpolation == animation::Interpolation::kCubicSpline;
    const size_t width =
        static_cast<size_t>(animation::ComponentCount(channel.path));
    const size_t elements = channel.values.size() / width;
    for (size_t e = 0; e < elements; ++e) {
      float* v = channel.values.data() + e * width;
      // Cubic layout: in-tangent, value, out-tangent; tangents only turn.
      const bool is_value = !cubic || e % 3 == 1;
      if (channel.path == animation::TargetPath::kTranslation) {
        const glm::vec3 p(v[0], v[1], v[2]);
        const glm::vec3 q =
            is_value ? ToPart(plan, p) : glm::vec3(plan.turn * p);
        v[0] = q.x;
        v[1] = q.y;
        v[2] = q.z;
      } else {
        const glm::quat r(v[3], v[0], v[1], v[2]);  // stored x y z w
        const glm::quat q = plan.turn * r;
        v[0] = q.x;
        v[1] = q.y;
        v[2] = q.z;
        v[3] = q.w;
      }
    }
  }
}

// Fills part.hierarchy and returns the source -> part node index map (-1 =
// not in the part).
core::Result<std::vector<int>, std::string> BuildHierarchy(
    const prefab::Prefab& source, const SplitOptions& options,
    const SplitPlan& plan, std::span<const glm::mat4> world,
    prefab::Prefab& part) {
  using MapResult = core::Result<std::vector<int>, std::string>;
  const std::vector<animation::Node>& nodes = source.hierarchy.nodes;
  std::vector<int> map(nodes.size(), -1);
  const glm::mat4 to_part = PartMatrix(plan);
  for (size_t i = 0; i < nodes.size(); ++i) {
    if (!options.keep_skeleton && !plan.keep[i]) {
      continue;
    }
    animation::Node node = nodes[i];
    const bool parent_in_part =
        node.parent >= 0 && map[static_cast<size_t>(node.parent)] >= 0;
    if (options.keep_skeleton) {
      if (node.parent < 0) {
        node.rest = ToPart(plan, node.rest);
      }
    } else if (parent_in_part) {
      node.parent = map[static_cast<size_t>(node.parent)];
    } else {
      std::optional<animation::Transform> rest =
          DecomposeMatrix(to_part * world[i]);
      if (!rest) {
        return MapResult::Error(std::format(
            "node '{}': its rest transform has shear or a mirror and cannot "
            "be re-rooted",
            node.name));
      }
      node.parent = -1;
      node.rest = *rest;
    }
    map[i] = static_cast<int>(part.hierarchy.nodes.size());
    part.hierarchy.nodes.push_back(std::move(node));
  }
  return MapResult::Ok(std::move(map));
}

// Copies the meshes of kept nodes with only the materials and textures they
// use, renumbered.
void CopyMeshesAndMaterials(const prefab::Prefab& source, const SplitPlan& plan,
                            std::span<const int> map, prefab::Prefab& part) {
  std::vector<int> material_map(source.materials.size(), -1);
  std::vector<int> texture_map(source.textures.size(), -1);
  for (const prefab::Mesh& mesh : source.meshes) {
    if (mesh.node < 0 || !plan.keep[static_cast<size_t>(mesh.node)]) {
      continue;
    }
    prefab::Mesh copy = mesh;
    copy.node = map[static_cast<size_t>(mesh.node)];
    for (asset::Submesh& submesh : copy.submeshes) {
      int& mapped = material_map[static_cast<size_t>(submesh.material)];
      if (mapped < 0) {
        mapped = static_cast<int>(part.materials.size());
        part.materials.push_back(
            source.materials[static_cast<size_t>(submesh.material)]);
      }
      submesh.material = mapped;
    }
    part.meshes.push_back(std::move(copy));
  }
  for (prefab::Material& material : part.materials) {
    for (int& texture : material.textures) {
      if (texture < 0) {
        continue;
      }
      int& mapped = texture_map[static_cast<size_t>(texture)];
      if (mapped < 0) {
        mapped = static_cast<int>(part.textures.size());
        part.textures.push_back(source.textures[static_cast<size_t>(texture)]);
        const auto& sources = source.edit.texture_sources;
        part.edit.texture_sources.push_back(
            static_cast<size_t>(texture) < sources.size()
                ? sources[static_cast<size_t>(texture)]
                : std::vector<std::string>{});
      }
      texture = mapped;
    }
  }
}

// Copies sockets, lights and child prefabs: on kept nodes (renumbered), and
// with the skeleton kept also those on the root, re-expressed.
void CopyAttachments(const prefab::Prefab& source, const SplitOptions& options,
                     const SplitPlan& plan, std::span<const int> map,
                     prefab::Prefab& part) {
  // Returns the part node of `node`, or nullopt if the item is dropped.
  const auto place = [&](int node) -> std::optional<int> {
    if (node < 0) {
      return options.keep_skeleton ? std::optional<int>(-1) : std::nullopt;
    }
    const int mapped = map[static_cast<size_t>(node)];
    return mapped >= 0 ? std::optional<int>(mapped) : std::nullopt;
  };
  for (prefab::Socket socket : source.sockets) {
    if (const std::optional<int> node = place(socket.node)) {
      if (*node < 0) {
        socket.local = ToPart(plan, socket.local);
      }
      socket.node = *node;
      part.sockets.push_back(std::move(socket));
    }
  }
  for (prefab::PointLight light : source.lights) {
    if (const std::optional<int> node = place(light.node)) {
      if (*node < 0) {
        light.position = ToPart(plan, light.position);
      }
      light.node = *node;
      part.lights.push_back(std::move(light));
    }
  }
  for (prefab::ChildPrefab child : source.children) {
    if (const std::optional<int> node = place(child.node)) {
      if (*node < 0) {
        child.local = ToPart(plan, child.local);
      }
      child.node = *node;
      part.children.push_back(std::move(child));
    }
  }
}

}  // namespace

bool IsSplitOption(std::string_view option) {
  return option == kNodesOption || option == kExcludeOption ||
         option == kOriginOption || option == kForwardOption;
}

bool ApplySplitFlag(SplitOptions& options, std::string_view option) {
  if (option != kKeepSkeletonFlag) {
    return false;
  }
  options.keep_skeleton = true;
  return true;
}

core::Result<void, std::string> ParseSplitOption(SplitOptions& options,
                                                 std::string_view option,
                                                 std::string_view value) {
  const auto fail = [&](std::string_view syntax) {
    return Status::Error(
        std::format("{}: expected {}, got '{}'", option, syntax, value));
  };
  if (option == kNodesOption || option == kExcludeOption) {
    std::vector<std::string> names = SplitList(value);
    if (names.empty()) {
      return fail("<node>,<node>,...");
    }
    std::vector<std::string>& target =
        option == kNodesOption ? options.nodes : options.exclude;
    target.insert(target.end(), names.begin(), names.end());
    return Status::Ok();
  }
  if (option == kOriginOption) {
    if (value.empty()) {
      return fail("<node> or <x>,<y>,<z>");
    }
    options.origin_node.reset();
    options.origin_point.reset();
    if (value.find(',') != std::string_view::npos) {
      options.origin_point = ParsePoint(value);
      if (!options.origin_point) {
        return fail("<node> or <x>,<y>,<z>");
      }
    } else {
      options.origin_node = std::string(value);
    }
    return Status::Ok();
  }
  if (option == kForwardOption) {
    const std::optional<glm::vec3> axis = ParseAxis(value);
    if (!axis) {
      return fail("+x, -x, +z or -z");
    }
    options.forward = *axis;
    return Status::Ok();
  }
  return Status::Error(std::format("unknown split option '{}'", option));
}

std::vector<glm::mat4> RestWorldMatrices(const prefab::Prefab& prefab) {
  const size_t count = prefab.hierarchy.nodes.size();
  std::vector<animation::Transform> pose(count);
  animation::ResetToRest(prefab.hierarchy, pose);
  std::vector<glm::mat4> world(count, glm::mat4(1.0f));
  animation::ComputeWorldMatrices(prefab.hierarchy, pose, world);
  return world;
}

asset::Bounds MeshBoundsInPrefabSpace(const prefab::Prefab& prefab, int mesh) {
  const prefab::Mesh& m = prefab.meshes[static_cast<size_t>(mesh)];
  glm::mat4 to_prefab(1.0f);
  if (m.node >= 0) {
    to_prefab = RestWorldMatrices(prefab)[static_cast<size_t>(m.node)];
  }
  asset::Bounds bounds;
  for (int corner = 0; corner < 8; ++corner) {
    const glm::vec3 local((corner & 1) != 0 ? m.bounds.max.x : m.bounds.min.x,
                          (corner & 2) != 0 ? m.bounds.max.y : m.bounds.min.y,
                          (corner & 4) != 0 ? m.bounds.max.z : m.bounds.min.z);
    const glm::vec3 p(to_prefab * glm::vec4(local, 1.0f));
    bounds.min = corner == 0 ? p : glm::min(bounds.min, p);
    bounds.max = corner == 0 ? p : glm::max(bounds.max, p);
  }
  return bounds;
}

std::optional<animation::Transform> DecomposeMatrix(const glm::mat4& matrix) {
  glm::vec3 axes[3];
  glm::vec3 scale(0.0f);
  for (int i = 0; i < 3; ++i) {
    const glm::vec3 column(matrix[i]);
    scale[i] = glm::length(column);
    if (!(scale[i] > 0.0f) || !std::isfinite(scale[i])) {
      return std::nullopt;
    }
    axes[i] = column / scale[i];
  }
  if (std::abs(glm::dot(axes[0], axes[1])) > kShearTolerance ||
      std::abs(glm::dot(axes[0], axes[2])) > kShearTolerance ||
      std::abs(glm::dot(axes[1], axes[2])) > kShearTolerance ||
      glm::dot(glm::cross(axes[0], axes[1]), axes[2]) < 0.0f) {
    return std::nullopt;
  }
  return animation::Transform{
      .translation = glm::vec3(matrix[3]),
      .rotation =
          glm::normalize(glm::quat_cast(glm::mat3(axes[0], axes[1], axes[2]))),
      .scale = scale};
}

core::Result<prefab::Socket, std::string> SocketAtPrefabFrame(
    const prefab::Prefab& prefab, std::string name, int node,
    const animation::Transform& frame) {
  using SocketResult = core::Result<prefab::Socket, std::string>;
  if (node >= static_cast<int>(prefab.hierarchy.nodes.size())) {
    return SocketResult::Error(
        std::format("socket '{}': no node {}", name, node));
  }
  glm::mat4 node_world(1.0f);
  if (node >= 0) {
    node_world = RestWorldMatrices(prefab)[static_cast<size_t>(node)];
  }
  const std::optional<animation::Transform> local =
      DecomposeMatrix(glm::inverse(node_world) * animation::ToMatrix(frame));
  if (!local) {
    return SocketResult::Error(std::format(
        "socket '{}': node {} has shear; cannot place a socket on it", name,
        node));
  }
  return SocketResult::Ok(
      prefab::Socket{.name = std::move(name), .node = node, .local = *local});
}

core::Result<prefab::Prefab, std::string> SplitPrefab(
    const prefab::Prefab& source, const SplitOptions& options) {
  if (options.origin_node && options.origin_point) {
    return PrefabResult::Error("give --origin as a node or a point, not both");
  }
  const std::vector<glm::mat4> world = RestWorldMatrices(source);
  core::Result<SplitPlan, std::string> planned =
      MakePlan(source, options, world);
  if (!planned.ok()) {
    return PrefabResult::Error(planned.error());
  }
  const SplitPlan& plan = planned.value();

  prefab::Prefab part;
  part.storage = source.storage;  // shared; unviewed blocks dropped below
  core::Result<std::vector<int>, std::string> map =
      BuildHierarchy(source, options, plan, world, part);
  if (!map.ok()) {
    return PrefabResult::Error(map.error());
  }
  CopyMeshesAndMaterials(source, plan, map.value(), part);
  if (part.meshes.empty()) {
    return PrefabResult::Error("the chosen nodes have no meshes");
  }
  CopyAttachments(source, options, plan, map.value(), part);
  if (options.keep_skeleton) {
    part.clips = source.clips;
    for (animation::Clip& clip : part.clips) {
      ReexpressRootChannels(plan, source.hierarchy, clip);
    }
  }
  part.bounds = ComputePrefabBounds(part);
  prefab::CompactStorage(part);
  return PrefabResult::Ok(std::move(part));
}

core::Result<void, std::string> SplitPrefabFile(
    const std::string& input, const std::string& output,
    const SplitOptions& options, std::span<const PrefabEdit> edits) {
  if (core::NormalizePath(input) == core::NormalizePath(output)) {
    return Status::Error(
        std::format("the output must not be the input ({})", input));
  }
  core::Result<prefab::Prefab, prefab::PrefabError> loaded =
      prefab::LoadPrefab(input);
  if (!loaded.ok()) {
    return Status::Error(std::format("cannot load {}: {}", input,
                                     prefab::ToString(loaded.error())));
  }
  PrefabResult part = SplitPrefab(loaded.value(), options);
  if (!part.ok()) {
    return Status::Error(part.error());
  }
  PrefabResult edited = ApplyPrefabEdits(part.value(), edits);
  if (!edited.ok()) {
    return Status::Error(edited.error());
  }
  if (std::optional<std::string> problem =
          prefab::ValidatePrefab(edited.value())) {
    return Status::Error(std::format("the part is invalid: {}", *problem));
  }
  if (core::Result<void, prefab::PrefabError> saved =
          prefab::SavePrefab(edited.value(), output);
      !saved.ok()) {
    return Status::Error(std::format("cannot write {}: {}", output,
                                     prefab::ToString(saved.error())));
  }
  return Status::Ok();
}

}  // namespace blue::cook
