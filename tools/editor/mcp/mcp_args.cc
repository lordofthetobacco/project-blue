#include "editor/mcp/mcp_args.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "core/result.h"
#include "material/material.h"
#include "mcp/json.h"
#include "prefab/prefab.h"
#include "scene/scene.h"
#include "scene/scene_data.h"

namespace blue::editor {
namespace {

using mcp::Json;
using IntResult = core::Result<int, std::string>;
using StringResult = core::Result<std::string, std::string>;

// Reads optional member `key` as an array of exactly N finite numbers.
template <size_t N>
core::Result<std::optional<std::array<float, N>>, std::string> GetFloats(
    const Json& args, std::string_view key) {
  using FloatsResult =
      core::Result<std::optional<std::array<float, N>>, std::string>;
  const Json* value = args.Find(key);
  if (value == nullptr || value->is_null()) {
    return FloatsResult::Ok(std::nullopt);
  }
  if (!value->is_array() || value->size() != N) {
    return FloatsResult::Error(
        std::format("{} must be an array of {} numbers", key, N));
  }
  std::array<float, N> result{};
  for (size_t i = 0; i < N; ++i) {
    const Json& item = value->array_items()[i];
    const double number = item.number_value();
    if (!item.is_number() || !std::isfinite(number)) {
      return FloatsResult::Error(
          std::format("{} must be an array of {} finite numbers", key, N));
    }
    result[i] = static_cast<float>(number);
  }
  return FloatsResult::Ok(result);
}

// Returns an error naming `key` unless every component lies in [min, max].
template <typename Vec>
ArgsStatus CheckRange(const Vec& v, std::string_view key, float min,
                      float max) {
  for (int i = 0; i < Vec::length(); ++i) {
    if (v[i] < min || v[i] > max) {
      return ArgsStatus::Error(
          std::format("{} components must lie in [{}, {}]", key, min, max));
    }
  }
  return ArgsStatus::Ok();
}

constexpr float kHuge = 1.0e9f;

// The parameters both material kinds share, read and range-checked.
struct SharedMaterialArgs {
  std::optional<glm::vec4> base_color;
  std::optional<float> metallic;
  std::optional<float> roughness;
  std::optional<glm::vec3> emissive;
  std::optional<float> emissive_strength;
  std::optional<glm::vec2> uv_scale;

