#include "world/world.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "animation/transform.h"
#include "core/log.h"
#include "core/path.h"
#include "material/material.h"
#include "prefab/prefab.h"
#include "render/scene_view.h"
#include "scene/scene_data.h"

namespace blue::world {
namespace {

double MillisecondsSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - start)
      .count();
}

// Appends the bit pattern of `value` to `key`, so equal keys mean
// bit-identical parameters.
void AppendBits(std::string& key, float value) {
  key += std::format("{:08x},", std::bit_cast<uint32_t>(value));
}

// Returns a key that identifies the parameter tweaks of `edit`.
std::string TweakKey(const scene::MaterialOverride& edit) {
  std::string key;
  if (edit.base_color) {
    key += "c";
    for (int i = 0; i < 4; ++i) {
      AppendBits(key, (*edit.base_color)[i]);
    }
  }
  if (edit.metallic) {
    key += "m";
    AppendBits(key, *edit.metallic);
  }
  if (edit.roughness) {
    key += "r";
    AppendBits(key, *edit.roughness);
  }
  if (edit.emissive) {
    key += "e";
    for (int i = 0; i < 3; ++i) {
      AppendBits(key, (*edit.emissive)[i]);
    }
  }
  if (edit.emissive_strength) {
    key += "s";
    AppendBits(key, *edit.emissive_strength);
  }
  if (edit.uv_scale) {
    key += "u";
    AppendBits(key, edit.uv_scale->x);
    AppendBits(key, edit.uv_scale->y);
  }
  return key;
}

// Applies the parameter tweaks of `edit` to `desc`.
void ApplyTweaks(const scene::MaterialOverride& edit,
                 material::MaterialDesc& desc) {
  if (edit.base_color) {
    desc.base_color = *edit.base_color;
  }
  if (edit.metallic) {
    desc.metallic = *edit.metallic;
  }
  if (edit.roughness) {
    desc.roughness = *edit.roughness;
  }
  if (edit.emissive) {
    desc.emissive = *edit.emissive;
  }
  if (edit.emissive_strength) {
    desc.emissive_strength = *edit.emissive_strength;
  }
  if (edit.uv_scale) {
    desc.uv_scale = *edit.uv_scale;
  }
}

render::PointLight ToRenderLight(const scene::ScenePointLight& light) {
  return {.position = light.position,
          .color = light.color,
          .intensity = light.intensity,
          .range = light.range};
}

// Returns the world box of `bounds` placed by `transform`.
asset::Bounds TransformBounds(const asset::Bounds& bounds,
                              const glm::mat4& transform) {
  asset::Bounds result{.min = glm::vec3(std::numeric_limits<float>::max()),
                       .max = glm::vec3(-std::numeric_limits<float>::max())};
  for (int corner = 0; corner < 8; ++corner) {
    const glm::vec3 local((corner & 1) != 0 ? bounds.max.x : bounds.min.x,
                          (corner & 2) != 0 ? bounds.max.y : bounds.min.y,
                          (corner & 4) != 0 ? bounds.max.z : bounds.min.z);
    const glm::vec3 world = glm::vec3(transform * glm::vec4(local, 1.0f));
    result.min = glm::min(result.min, world);
    result.max = glm::max(result.max, world);
  }
  return result;
}

// Returns the entry distance of the ray (origin, direction) into `bounds`
// (0 when it starts inside), or nullopt if it misses within `max_t`.
std::optional<float> RayBox(glm::vec3 origin, glm::vec3 direction,
                            const asset::Bounds& bounds, float max_t) {
  float near_t = 0.0f;
  float far_t = max_t;
  for (int axis = 0; axis < 3; ++axis) {
    const float d = direction[axis];
    const float o = origin[axis];
    if (std::abs(d) < 1.0e-12f) {
      if (o < bounds.min[axis] || o > bounds.max[axis]) {
        return std::nullopt;
      }
      continue;
    }
    float t0 = (bounds.min[axis] - o) / d;
    float t1 = (bounds.max[axis] - o) / d;
    if (t0 > t1) {
      std::swap(t0, t1);
    }
    near_t = std::max(near_t, t0);
    far_t = std::min(far_t, t1);
    if (near_t > far_t) {
      return std::nullopt;
    }
  }
  return near_t;
}

