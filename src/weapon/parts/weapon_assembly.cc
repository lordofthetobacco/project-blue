#include "weapon/parts/weapon_assembly.h"

#include <cstddef>
#include <format>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "animation/transform.h"
#include "asset/mesh_data.h"
#include "core/result.h"
#include "prefab/prefab.h"
#include "weapon/parts/part_catalog.h"
#include "weapon/parts/weapon_build.h"
#include "weapon/parts/weapon_stats.h"
#include "weapon/parts/weapon_tuning.h"
#include "weapon/weapon_def.h"
#include "weapon/weapon_geometry.h"
#include "weapon/weapon_prefab.h"

namespace blue::weapon::parts {
namespace {

using AssemblyResult = core::Result<WeaponDef, std::string>;
using VoidResult = core::Result<void, std::string>;

// The socket a part offers slot `slot_name` with.
constexpr std::string_view kSlotSocketPrefix = "slot.";
// The socket of a part that sits on its parent's slot socket.
constexpr std::string_view kMountSocket = "mount";

// A part placed so far, with what the later steps need of its model.
struct Placed {
  PartPlacement placement;
  PartCategory category = PartCategory::kTactical;
  const prefab::Prefab* prefab = nullptr;  // null: no model
  std::vector<glm::mat4> rest;             // the prefab's rest node matrices
};

// Returns the model of `part`: nullopt for a part without one.
core::Result<std::optional<PartModel>, std::string> LoadModel(
    const PartDef& part, const ModelSource& models) {
  using ModelResult = core::Result<std::optional<PartModel>, std::string>;
  if (part.prefab.empty()) {
    return ModelResult::Ok(std::nullopt);
  }
  core::Result<PartModel, std::string> loaded = models(part.prefab);
  if (!loaded.ok()) {
    return ModelResult::Error(std::format(
        "part '{}': cannot load {}: {}", part.id, part.prefab, loaded.error()));
  }
  if (loaded.value().prefab == nullptr) {
    return ModelResult::Error(
        std::format("part '{}': no model for {}", part.id, part.prefab));
  }
  return ModelResult::Ok(loaded.value());
}

// Places the parts installed in the slots of `build`, whose own part is
// out[parent], and then theirs.
VoidResult PlaceChildren(const PartCatalog& catalog, const WeaponBuild& build,
                         int parent, const ModelSource& models,
                         std::vector<Placed>& out) {
  const PartDef& owner = catalog.part(build.part());
  for (int slot = 0; slot < build.slot_count(); ++slot) {
    const WeaponBuild* child = build.installed(slot);
    if (child == nullptr) {
      continue;
    }
    const PartDef& part = catalog.part(child->part());
    const std::string& slot_name = owner.slots[static_cast<size_t>(slot)].name;
    // out may grow below: copy what is needed of the parent first.
    const Placed& holder = out[static_cast<size_t>(parent)];
    if (holder.prefab == nullptr) {
      return VoidResult::Error(
          std::format("part '{}' has no model to hold '{}' in its slot '{}'",
                      owner.id, part.id, slot_name));
    }
    const std::string socket_name = std::string(kSlotSocketPrefix) + slot_name;
    const int socket_index =
        FindSocketIgnoringCase(*holder.prefab, socket_name);
    if (socket_index < 0) {
      return VoidResult::Error(
          std::format("part '{}' ({}) has no '{}' socket for '{}'", owner.id,
                      owner.prefab, socket_name, part.id));
    }
    const prefab::Socket& socket =
        holder.prefab->sockets[static_cast<size_t>(socket_index)];

    core::Result<std::optional<PartModel>, std::string> model =
        LoadModel(part, models);
    if (!model.ok()) {
      return VoidResult::Error(model.error());
    }
    Placed placed{.category = part.category};
    PartPlacement& placement = placed.placement;
    placement.part = child->part();
    placement.parent = parent;
    placement.parent_node = socket.node;
    glm::mat4 mount(1.0f);
    if (model.value().has_value()) {
      placed.prefab = model.value()->prefab;
      placement.prefab_key = model.value()->key;
      placed.rest = RestMatrices(*placed.prefab);
      if (const std::optional<glm::mat4> found =
              SocketRestMatrix(*placed.prefab, placed.rest, kMountSocket)) {
        mount = *found;
      }
    }
    placement.local = animation::ToMatrix(socket.local) * glm::inverse(mount);
    const glm::mat4 holder_node = RestNodeMatrix(holder.rest, socket.node);
    placement.rest = holder.placement.rest * holder_node * placement.local;
    if (parent == 0) {
      placement.root_node = socket.node;
      placement.root_local = placement.local;
    } else {
      placement.root_node = holder.placement.root_node;
      placement.root_local =
          holder.placement.root_local * holder_node * placement.local;
    }
    out.push_back(std::move(placed));
    const int index = static_cast<int>(out.size()) - 1;
    if (VoidResult placed_children =
            PlaceChildren(catalog, *child, index, models, out);
        !placed_children.ok()) {
      return placed_children;
    }
  }
  return VoidResult::Ok();
}

// Returns the root-space matrix of socket `name` of out[index], or nullopt.
std::optional<glm::mat4> PlacedSocket(const Placed& placed,
                                      std::string_view name) {
  if (placed.prefab == nullptr) {
    return std::nullopt;
  }
  const std::optional<glm::mat4> socket =
      SocketRestMatrix(*placed.prefab, placed.rest, name);
  if (!socket) {
    return std::nullopt;
  }
  return placed.placement.rest * *socket;
}

// A functional socket found on a part.
struct FoundSocket {
  glm::mat4 matrix{1.0f};
  size_t part = 0;  // index into the placements
};

// Returns socket `name` from the first part of the first category in
// `preferred` that has it, else from the first part (root first) that has
// it, else nullopt.
std::optional<FoundSocket> FindFunctional(
    std::span<const Placed> placed, std::string_view name,
    std::initializer_list<PartCategory> preferred) {
  for (const PartCategory category : preferred) {
    for (size_t i = 0; i < placed.size(); ++i) {
      if (placed[i].category != category) {
        continue;
      }
      if (const std::optional<glm::mat4> socket =
              PlacedSocket(placed[i], name)) {
        return FoundSocket{.matrix = *socket, .part = i};
      }
    }
  }
  for (size_t i = 0; i < placed.size(); ++i) {
    if (const std::optional<glm::mat4> socket = PlacedSocket(placed[i], name)) {
      return FoundSocket{.matrix = *socket, .part = i};
    }
  }
  return std::nullopt;
}

glm::vec3 Origin(const glm::mat4& matrix) { return glm::vec3(matrix[3]); }

// Returns the calibration of the placed parts; see AssembleWeapon.
WeaponGeometry GeometryFromParts(std::span<const Placed> placed) {
  WeaponGeometry geometry;
  geometry.scale = 1.0f;
  const std::optional<FoundSocket> muzzle = FindFunctional(
      placed, kMuzzleSocket, {PartCategory::kMuzzle, PartCategory::kBarrel});
  const std::optional<FoundSocket> sight = FindFunctional(
      placed, kSightSocket,
      {PartCategory::kOptic, PartCategory::kSightRear, PartCategory::kSlide});
  const std::optional<FoundSocket> grip =
      FindFunctional(placed, kGripSocket, {});

  constexpr glm::vec3 kForward(0.0f, 0.0f, 1.0f);
  if (sight) {
    geometry.sight_eye_point = Origin(sight->matrix);
    const glm::vec3 axis = glm::mat3(sight->matrix) * kForward;
    geometry.sight_axis =
        glm::length(axis) > 0.0f ? glm::normalize(axis) : kForward;
  } else {
    geometry.sight_eye_point = glm::vec3(0.0f);
    const glm::vec3 to_muzzle =
        muzzle ? Origin(muzzle->matrix) : glm::vec3(0.0f);
    geometry.sight_axis =
        glm::length(to_muzzle) > 0.0f ? glm::normalize(to_muzzle) : kForward;
  }
  geometry.muzzle = muzzle ? Origin(muzzle->matrix) : geometry.sight_eye_point;
  geometry.grip = grip ? Origin(grip->matrix) : glm::vec3(0.0f);

  geometry.optic_lens = geometry.sight_eye_point;
  geometry.optic_lens_radius = 0.0f;
  geometry.eyepiece = geometry.sight_eye_point;
  if (sight) {
    const Placed& optic = placed[sight->part];
    if (const std::optional<glm::mat4> lens =
            PlacedSocket(optic, kLensSocket)) {
      geometry.optic_lens = Origin(*lens);
      geometry.optic_lens_radius = glm::length(glm::vec3((*lens)[0]));
    }
    if (const std::optional<glm::mat4> eyepiece =
            PlacedSocket(optic, kEyepieceSocket)) {
      geometry.eyepiece = Origin(*eyepiece);
    }
  }
  return geometry;
}

// Returns `bounds` moved by `matrix`: the box around its eight corners.
asset::Bounds TransformBounds(const asset::Bounds& bounds,
                              const glm::mat4& matrix) {
  asset::Bounds out{.min = glm::vec3(0.0f), .max = glm::vec3(0.0f)};
  for (int corner = 0; corner < 8; ++corner) {
    const glm::vec3 local((corner & 1) != 0 ? bounds.max.x : bounds.min.x,
                          (corner & 2) != 0 ? bounds.max.y : bounds.min.y,
                          (corner & 4) != 0 ? bounds.max.z : bounds.min.z);
    const glm::vec3 moved = glm::vec3(matrix * glm::vec4(local, 1.0f));
    out.min = corner == 0 ? moved : glm::min(out.min, moved);
    out.max = corner == 0 ? moved : glm::max(out.max, moved);
  }
  return out;
}

// Returns the bounds of every placed part with meshes, at rest.
asset::Bounds CombinedBounds(std::span<const Placed> placed) {
  std::optional<asset::Bounds> combined;
  for (const Placed& part : placed) {
    if (part.prefab == nullptr || part.prefab->meshes.empty()) {
      continue;
    }
    const asset::Bounds moved =
        TransformBounds(part.prefab->bounds, part.placement.rest);
    combined = combined
                   ? asset::Bounds{.min = glm::min(combined->min, moved.min),
                                   .max = glm::max(combined->max, moved.max)}
                   : moved;
  }
  return combined.value_or(asset::Bounds{});
}

// Adds what every definition of a build carries: the build, its stats and
// their summaries.
void AddBuildData(const PartCatalog& catalog, const WeaponBuild& build,
                  WeaponDef& def) {
  def.build = build;
  def.stats = ComputeStats(catalog, build);
  def.stats_text = StatsSummary(*def.stats);
  def.missing_text = MissingSummary(*def.stats);
}

// Returns today's single-piece weapon `root.legacy_type` from `model`.
AssemblyResult AssembleLegacy(const PartCatalog& catalog,
                              const WeaponBuild& build, const PartDef& root,
                              const PartModel& model) {
  prefab::Prefab tagged = *model.prefab;  // Shares the bulk data.
  prefab::SetTag(tagged, kWeaponTag, root.legacy_type);
  core::Result<std::optional<WeaponDef>, std::string> made =
      MakeWeaponDef(tagged);
  if (!made.ok()) {
    return AssemblyResult::Error(
        std::format("part '{}': {}", root.id, made.error()));
  }
  WeaponDef def = std::move(*made.value());
  def.parts.front().part = build.part();
  def.parts.front().prefab_key = model.key;
  AddBuildData(catalog, build, def);
  return AssemblyResult::Ok(std::move(def));
}

}  // namespace

std::string BuildName(const PartCatalog& catalog, const WeaponBuild& build) {
  if (build.empty()) {
    return {};
  }
  const PresetDef* same_root = nullptr;
  for (const PresetDef& preset : catalog.presets()) {
    if (preset.root != build.part()) {
      continue;
    }
    if (same_root == nullptr) {
      same_root = &preset;
    }
    const core::Result<WeaponBuild, BuildError> made =
        MakeBuild(catalog, preset);
    if (made.ok() && made.value() == build) {
      return preset.name;
    }
  }
  return same_root != nullptr ? same_root->name
                              : catalog.part(build.part()).name;
}

core::Result<WeaponDef, std::string> AssembleWeapon(const PartCatalog& catalog,
                                                    const WeaponBuild& build,
                                                    const ModelSource& models) {
  if (build.empty()) {
    return AssemblyResult::Error("an empty build is no weapon");
  }
  const PartDef& root = catalog.part(build.part());
  core::Result<std::optional<PartModel>, std::string> root_model =
      LoadModel(root, models);
  if (!root_model.ok()) {
    return AssemblyResult::Error(root_model.error());
  }
  if (!root_model.value().has_value()) {
    return AssemblyResult::Error(
        std::format("part '{}' has no model to build a weapon on", root.id));
  }
  const PartModel& model = *root_model.value();
  if (root.category == PartCategory::kComplete && !root.legacy_type.empty()) {
    return AssembleLegacy(catalog, build, root, model);
  }

  std::vector<Placed> placed;
  placed.push_back({.category = root.category,
                    .prefab = model.prefab,
                    .rest = RestMatrices(*model.prefab)});
  placed.back().placement.part = build.part();
  placed.back().placement.prefab_key = model.key;
  if (VoidResult children = PlaceChildren(catalog, build, 0, models, placed);
      !children.ok()) {
    return AssemblyResult::Error(children.error());
  }

  core::Result<std::optional<FireAnimation>, std::string> fire =
      ReadFireAnimation(*model.prefab);
  if (!fire.ok()) {
    return AssemblyResult::Error(
        std::format("part '{}': {}", root.id, fire.error()));
  }

  WeaponDef def;
  def.geometry = GeometryFromParts(placed);
  def.bounds = CombinedBounds(placed);
  def.fire = std::move(fire).value();
  def.parts.reserve(placed.size());
  for (const Placed& part : placed) {
    def.parts.push_back(part.placement);
  }
  AddBuildData(catalog, build, def);
  def.name = BuildName(catalog, build);
  BuildTuning tuning = TuneFromStats(*def.stats, def.name);
  def.type = WeaponType{.id = {},
                        .spec = tuning.spec,
                        .viewmodel = tuning.viewmodel,
                        .aim_fov_scale = tuning.aim_fov_scale};
  def.accuracy_moa = tuning.accuracy_moa;
  def.zero_range = tuning.zero_range;
  def.switch_time_scale = tuning.switch_time_scale;
  // def.name moves with the def: the library points the view at its copy.
  def.type.spec.display_name = {};
  return AssemblyResult::Ok(std::move(def));
}

}  // namespace blue::weapon::parts