  [[nodiscard]] int count() const {
    return int{base_color.has_value()} + int{metallic.has_value()} +
           int{roughness.has_value()} + int{emissive.has_value()} +
           int{emissive_strength.has_value()} + int{uv_scale.has_value()};
  }
};

core::Result<SharedMaterialArgs, std::string> ReadSharedMaterialArgs(
    const Json& args) {
  using SharedResult = core::Result<SharedMaterialArgs, std::string>;
  SharedMaterialArgs out;
  auto base_color = GetVec4Arg(args, "base_color");
  if (!base_color.ok()) {
    return SharedResult::Error(base_color.error());
  }
  if (base_color.value()) {
    if (auto ok = CheckRange(*base_color.value(), "base_color", 0.0f, 1.0f);
        !ok.ok()) {
      return SharedResult::Error(ok.error());
    }
    out.base_color = base_color.value();
  }
  auto metallic = GetFloatArg(args, "metallic", 0.0f, 1.0f);
  auto roughness = GetFloatArg(args, "roughness", 0.0f, 1.0f);
  auto strength = GetFloatArg(args, "emissive_strength", 0.0f, kHuge);
  if (!metallic.ok() || !roughness.ok() || !strength.ok()) {
    return SharedResult::Error(!metallic.ok()    ? metallic.error()
                               : !roughness.ok() ? roughness.error()
                                                 : strength.error());
  }
  out.metallic = metallic.value();
  out.roughness = roughness.value();
  out.emissive_strength = strength.value();
  auto emissive = GetVec3Arg(args, "emissive");
  if (!emissive.ok()) {
    return SharedResult::Error(emissive.error());
  }
  if (emissive.value()) {
    if (auto ok = CheckRange(*emissive.value(), "emissive", 0.0f, kHuge);
        !ok.ok()) {
      return SharedResult::Error(ok.error());
    }
    out.emissive = emissive.value();
  }
  auto uv_scale = GetVec2Arg(args, "uv_scale");
  if (!uv_scale.ok()) {
    return SharedResult::Error(uv_scale.error());
  }
  if (uv_scale.value()) {
    if (uv_scale.value()->x <= 0.0f || uv_scale.value()->y <= 0.0f) {
      return SharedResult::Error("uv_scale components must be > 0");
    }
    out.uv_scale = uv_scale.value();
  }
  return SharedResult::Ok(out);
}

// Resolves a clip given by name (string) or index (integer) in `prefab`.
IntResult ResolveClip(const Json* value, const prefab::Prefab& prefab) {
  if (value == nullptr) {
    return IntResult::Error("clip is required");
  }
  if (value->is_string()) {
    for (size_t i = 0; i < prefab.clips.size(); ++i) {
      if (prefab.clips[i].name == value->string_value()) {
        return IntResult::Ok(static_cast<int>(i));
      }
    }
    return IntResult::Error(
        std::format("no clip named '{}'", value->string_value()));
  }
  if (value->is_integer() && value->integer_value() >= 0 &&
      static_cast<size_t>(value->integer_value()) < prefab.clips.size()) {
    return IntResult::Ok(static_cast<int>(value->integer_value()));
  }
  return IntResult::Error("clip must be a clip name or a valid clip index");
}

}  // namespace

core::Result<std::optional<glm::vec2>, std::string> GetVec2Arg(
    const Json& args, std::string_view key) {
  using VecResult = core::Result<std::optional<glm::vec2>, std::string>;
  auto values = GetFloats<2>(args, key);
  if (!values.ok()) {
    return VecResult::Error(values.error());
  }
  if (!values.value()) {
    return VecResult::Ok(std::nullopt);
  }
  const std::array<float, 2>& v = *values.value();
  return VecResult::Ok(glm::vec2(v[0], v[1]));
}

core::Result<std::optional<glm::vec3>, std::string> GetVec3Arg(
    const Json& args, std::string_view key) {
  using VecResult = core::Result<std::optional<glm::vec3>, std::string>;
  auto values = GetFloats<3>(args, key);
  if (!values.ok()) {
    return VecResult::Error(values.error());
  }
  if (!values.value()) {
    return VecResult::Ok(std::nullopt);
  }
  const std::array<float, 3>& v = *values.value();
  return VecResult::Ok(glm::vec3(v[0], v[1], v[2]));
}

core::Result<std::optional<glm::vec4>, std::string> GetVec4Arg(
    const Json& args, std::string_view key) {
  using VecResult = core::Result<std::optional<glm::vec4>, std::string>;
  auto values = GetFloats<4>(args, key);
  if (!values.ok()) {
    return VecResult::Error(values.error());
  }
  if (!values.value()) {
    return VecResult::Ok(std::nullopt);
  }
  const std::array<float, 4>& v = *values.value();
  return VecResult::Ok(glm::vec4(v[0], v[1], v[2], v[3]));
}

core::Result<std::optional<float>, std::string> GetFloatArg(
    const Json& args, std::string_view key, float min, float max) {
  using FloatResult = core::Result<std::optional<float>, std::string>;
  const Json* value = args.Find(key);
  if (value == nullptr || value->is_null()) {
    return FloatResult::Ok(std::nullopt);
  }
  const double number = value->number_value();
  if (!value->is_number() || !std::isfinite(number)) {
    return FloatResult::Error(std::format("{} must be a number", key));
  }
  const auto result = static_cast<float>(number);
  if (result < min || result > max) {
    return FloatResult::Error(
        std::format("{} must lie in [{}, {}]", key, min, max));
  }
  return FloatResult::Ok(result);
}

core::Result<std::vector<scene::InstanceId>, std::string> GetInstanceIdsArg(
    const Json& args, std::string_view key) {
  using IdsResult = core::Result<std::vector<scene::InstanceId>, std::string>;
  std::vector<scene::InstanceId> ids;
  const Json* value = args.Find(key);
  if (value == nullptr || value->is_null()) {
    return IdsResult::Ok(std::move(ids));
  }
  const auto add = [&ids](const Json& item) {
    if (!item.is_integer() || item.integer_value() <= 0 ||
        item.integer_value() > int64_t{UINT32_MAX}) {
      return false;
    }
    ids.push_back(static_cast<scene::InstanceId>(item.integer_value()));
    return true;
  };
  bool ok = true;
  if (value->is_array()) {
    for (const Json& item : value->array_items()) {
      ok = ok && add(item);
    }
  } else {
    ok = add(*value);
  }
  if (!ok) {
    return IdsResult::Error(
        std::format("{} must be an instance id or an array of ids", key));
  }
  return IdsResult::Ok(std::move(ids));
}

core::Result<std::vector<std::string>, std::string> GetStringListArg(
    const Json& args, std::string_view key) {
  using ListResult = core::Result<std::vector<std::string>, std::string>;
  std::vector<std::string> list;
  const Json* value = args.Find(key);
  if (value == nullptr || value->is_null()) {
    return ListResult::Ok(std::move(list));
  }
  if (value->is_string()) {
    list.push_back(value->string_value());
    return ListResult::Ok(std::move(list));
  }
  if (value->is_array()) {
    for (const Json& item : value->array_items()) {
      if (!item.is_string()) {
        return ListResult::Error(
            std::format("{} must be a string or an array of strings", key));
      }
      list.push_back(item.string_value());
    }
    return ListResult::Ok(std::move(list));
  }
  return ListResult::Error(
      std::format("{} must be a string or an array of strings", key));
}

StringResult QuoteCommandWord(std::string_view word) {
  if (word.find_first_of("\"\r\n") != std::string_view::npos) {
    return StringResult::Error(std::format(
        "'{}' cannot be passed to an editor command (quote or line break)",
        word));
  }
  if (word.empty() || word.find_first_of(" \t;") != std::string_view::npos) {
    return StringResult::Ok(std::format("\"{}\"", word));
  }
  return StringResult::Ok(std::string(word));
}

StringResult BuildCommand(std::span<const std::string> words) {
  std::string line;
  for (const std::string& word : words) {
    StringResult quoted = QuoteCommandWord(word);
    if (!quoted.ok()) {
      return quoted;
    }
    line += line.empty() ? quoted.value() : " " + quoted.value();
  }
  return StringResult::Ok(std::move(line));
}

ArgsStatus ApplyTransformArgs(const Json& args, scene::Transform& transform) {
  auto position = GetVec3Arg(args, "position");
  auto rotation_deg = GetVec3Arg(args, "rotation_deg");
  auto rotation = GetVec4Arg(args, "rotation");
  if (!position.ok() || !rotation_deg.ok() || !rotation.ok()) {
    return ArgsStatus::Error(!position.ok()       ? position.error()
                             : !rotation_deg.ok() ? rotation_deg.error()
                                                  : rotation.error());
  }
  if (rotation_deg.value() && rotation.value()) {
    return ArgsStatus::Error("give rotation_deg or rotation, not both");
  }
  std::optional<glm::vec3> scale;
  if (const Json* value = args.Find("scale");
      value != nullptr && value->is_number()) {
    auto uniform = GetFloatArg(args, "scale", 1.0e-6f, kHuge);
    if (!uniform.ok()) {
      return ArgsStatus::Error("scale must be > 0");
    }
    scale = glm::vec3(*uniform.value());
  } else {
    auto vector = GetVec3Arg(args, "scale");
    if (!vector.ok()) {
      return ArgsStatus::Error(
          "scale must be a number or an array of 3 numbers");
    }
    scale = vector.value();
    if (scale && (scale->x <= 0.0f || scale->y <= 0.0f || scale->z <= 0.0f)) {
      return ArgsStatus::Error("scale components must be > 0");
    }
  }
  std::optional<glm::quat> quat;
  if (rotation.value()) {
    const glm::vec4& q = *rotation.value();
    if (glm::length(q) < 1.0e-6f) {
      return ArgsStatus::Error("rotation must be a non-zero quaternion");
    }
    quat = glm::normalize(glm::quat(q.w, q.x, q.y, q.z));
  } else if (rotation_deg.value()) {
    quat = glm::quat(glm::radians(*rotation_deg.value()));
  }
  if (!position.value() && !quat && !scale) {
    return ArgsStatus::Error(
        "give at least one of position, rotation_deg, rotation, scale");
  }
  if (position.value()) {
    transform.position = *position.value();
  }
  if (quat) {
    transform.rotation = *quat;
  }
  if (scale) {
    transform.scale = *scale;
  }
  return ArgsStatus::Ok();
}

IntResult ApplyMaterialArgs(const Json& args, prefab::Material& material) {
  auto shared = ReadSharedMaterialArgs(args);
  if (!shared.ok()) {
    return IntResult::Error(shared.error());
  }
  auto normal_scale = GetFloatArg(args, "normal_scale", 0.0f, kHuge);
  auto occlusion = GetFloatArg(args, "occlusion_strength", 0.0f, 1.0f);
  auto cutoff = GetFloatArg(args, "alpha_cutoff", 0.0f, 1.0f);
  if (!normal_scale.ok() || !occlusion.ok() || !cutoff.ok()) {
    return IntResult::Error(!normal_scale.ok() ? normal_scale.error()
                            : !occlusion.ok()  ? occlusion.error()
                                               : cutoff.error());
  }
  std::optional<material::AlphaMode> alpha_mode;
  if (const Json* mode = args.Find("alpha_mode"); mode != nullptr) {
    if (mode->is_string() && mode->string_value() == "opaque") {
      alpha_mode = material::AlphaMode::kOpaque;
    } else if (mode->is_string() && mode->string_value() == "mask") {
      alpha_mode = material::AlphaMode::kMask;
    } else {
      return IntResult::Error("alpha_mode must be \"opaque\" or \"mask\"");
    }
  }
  const std::optional<bool> double_sided = args.GetBool("double_sided");
  const SharedMaterialArgs& s = shared.value();
  const int count = s.count() + int{normal_scale.value().has_value()} +
                    int{occlusion.value().has_value()} +
                    int{cutoff.value().has_value()} +
                    int{alpha_mode.has_value()} + int{double_sided.has_value()};
  if (count == 0) {
    return IntResult::Error("give at least one material parameter");
  }
  material.base_color = s.base_color.value_or(material.base_color);
  material.metallic = s.metallic.value_or(material.metallic);
  material.roughness = s.roughness.value_or(material.roughness);
  material.emissive = s.emissive.value_or(material.emissive);
  material.emissive_strength =
      s.emissive_strength.value_or(material.emissive_strength);
  material.uv_scale = s.uv_scale.value_or(material.uv_scale);
  material.normal_scale = normal_scale.value().value_or(material.normal_scale);
  material.occlusion_strength =
      occlusion.value().value_or(material.occlusion_strength);
  material.alpha_cutoff = cutoff.value().value_or(material.alpha_cutoff);
  material.alpha_mode = alpha_mode.value_or(material.alpha_mode);
  material.double_sided = double_sided.value_or(material.double_sided);
  return IntResult::Ok(count);
}

IntResult ApplyOverrideArgs(const Json& args,
                            scene::MaterialOverride& material) {
  auto shared = ReadSharedMaterialArgs(args);
  if (!shared.ok()) {
    return IntResult::Error(shared.error());
  }
  const SharedMaterialArgs& s = shared.value();
  if (s.base_color) {
    material.base_color = s.base_color;
  }
  if (s.metallic) {
    material.metallic = s.metallic;
  }
  if (s.roughness) {
    material.roughness = s.roughness;
  }
  if (s.emissive) {
    material.emissive = s.emissive;
  }
  if (s.emissive_strength) {
    material.emissive_strength = s.emissive_strength;
  }
  if (s.uv_scale) {
    material.uv_scale = s.uv_scale;
  }
  return IntResult::Ok(s.count());
}

ArgsStatus ApplySunArgs(const Json& args, scene::SunLight& sun) {
  auto elevation = GetFloatArg(args, "elevation_deg", -90.0f, 90.0f);
  auto azimuth = GetFloatArg(args, "azimuth_deg", -kHuge, kHuge);
  auto direction = GetVec3Arg(args, "direction");
  auto color = GetVec3Arg(args, "color");
  auto intensity = GetFloatArg(args, "intensity", 0.0f, kHuge);
  for (const std::string* error :
       {elevation.ok() ? nullptr : &elevation.error(),
        azimuth.ok() ? nullptr : &azimuth.error(),
        direction.ok() ? nullptr : &direction.error(),
        color.ok() ? nullptr : &color.error(),
        intensity.ok() ? nullptr : &intensity.error()}) {
    if (error != nullptr) {
      return ArgsStatus::Error(*error);
    }
  }
  const bool angles = elevation.value() || azimuth.value();
  if (angles && direction.value()) {
    return ArgsStatus::Error("give direction or the angles, not both");
  }
  if (direction.value() && glm::length(*direction.value()) < 1.0e-6f) {
    return ArgsStatus::Error("direction must not be zero");
  }
  if (color.value()) {
    if (auto ok = CheckRange(*color.value(), "color", 0.0f, kHuge); !ok.ok()) {
      return ok;
    }
  }
  const std::optional<bool> shadows = args.GetBool("casts_shadows");
  if (!angles && !direction.value() && !color.value() && !intensity.value() &&
      !shadows) {
    return ArgsStatus::Error("give at least one sun parameter");
  }
  if (angles) {
    scene::SunAngles current = scene::SunAnglesFromDirection(sun.direction);
    current.elevation = elevation.value() ? glm::radians(*elevation.value())
                                          : current.elevation;
    current.azimuth =
        azimuth.value() ? glm::radians(*azimuth.value()) : current.azimuth;
    sun.direction = scene::SunDirectionFromAngles(current);
  } else if (direction.value()) {
    sun.direction = glm::normalize(*direction.value());
  }
  sun.color = color.value().value_or(sun.color);
  sun.intensity = intensity.value().value_or(sun.intensity);
  sun.casts_shadows = shadows.value_or(sun.casts_shadows);
  return ArgsStatus::Ok();
}

ArgsStatus ApplyEnvironmentArgs(const Json& args,
                                scene::Environment& environment) {
  auto intensity = GetFloatArg(args, "intensity", 0.0f, kHuge);
  auto rotation = GetFloatArg(args, "rotation_deg", -kHuge, kHuge);
  auto exposure = GetFloatArg(args, "exposure_ev", -20.0f, 20.0f);
  if (!intensity.ok() || !rotation.ok() || !exposure.ok()) {
    return ArgsStatus::Error(!intensity.ok()  ? intensity.error()
                             : !rotation.ok() ? rotation.error()
                                              : exposure.error());
  }
  environment.intensity = intensity.value().value_or(environment.intensity);
  if (rotation.value()) {
    environment.rotation_radians = glm::radians(*rotation.value());
  }
  environment.exposure_ev = exposure.value().value_or(environment.exposure_ev);
  return ArgsStatus::Ok();
}

ArgsStatus ApplyPointLightArgs(const Json& args,
                               scene::ScenePointLight& light) {
  auto position = GetVec3Arg(args, "position");
  auto color = GetVec3Arg(args, "color");
  auto intensity = GetFloatArg(args, "intensity", 0.0f, kHuge);
  auto range = GetFloatArg(args, "range", 1.0e-3f, kHuge);
  if (!position.ok() || !color.ok() || !intensity.ok() || !range.ok()) {
    return ArgsStatus::Error(!position.ok()    ? position.error()
                             : !color.ok()     ? color.error()
                             : !intensity.ok() ? intensity.error()
                                               : range.error());
  }
  if (color.value()) {
    if (auto ok = CheckRange(*color.value(), "color", 0.0f, kHuge); !ok.ok()) {
      return ok;
    }
  }
  light.position = position.value().value_or(light.position);
  light.color = color.value().value_or(light.color);
  light.intensity = intensity.value().value_or(light.intensity);
  light.range = range.value().value_or(light.range);
  if (const std::string* name = args.GetString("name");
      name != nullptr && !name->empty()) {
    light.name = *name;
  }
  return ArgsStatus::Ok();
}

IntResult ApplyBodyArgs(const Json& args, prefab::Physics& physics) {
  auto mass = GetFloatArg(args, "mass", 1.0e-6f, kHuge);
  auto friction = GetFloatArg(args, "friction", 0.0f, kHuge);
  auto restitution = GetFloatArg(args, "restitution", 0.0f, 1.0f);
  if (!mass.ok() || !friction.ok() || !restitution.ok()) {
    return IntResult::Error(!mass.ok()       ? "mass must be > 0"
                            : !friction.ok() ? friction.error()
                                             : restitution.error());
  }
  const std::optional<bool> dynamic = args.GetBool("dynamic");
  physics.dynamic = dynamic.value_or(physics.dynamic);
  physics.mass = mass.value().value_or(physics.mass);
  physics.friction = friction.value().value_or(physics.friction);
  physics.restitution = restitution.value().value_or(physics.restitution);
  return IntResult::Ok(
      int{dynamic.has_value()} + int{mass.value().has_value()} +
      int{friction.value().has_value()} + int{restitution.value().has_value()});
}

std::optional<prefab::ColliderKind> ParseColliderKindArg(
    std::string_view name) {
  static constexpr std::array<std::string_view, 6> kNames{
      "none", "box", "sphere", "capsule", "hull", "mesh"};
  for (size_t i = 0; i < kNames.size(); ++i) {
    if (kNames[i] == name) {
      return static_cast<prefab::ColliderKind>(i);
    }
  }
  return std::nullopt;
}

StringResult ClipEditCommand(const Json& args, const prefab::Prefab& prefab) {
  const std::string* action = args.GetString("action");
  if (action == nullptr) {
    return StringResult::Error("action is required");
  }
  std::vector<std::string> words;
  if (*action == "merge") {
    const std::string* name = args.GetString("name");
    const Json* clips = args.GetArray("clips");
    if (name == nullptr || name->empty() || clips == nullptr ||
        clips->size() < 2) {
      return StringResult::Error(
          "merge needs name and clips (at least two clip names or indices)");
    }
    words = {"clip-merge", *name};
    for (const Json& clip : clips->array_items()) {
      IntResult index = ResolveClip(&clip, prefab);
      if (!index.ok()) {
        return StringResult::Error(index.error());
      }
      words.push_back(std::to_string(index.value()));
    }
    return BuildCommand(words);
  }
  IntResult clip = ResolveClip(args.Find("clip"), prefab);
  if (!clip.ok()) {
    return StringResult::Error(clip.error());
  }
  const std::string index = std::to_string(clip.value());
  if (*action == "trim") {
    auto start = GetFloatArg(args, "start", 0.0f, kHuge);
    auto end = GetFloatArg(args, "end", 0.0f, kHuge);
    if (!start.ok() || !end.ok() || !start.value() || !end.value() ||
        *end.value() <= *start.value()) {
      return StringResult::Error(
          "trim needs start and end in seconds, end > start >= 0");
    }
    words = {"clip-trim", index, std::format("{}", *start.value()),
             std::format("{}", *end.value())};
  } else if (*action == "loop") {
    const std::optional<bool> loop = args.GetBool("loop");
    if (!loop) {
      return StringResult::Error("loop needs loop (boolean)");
    }
    words = {"clip-loop", index, *loop ? "1" : "0"};
  } else if (*action == "add_event") {
    const std::string* event = args.GetString("event");
    auto time = GetFloatArg(args, "time", 0.0f, kHuge);
    if (event == nullptr || event->empty() || !time.ok() || !time.value()) {
      return StringResult::Error("add_event needs event (name) and time >= 0");
    }
    words = {"event-add", index, *event, std::format("{}", *time.value())};
  } else if (*action == "remove_event") {
    const std::optional<int64_t> event = args.GetInteger("event_index");
    if (!event || *event < 0) {
      return StringResult::Error("remove_event needs event_index >= 0");
    }
    words = {"event-remove", index, std::to_string(*event)};
  } else {
    return StringResult::Error(
        "action must be merge, trim, loop, add_event or remove_event");
  }
  return BuildCommand(words);
}

std::string LimitText(std::string text, size_t max_bytes) {
  if (text.size() <= max_bytes) {
    return text;
  }
  size_t cut = text.rfind('\n', max_bytes);
  if (cut == std::string::npos || cut < max_bytes / 2) {
    cut = max_bytes;
    // Do not split a UTF-8 sequence.
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
      --cut;
    }
  }
  const size_t dropped = text.size() - cut;
  text.resize(cut);
  text += std::format("\n... ({} more bytes not shown)", dropped);
  return text;
}

}  // namespace blue::editor