// Returns the ray parameter of the hit with triangle (a, b, c), either side,
// or nullopt (Moller-Trumbore).
std::optional<float> RayTriangle(glm::vec3 origin, glm::vec3 direction,
                                 glm::vec3 a, glm::vec3 b, glm::vec3 c) {
  const glm::vec3 edge1 = b - a;
  const glm::vec3 edge2 = c - a;
  const glm::vec3 p = glm::cross(direction, edge2);
  const float det = glm::dot(edge1, p);
  if (std::abs(det) < 1.0e-12f) {
    return std::nullopt;
  }
  const float inv_det = 1.0f / det;
  const glm::vec3 s = origin - a;
  const float u = glm::dot(s, p) * inv_det;
  if (u < 0.0f || u > 1.0f) {
    return std::nullopt;
  }
  const glm::vec3 q = glm::cross(s, edge1);
  const float v = glm::dot(direction, q) * inv_det;
  if (v < 0.0f || u + v > 1.0f) {
    return std::nullopt;
  }
  const float t = glm::dot(edge2, q) * inv_det;
  if (t < 0.0f) {
    return std::nullopt;
  }
  return t;
}

}  // namespace

// The state of one Build() call.
struct World::Builder {
  World& world;
  const scene::SceneData& scene;
  std::vector<WorldIssue> issues;
  // Override submesh ranges to turn into spans once override_draws_ stops
  // growing: (placement, mesh, offset, count).
  struct PendingSpan {
    size_t placement;
    size_t mesh;
    size_t offset;
    size_t count;
  };
  std::vector<PendingSpan> pending;
  // scene.prefabs resolved once per build (nullopt = not tried yet).
  std::vector<std::optional<std::optional<PrefabId>>> resolved;

  void Issue(scene::InstanceId id, std::string message) {
    issues.push_back({id, std::move(message)});
  }

  // Returns the cached prefab at scene.prefabs[index], or nullopt (issue).
  std::optional<PrefabId> ScenePrefab(scene::InstanceId owner, int index) {
    if (index < 0 || static_cast<size_t>(index) >= scene.prefabs.size()) {
      Issue(owner, std::format("prefab index {} out of range", index));
      return std::nullopt;
    }
    if (resolved.empty()) {
      resolved.resize(scene.prefabs.size());
    }
    std::optional<std::optional<PrefabId>>& slot =
        resolved[static_cast<size_t>(index)];
    if (slot.has_value()) {
      return *slot;
    }
    const std::string& path = scene.prefabs[static_cast<size_t>(index)];
    auto id = world.cache_.Get(path);
    if (!id.ok()) {
      Issue(owner, std::format("cannot load prefab {}: {}", path,
                               prefab::ToString(id.error())));
      slot = std::optional<PrefabId>();
      return std::nullopt;
    }
    slot = id.value();
    return id.value();
  }

