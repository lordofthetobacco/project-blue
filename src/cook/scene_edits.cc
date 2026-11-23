#include "cook/scene_edits.h"

#include <charconv>
#include <cmath>
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
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>

#include "platform/file_system.h"
#include "scene/mscn_serializer.h"
#include "scene/scene_data.h"

namespace blue::cook {
namespace {

using EditResult = core::Result<SceneEdit, std::string>;
using Status = core::Result<void, std::string>;

constexpr std::string_view kAddInstanceOption = "--add-instance";
constexpr std::string_view kRemoveInstanceOption = "--remove-instance";
constexpr std::string_view kAddInstanceSyntax =
    "--add-instance <name>=<prefab.pbo>@<x>,<y>,<z>[/<yaw degrees>]";

// Splits "a<separator>b" at the first separator.
std::optional<std::pair<std::string_view, std::string_view>> Split(
    std::string_view text, char separator) {
  const size_t at = text.find(separator);
  if (at == std::string_view::npos) {
    return std::nullopt;
  }
  return std::pair{text.substr(0, at), text.substr(at + 1)};
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

// Parses "<x>,<y>,<z>".
std::optional<glm::vec3> ParseVec3(std::string_view text) {
  const auto xy = Split(text, ',');
  const auto yz = xy ? Split(xy->second, ',') : std::nullopt;
  const std::optional<float> x = xy ? ParseFloat(xy->first) : std::nullopt;
  const std::optional<float> y = yz ? ParseFloat(yz->first) : std::nullopt;
  const std::optional<float> z = yz ? ParseFloat(yz->second) : std::nullopt;
  if (!x || !y || !z) {
    return std::nullopt;
  }
  return glm::vec3(*x, *y, *z);
}

// Parses "<name>=<prefab>@<x>,<y>,<z>[/<yaw degrees>]".
std::optional<SceneEdit> ParseAddInstance(std::string_view value) {
  const auto parts = Split(value, '=');
  if (!parts || parts->first.empty()) {
    return std::nullopt;
  }
  const size_t at = parts->second.rfind('@');
  if (at == std::string_view::npos || at == 0) {
    return std::nullopt;
  }
  std::string_view coordinates = parts->second.substr(at + 1);
  std::optional<float> yaw = 0.0f;
  if (const auto with_yaw = Split(coordinates, '/')) {
    coordinates = with_yaw->first;
    yaw = ParseFloat(with_yaw->second);
  }
  const std::optional<glm::vec3> position = ParseVec3(coordinates);
  if (!position || !yaw) {
    return std::nullopt;
  }
  return SceneEdit{.kind = SceneEdit::Kind::kAddInstance,
                   .name = std::string(parts->first),
                   .prefab_path = std::string(parts->second.substr(0, at)),
                   .position = *position,
                   .yaw_degrees = *yaw};
}

// Returns how many instances of `scene` are called `name`, and the id of the
// last one.
std::pair<int, scene::InstanceId> CountNamed(const scene::SceneData& scene,
                                             std::string_view name) {
  int count = 0;
  scene::InstanceId id = scene::kNoInstance;
  for (const scene::Instance& instance : scene.instances) {
    if (instance.name == name) {
      ++count;
      id = instance.id;
    }
  }
  return {count, id};
}

Status ApplyAddInstance(scene::SceneData& scene, const SceneEdit& edit) {
  if (edit.prefab_path.empty()) {
    return Status::Error(
        std::format("instance '{}': empty prefab path", edit.name));
  }
  if (CountNamed(scene, edit.name).first > 0) {
    return Status::Error(
        std::format("an instance '{}' already exists", edit.name));
  }
  scene::SceneData edited = scene;
  scene::Instance instance{
      .name = edit.name,
      .prefab = scene::AddPrefab(edited, edit.prefab_path),
      .transform = {.position = edit.position,
                    .rotation = glm::angleAxis(glm::radians(edit.yaw_degrees),
                                               glm::vec3(0.0f, 1.0f, 0.0f))}};
  if (scene::AddInstance(edited, std::move(instance)) == scene::kNoInstance) {
    return Status::Error(
        std::format("instance '{}' cannot be added", edit.name));
  }
  scene = std::move(edited);
  return Status::Ok();
}

Status ApplyRemoveInstance(scene::SceneData& scene, const SceneEdit& edit) {
  const auto [count, id] = CountNamed(scene, edit.name);
  if (count == 0) {
    return Status::Error(std::format("no instance '{}'", edit.name));
  }
  if (count > 1) {
    return Status::Error(std::format(
        "{} instances are called '{}'; rename them in the editor first", count,
        edit.name));
  }
  scene::RemoveInstance(scene, id);
  return Status::Ok();
}

}  // namespace

bool IsSceneEditOption(std::string_view option) {
  return option == kAddInstanceOption || option == kRemoveInstanceOption;
}

core::Result<SceneEdit, std::string> ParseSceneEdit(std::string_view option,
                                                    std::string_view value) {
  if (option == kAddInstanceOption) {
    std::optional<SceneEdit> edit = ParseAddInstance(value);
    if (!edit) {
      return EditResult::Error(std::format("bad value '{}', expected {}", value,
                                           kAddInstanceSyntax));
    }
    return EditResult::Ok(std::move(*edit));
  }
  if (option == kRemoveInstanceOption) {
    if (value.empty()) {
      return EditResult::Error(
          "expected --remove-instance <name> with a non-empty name");
    }
    return EditResult::Ok(
        {.kind = SceneEdit::Kind::kRemoveInstance, .name = std::string(value)});
  }
  return EditResult::Error(std::format("unknown scene edit '{}'", option));
}

core::Result<void, std::string> ApplySceneEdit(scene::SceneData& scene,
                                               const SceneEdit& edit) {
  if (edit.name.empty()) {
    return Status::Error("empty instance name");
  }
  switch (edit.kind) {
    case SceneEdit::Kind::kAddInstance:
      return ApplyAddInstance(scene, edit);
    case SceneEdit::Kind::kRemoveInstance:
      return ApplyRemoveInstance(scene, edit);
  }
  return Status::Error("unknown scene edit");
}

core::Result<scene::SceneData, std::string> ApplySceneEdits(
    const scene::SceneData& scene, std::span<const SceneEdit> edits) {
  using SceneResult = core::Result<scene::SceneData, std::string>;
  scene::SceneData edited = scene;
  for (size_t i = 0; i < edits.size(); ++i) {
    if (Status applied = ApplySceneEdit(edited, edits[i]); !applied.ok()) {
      return SceneResult::Error(
          std::format("edit {}: {}", i + 1, applied.error()));
    }
  }
  return SceneResult::Ok(std::move(edited));
}

core::Result<void, std::string> EditSceneFile(
    const std::string& input, const std::string& output,
    std::span<const SceneEdit> edits) {
  core::Result<scene::SceneData, scene::MscnError> loaded =
      scene::LoadMscn(input);
  if (!loaded.ok()) {
    return Status::Error(std::format("cannot load {}: {}", input,
                                     scene::ToString(loaded.error())));
  }
  std::vector<SceneEdit> resolved(edits.begin(), edits.end());
  for (SceneEdit& edit : resolved) {
    if (edit.kind != SceneEdit::Kind::kAddInstance) {
      continue;
    }
    edit.prefab_path = platform::AbsolutePath(edit.prefab_path);
    if (!platform::IsRegularFile(edit.prefab_path)) {
      return Status::Error(std::format("instance '{}': no prefab file {}",
                                       edit.name, edit.prefab_path));
    }
  }
  core::Result<scene::SceneData, std::string> edited =
      ApplySceneEdits(loaded.value(), resolved);
  if (!edited.ok()) {
    return Status::Error(edited.error());
  }
  if (std::optional<std::string> problem =
          scene::CheckMscnSerializable(edited.value(), output)) {
    return Status::Error(
        std::format("the edited scene cannot be saved: {}", *problem));
  }
  if (core::Result<void, scene::MscnError> saved =
          scene::SaveMscn(edited.value(), output);
      !saved.ok()) {
    return Status::Error(std::format("cannot write {}: {}", output,
                                     scene::ToString(saved.error())));
  }
  return Status::Ok();
}

}  // namespace blue::cook
