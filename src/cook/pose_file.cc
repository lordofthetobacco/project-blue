#include "cook/pose_file.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/ext/quaternion_float.hpp>
#include <glm/ext/quaternion_geometric.hpp>
#include <glm/vec3.hpp>

#include "animation/transform.h"
#include "core/result.h"
#include "core/text.h"
#include "platform/file_system.h"
#include "prefab/prefab.h"

namespace blue::cook {
namespace {

using PoseResult = core::Result<PoseFile, std::string>;

constexpr std::string_view kWeaponKey = "weapon";
constexpr std::string_view kTranslationSuffix = ".translation";
constexpr std::string_view kRotationSuffix = ".rotation";
// How far a written quaternion's length may be from 1 (six decimals).
constexpr float kUnitTolerance = 0.001f;
constexpr size_t kMaxPoseFileBytes = 64 * 1024;

// One named transform while parsing; each half is set once.
struct NamedTransform {
  std::string name;
  std::optional<glm::vec3> translation;
  std::optional<glm::quat> rotation;
};

// Parses exactly N whitespace-separated finite floats.
template <size_t N>
std::optional<std::array<float, N>> ParseFloats(std::string_view text) {
  std::array<float, N> values{};
  for (size_t i = 0; i < N; ++i) {
    text = core::Trim(text);
    const size_t end = std::min(text.find_first_of(" \t"), text.size());
    const std::optional<float> value =
        core::ParseFiniteFloat(text.substr(0, end));
    if (!value) {
      return std::nullopt;
    }
    values[i] = *value;
    text = text.substr(end);
  }
  if (!core::Trim(text).empty()) {
    return std::nullopt;
  }
  return values;
}

NamedTransform& Named(std::vector<NamedTransform>& transforms,
                      std::string_view name) {
  for (NamedTransform& transform : transforms) {
    if (transform.name == name) {
      return transform;
    }
  }
  return transforms.emplace_back(NamedTransform{.name = std::string(name)});
}

// Applies one "key = value" line; returns an error message on failure.
std::optional<std::string> ApplyLine(std::string_view key,
                                     std::string_view value,
                                     std::optional<std::string>& weapon,
                                     std::vector<NamedTransform>& transforms) {
  if (key == kWeaponKey) {
    if (weapon) {
      return "weapon given twice";
    }
    if (value.empty()) {
      return "empty weapon";
    }
    weapon = std::string(value);
    return std::nullopt;
  }
  if (key.ends_with(kTranslationSuffix)) {
    const std::string_view name =
        key.substr(0, key.size() - kTranslationSuffix.size());
    const auto xyz = ParseFloats<3>(value);
    NamedTransform& transform = Named(transforms, name);
    if (name.empty() || !xyz || transform.translation) {
      return std::format("bad or repeated translation '{}'", value);
    }
    transform.translation = glm::vec3((*xyz)[0], (*xyz)[1], (*xyz)[2]);
    return std::nullopt;
  }
  if (key.ends_with(kRotationSuffix)) {
    const std::string_view name =
        key.substr(0, key.size() - kRotationSuffix.size());
    const auto wxyz = ParseFloats<4>(value);
    NamedTransform& transform = Named(transforms, name);
    if (name.empty() || !wxyz || transform.rotation) {
      return std::format("bad or repeated rotation '{}'", value);
    }
    const glm::quat rotation((*wxyz)[0], (*wxyz)[1], (*wxyz)[2], (*wxyz)[3]);
    if (std::abs(glm::length(rotation) - 1.0f) > kUnitTolerance) {
      return std::format("rotation '{}' is not a unit quaternion", value);
    }
    transform.rotation = glm::normalize(rotation);
    return std::nullopt;
  }
  return std::format("unknown key '{}'", key);
}

}  // namespace

core::Result<PoseFile, std::string> ParsePoseFile(std::string_view text) {
  std::optional<std::string> weapon;
  std::vector<NamedTransform> transforms;
  int line_number = 0;
  while (!text.empty()) {
    ++line_number;
    const size_t end = text.find('\n');
    const std::string_view line = core::Trim(text.substr(0, end));
    text = end == std::string_view::npos ? std::string_view()
                                         : text.substr(end + 1);
    if (line.empty() || line.front() == '#') {
      continue;
    }
    const size_t equals = line.find('=');
    if (equals == std::string_view::npos) {
      return PoseResult::Error(std::format("line {}: no '='", line_number));
    }
    if (std::optional<std::string> problem = ApplyLine(
            core::Trim(line.substr(0, equals)),
            core::Trim(line.substr(equals + 1)), weapon, transforms)) {
      return PoseResult::Error(
          std::format("line {}: {}", line_number, *problem));
    }
  }
  if (!weapon) {
    return PoseResult::Error("no weapon");
  }
  PoseFile pose{.weapon = std::move(*weapon)};
  bool weapon_placed = false;
  for (const NamedTransform& transform : transforms) {
    if (!transform.translation || !transform.rotation) {
      return PoseResult::Error(std::format(
          "'{}' needs both a translation and a rotation", transform.name));
    }
    const animation::Transform local{.translation = *transform.translation,
                                     .rotation = *transform.rotation};
    if (transform.name == kWeaponKey) {
      pose.weapon_local = local;
      weapon_placed = true;
    } else {
      pose.sockets.push_back({.name = transform.name, .local = local});
    }
  }
  if (!weapon_placed) {
    return PoseResult::Error("no weapon transform");
  }
  return PoseResult::Ok(std::move(pose));
}

core::Result<PoseFile, std::string> LoadPoseFile(const std::string& path) {
  auto bytes = platform::ReadFile(path, kMaxPoseFileBytes);
  if (!bytes.ok()) {
    return PoseResult::Error(std::format("cannot read {}: {}", path,
                                         platform::ToString(bytes.error())));
  }
  const std::vector<std::byte>& data = bytes.value();
  const std::string_view text(reinterpret_cast<const char*>(data.data()),
                              data.size());
  auto parsed = ParsePoseFile(text);
  if (!parsed.ok()) {
    return PoseResult::Error(std::format("{}: {}", path, parsed.error()));
  }
  return parsed;
}

}  // namespace blue::cook