  // Adds a placement of `prefab` and, depth first, its nested prefabs.
  // `chain` holds the prefabs from the instance's own down to the parent,
  // for cycle detection.
  void AddPlacement(scene::InstanceId owner, PrefabId prefab, int parent,
                    int parent_node, const glm::mat4& local, int depth,
                    std::vector<PrefabId>& chain) {
    const auto index = static_cast<int>(world.placements_.size());
    world.placements_.push_back(
        {.owner = owner, .prefab = prefab, .parent = parent, .depth = depth});
    world.states_.push_back({.pose = PrefabPose(world.cache_, prefab),
                             .local = local,
                             .parent_node = parent_node});
    chain.push_back(prefab);
    // Copy: the cache's vectors stay put, but be robust to Get() appending.
    const std::vector<prefab::ChildPrefab> children =
        world.cache_.prefab(prefab).children;
    for (const prefab::ChildPrefab& child : children) {
      auto child_id = world.cache_.Get(child.path);
      if (!child_id.ok()) {
        Issue(owner, std::format("cannot load nested prefab {}: {}", child.path,
                                 prefab::ToString(child_id.error())));
        continue;
      }
      if (std::ranges::find(chain, child_id.value()) != chain.end()) {
        std::string cycle;
        for (const PrefabId link : chain) {
          cycle += world.cache_.key(link) + " -> ";
        }
        Issue(owner, "nested prefab cycle: " + cycle + child.path);
        continue;
      }
      if (depth + 1 > kMaxPrefabDepth) {
        Issue(owner, std::format("nested prefab {} exceeds the depth limit "
                                 "of {}",
                                 child.path, kMaxPrefabDepth));
        continue;
      }
      AddPlacement(owner, child_id.value(), index, child.node,
                   animation::ToMatrix(child.local), depth + 1, chain);
    }
    chain.pop_back();
  }

  // Returns the material that replaces material `material` of `prefab` for
  // `instance`, or nullopt to keep the prefab's own.
  std::optional<render::MaterialId> OverrideFor(const scene::Instance& instance,
                                                PrefabId prefab, int material) {
    const prefab::Material& base =
        world.cache_.prefab(prefab).materials[static_cast<size_t>(material)];
    const scene::MaterialOverride* edit =
        scene::FindMaterialOverride(instance, base.name);
    if (edit == nullptr) {
      return std::nullopt;
    }
    PrefabId source = prefab;
    int source_material = material;
    if (edit->source_prefab != scene::kNoPrefab) {
      const std::optional<PrefabId> library =
          ScenePrefab(instance.id, edit->source_prefab);
      const int found =
          library ? prefab::FindMaterial(world.cache_.prefab(*library),
                                         edit->source_material)
                  : -1;
      if (found < 0) {
        Issue(instance.id,
              std::format("override of '{}': material '{}' not found in its "
                          "source prefab",
                          base.name, edit->source_material));
      } else {
        source = *library;
        source_material = found;
      }
    }
    const std::string key = TweakKey(*edit);
    if (source == prefab && source_material == material && key.empty()) {
      return std::nullopt;
    }
    return world.cache_.DerivedMaterial(
        source, source_material, key,
        [edit](material::MaterialDesc& desc) { ApplyTweaks(*edit, desc); });
  }

  // Resolves the material overrides of `instance` for the placements in
  // [first, first + count).
  void ApplyOverrides(const scene::Instance& instance, size_t first,
                      size_t count) {
    if (instance.materials.empty()) {
      return;
    }
    std::vector<bool> used(instance.materials.size(), false);
    for (size_t p = first; p < first + count; ++p) {
      const PrefabId prefab = world.placements_[p].prefab;
      const prefab::Prefab& data = world.cache_.prefab(prefab);
      for (const prefab::Material& material : data.materials) {
        for (size_t o = 0; o < instance.materials.size(); ++o) {
          used[o] =
              used[o] || instance.materials[o].material_name == material.name;
        }
      }
      const PrefabGpu& gpu = world.cache_.gpu(prefab);
      for (size_t m = 0; m < data.meshes.size(); ++m) {
        const size_t offset = world.override_draws_.size();
        bool overridden = false;
        for (const asset::Submesh& submesh : data.meshes[m].submeshes) {
          std::optional<render::MaterialId> replacement =
              OverrideFor(instance, prefab, submesh.material);
          overridden = overridden || replacement.has_value();
          world.override_draws_.push_back(
              {.first_index = submesh.first_index,
               .index_count = submesh.index_count,
               .material = replacement.value_or(
                   gpu.materials[static_cast<size_t>(submesh.material)])});
        }
        if (overridden) {
          pending.push_back(
              {p, m, offset, world.override_draws_.size() - offset});
        } else {
          world.override_draws_.resize(offset);
        }
      }
    }
    for (size_t o = 0; o < used.size(); ++o) {
      if (!used[o]) {
        Issue(instance.id,
              std::format("material override '{}': no such material in the "
                          "prefab",
                          instance.materials[o].material_name));
      }
    }
  }
};

std::vector<WorldIssue> World::Build(const scene::SceneData& scene) {
  const auto start = std::chrono::steady_clock::now();
  placements_.clear();
  states_.clear();
  instances_.clear();
  entry_index_.clear();
  override_draws_.clear();
  lights_.clear();
  light_offsets_.clear();
  animated_.clear();

  Builder builder{.world = *this, .scene = scene};

  // Order the visible instances parents first; instances whose parent chain
  // is broken or hidden are skipped.
  std::unordered_map<scene::InstanceId, size_t> by_id;
  for (size_t i = 0; i < scene.instances.size(); ++i) {
    by_id.emplace(scene.instances[i].id, i);
  }
  // 0 = unvisited, 1 = in progress, 2 = placed, 3 = skipped.
  std::vector<uint8_t> state(scene.instances.size(), 0);
  std::vector<size_t> order;
  order.reserve(scene.instances.size());
  for (size_t root = 0; root < scene.instances.size(); ++root) {
    // Walk up to the first resolved ancestor, then place downwards.
    std::vector<size_t> path;
    size_t current = root;
    bool broken = false;
    while (state[current] == 0) {
      state[current] = 1;
      path.push_back(current);
      const scene::InstanceId parent = scene.instances[current].parent;
      if (parent == scene::kNoInstance) {
        break;
      }
      auto found = by_id.find(parent);
      if (found == by_id.end() || state[found->second] == 1) {
        broken = true;
        break;
      }
      current = found->second;
    }
    // The top of the path decides for the whole path.
    const size_t top = path.empty() ? current : path.back();
    const scene::InstanceId top_parent = scene.instances[top].parent;
    bool skip =
        broken || (top_parent != scene::kNoInstance &&
                   by_id.contains(top_parent) && state[by_id[top_parent]] == 3);
    if (broken && !path.empty()) {
      builder.Issue(scene.instances[path.back()].id,
                    "broken parent chain (missing parent or a cycle)");
    }
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
      skip = skip || !scene.instances[*it].visible;
      state[*it] = skip ? 3 : 2;
      if (!skip) {
        order.push_back(*it);
      }
    }
  }

  for (const size_t index : order) {
    const scene::Instance& instance = scene.instances[index];
    InstanceEntry entry{.id = instance.id, .parent = instance.parent};
    glm::mat4 parent_world(1.0f);
    if (const InstanceEntry* parent = FindEntry(instance.parent)) {
      parent_world = parent->world;
    }
    entry.world = parent_world * instance.transform.Matrix();
    entry.first_placement = -1;
    if (instance.prefab != scene::kNoPrefab) {
      if (std::optional<PrefabId> prefab =
              builder.ScenePrefab(instance.id, instance.prefab)) {
        entry.first_placement = static_cast<int>(placements_.size());
        std::vector<PrefabId> chain;
        builder.AddPlacement(instance.id, *prefab, -1, -1, glm::mat4(1.0f), 0,
                             chain);
        entry.placement_count =
            static_cast<int>(placements_.size()) - entry.first_placement;
        builder.ApplyOverrides(instance,
                               static_cast<size_t>(entry.first_placement),
                               static_cast<size_t>(entry.placement_count));
      }
    }
    entry_index_.emplace(entry.id, instances_.size());
    instances_.push_back(entry);
  }

  // override_draws_ is complete: hand out the views.
  for (PlacementState& placement : states_) {
    placement.submeshes.clear();
  }
  for (const Builder::PendingSpan& span : builder.pending) {
    PlacementState& placement = states_[span.placement];
    const size_t mesh_count =
        cache_.prefab(placements_[span.placement].prefab).meshes.size();
    placement.submeshes.resize(mesh_count);
    placement.submeshes[span.mesh] = std::span<const render::SubmeshDraw>(
        override_draws_.data() + span.offset, span.count);
  }

  for (const scene::ScenePointLight& light : scene.point_lights) {
    lights_.push_back(ToRenderLight(light));
  }
  scene_light_count_ = lights_.size();
  light_offsets_.reserve(placements_.size());
  for (const Placement& placement : placements_) {
    light_offsets_.push_back(static_cast<uint32_t>(lights_.size()));
    lights_.resize(lights_.size() +
                   cache_.prefab(placement.prefab).lights.size());
  }
  for (const InstanceEntry& entry : instances_) {
    if (entry.first_placement >= 0) {
      placements_[static_cast<size_t>(entry.first_placement)].world =
          entry.world;
    }
  }
  UpdatePlacementMatrices(0, placements_.size());

  core::log::Info(
      "World built: {} instances, {} placements, {} lights, {} "
      "issues in {:.2f} ms",
      instances_.size(), placements_.size(), lights_.size(),
      builder.issues.size(), MillisecondsSince(start));
  return std::move(builder.issues);
}

void World::UpdatePlacementMatrices(size_t first, size_t count) {
  for (size_t i = first; i < first + count; ++i) {
    Placement& placement = placements_[i];
    if (placement.parent >= 0) {
      const auto parent = static_cast<size_t>(placement.parent);
      placement.world =
          placements_[parent].world *
          states_[parent].pose.NodeMatrix(states_[i].parent_node) *
          states_[i].local;
    }
  }
  UpdateLights(first, count);
}

void World::UpdateLights(size_t first, size_t count) {
  for (size_t i = first; i < first + count; ++i) {
    const std::vector<prefab::PointLight>& lights =
        cache_.prefab(placements_[i].prefab).lights;
    for (size_t l = 0; l < lights.size(); ++l) {
      const prefab::PointLight& light = lights[l];
      const glm::mat4 transform =
          placements_[i].world * states_[i].pose.NodeMatrix(light.node);
      lights_[light_offsets_[i] + l] = {
          .position = glm::vec3(transform * glm::vec4(light.position, 1.0f)),
          .color = light.color,
          .intensity = light.intensity,
          .range = light.range};
    }
  }
}

const World::InstanceEntry* World::FindEntry(scene::InstanceId id) const {
  auto found = entry_index_.find(id);
  return found == entry_index_.end() ? nullptr : &instances_[found->second];
}

// Returns the number of placements nested below placement `index`
// (contiguous after it: placements are stored depth first).
size_t World::DescendantCount(size_t index) const {
  size_t end = index + 1;
  while (end < placements_.size() &&
         placements_[end].depth > placements_[index].depth &&
         placements_[end].owner == placements_[index].owner) {
    ++end;
  }
  return end - index - 1;
}

bool World::SetInstanceTransform(const scene::SceneData& scene,
                                 scene::InstanceId id) {
  auto found = entry_index_.find(id);
  if (found == entry_index_.end()) {
    return false;
  }
  // Entries are ordered parents first, so one pass from `id` covers its
  // whole subtree.
  std::vector<scene::InstanceId> moved{id};
  for (size_t i = found->second; i < instances_.size(); ++i) {
    InstanceEntry& entry = instances_[i];
    const bool in_subtree =
        entry.id == id || std::ranges::find(moved, entry.parent) != moved.end();
    if (!in_subtree) {
      continue;
    }
    if (entry.id != id) {
      moved.push_back(entry.id);
    }
    const scene::Instance* instance = scene::FindInstance(scene, entry.id);
    if (instance == nullptr) {
      continue;
    }
    const InstanceEntry* parent = FindEntry(entry.parent);
    entry.world = (parent != nullptr ? parent->world : glm::mat4(1.0f)) *
                  instance->transform.Matrix();
    if (entry.first_placement >= 0) {
      const auto first = static_cast<size_t>(entry.first_placement);
      placements_[first].world = entry.world;
      UpdatePlacementMatrices(first,
                              static_cast<size_t>(entry.placement_count));
    }
  }
  return true;
}

bool World::SetInstanceMatrix(scene::InstanceId id, const glm::mat4& world) {
  auto found = entry_index_.find(id);
  if (found == entry_index_.end()) {
    return false;
  }
  InstanceEntry& entry = instances_[found->second];
  entry.world = world;
  if (entry.first_placement >= 0) {
    const auto first = static_cast<size_t>(entry.first_placement);
    placements_[first].world = world;
    UpdatePlacementMatrices(first, static_cast<size_t>(entry.placement_count));
  }
  return true;
}

bool World::PlayClip(scene::InstanceId id, std::string_view clip_name,
                     float speed, std::optional<bool> loop) {
  const InstanceEntry* entry = FindEntry(id);
  if (entry == nullptr || entry->first_placement < 0) {
    return false;
  }
  const auto index = static_cast<uint32_t>(entry->first_placement);
  if (!states_[index].pose.Play(clip_name, 0.0f, speed, loop)) {
    return false;
  }
  if (std::ranges::find(animated_, index) == animated_.end()) {
    animated_.push_back(index);
  }
  return true;
}

PrefabPose* World::pose(scene::InstanceId id) {
  const InstanceEntry* entry = FindEntry(id);
  if (entry == nullptr || entry->first_placement < 0) {
    return nullptr;
  }
  const auto index = static_cast<uint32_t>(entry->first_placement);
  // Scrubbing changes the pose without Update(): keep it refreshed.
  if (std::ranges::find(animated_, index) == animated_.end()) {
    animated_.push_back(index);
  }
  return &states_[index].pose;
}

void World::Update(float dt) {
  for (const uint32_t index : animated_) {
    states_[index].pose.Update(dt);
    // Nested prefabs and lights follow the (possibly scrubbed) pose.
    UpdatePlacementMatrices(index, DescendantCount(index) + 1);
  }
}

void World::AppendDraws(std::vector<render::DrawItem>& out) const {
  for (size_t i = 0; i < placements_.size(); ++i) {
    states_[i].pose.AppendDraws(placements_[i].world, out,
                                states_[i].submeshes);
  }
}

void World::AppendSelectedDraws(std::span<const scene::InstanceId> selection,
                                uint32_t first_draw,
                                std::vector<uint32_t>& out) const {
  if (selection.empty()) {
    return;
  }
  // Selected instances and their descendants (entries are parents first).
  std::vector<scene::InstanceId> selected(selection.begin(), selection.end());
  for (const InstanceEntry& entry : instances_) {
    if (std::ranges::find(selected, entry.parent) != selected.end() &&
        std::ranges::find(selected, entry.id) == selected.end()) {
      selected.push_back(entry.id);
    }
  }
  uint32_t draw = first_draw;
  for (const Placement& placement : placements_) {
    const auto mesh_count =
        static_cast<uint32_t>(cache_.prefab(placement.prefab).meshes.size());
    if (std::ranges::find(selected, placement.owner) != selected.end()) {
      for (uint32_t m = 0; m < mesh_count; ++m) {
        out.push_back(draw + m);
      }
    }
    draw += mesh_count;
  }
}

std::optional<glm::mat4> World::SocketMatrix(scene::InstanceId id,
                                             std::string_view name) const {
  const InstanceEntry* entry = FindEntry(id);
  if (entry == nullptr || entry->first_placement < 0) {
    return std::nullopt;
  }
  for (int i = 0; i < entry->placement_count; ++i) {
    const auto p = static_cast<size_t>(entry->first_placement + i);
    if (std::optional<glm::mat4> socket = states_[p].pose.SocketMatrix(name)) {
      return placements_[p].world * *socket;
    }
  }
  return std::nullopt;
}

std::optional<asset::Bounds> World::InstanceBounds(scene::InstanceId id) const {
  const InstanceEntry* entry = FindEntry(id);
  if (entry == nullptr || entry->first_placement < 0) {
    return std::nullopt;
  }
  std::optional<asset::Bounds> result;
  for (int i = 0; i < entry->placement_count; ++i) {
    const auto p = static_cast<size_t>(entry->first_placement + i);
    for (const prefab::Mesh& mesh :
         cache_.prefab(placements_[p].prefab).meshes) {
      const asset::Bounds box = TransformBounds(
          mesh.bounds,
          placements_[p].world * states_[p].pose.NodeMatrix(mesh.node));
      if (!result) {
        result = box;
      } else {
        result->min = glm::min(result->min, box.min);
        result->max = glm::max(result->max, box.max);
      }
    }
  }
  return result;
}

std::optional<PickHit> World::Pick(glm::vec3 origin, glm::vec3 direction,
                                   float max_distance, PickMode mode) const {
  const float length = glm::length(direction);
  if (!(length > 1.0e-9f) || !(max_distance > 0.0f)) {
    return std::nullopt;
  }
  const glm::vec3 unit = direction / length;
  std::optional<PickHit> best;
  for (size_t p = 0; p < placements_.size(); ++p) {
    const prefab::Prefab& prefab = cache_.prefab(placements_[p].prefab);
    for (const prefab::Mesh& mesh : prefab.meshes) {
      const glm::mat4 transform =
          placements_[p].world * states_[p].pose.NodeMatrix(mesh.node);
      const glm::mat4 inverse = glm::inverse(transform);
      // The ray in mesh space, parameterised by world distance.
      const glm::vec3 local_origin =
          glm::vec3(inverse * glm::vec4(origin, 1.0f));
      const glm::vec3 local_dir = glm::vec3(inverse * glm::vec4(unit, 0.0f));
      const float limit = best ? best->distance : max_distance;
      std::optional<float> hit =
          RayBox(local_origin, local_dir, mesh.bounds, limit);
      if (!hit) {
        continue;
      }
      if (mode == PickMode::kTriangles) {
        std::optional<float> closest;
        for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
          const std::optional<float> tri = RayTriangle(
              local_origin, local_dir, mesh.vertices[mesh.indices[t]].position,
              mesh.vertices[mesh.indices[t + 1]].position,
              mesh.vertices[mesh.indices[t + 2]].position);
          if (tri && *tri <= limit && (!closest || *tri < *closest)) {
            closest = tri;
          }
        }
        hit = closest;
      }
      if (hit && (!best || *hit < best->distance)) {
        best = PickHit{.instance = placements_[p].owner,
                       .distance = *hit,
                       .point = origin + unit * *hit};
      }
    }
  }
  return best;
}

std::vector<std::vector<std::string>> World::MaterialNames(
    const scene::SceneData& scene) {
  std::vector<std::vector<std::string>> names(scene.prefabs.size());
  for (size_t i = 0; i < scene.prefabs.size(); ++i) {
    auto root = cache_.Get(scene.prefabs[i]);
    if (!root.ok()) {
      continue;
    }
    // Depth-first over nested prefabs; `chain` stops cycles.
    std::vector<std::pair<PrefabId, int>> stack{{root.value(), 0}};
    std::vector<PrefabId> visited;
    while (!stack.empty()) {
      const auto [id, depth] = stack.back();
      stack.pop_back();
      if (std::ranges::find(visited, id) != visited.end() ||
          depth > kMaxPrefabDepth) {
        continue;
      }
      visited.push_back(id);
      for (const prefab::Material& material : cache_.prefab(id).materials) {
        if (std::ranges::find(names[i], material.name) == names[i].end()) {
          names[i].push_back(material.name);
        }
      }
      for (const prefab::ChildPrefab& child : cache_.prefab(id).children) {
        if (auto child_id = cache_.Get(child.path); child_id.ok()) {
          stack.emplace_back(child_id.value(), depth + 1);
        }
      }
    }
  }
  return names;
}

}  // namespace blue::world
