#include "cook/prefab_edits.h"

#include <charconv>
#include <cmath>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <glm/ext/quaternion_float.hpp>
#include <glm/ext/quaternion_geometric.hpp>
#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include "animation/hierarchy.h"
#include "animation/transform.h"
#include "cook/clip_tools.h"
#include "prefab/prefab_file.h"

namespace blue::cook {
namespace {

using EditResult = core::Result<PrefabEdit, std::string>;
using Status = core::Result<void, std::string>;

constexpr std::string_view kTagOption = "--tag";
constexpr std::string_view kRemoveTagOption = "--remove-tag";
constexpr std::string_view kSocketOption = "--socket";
constexpr std::string_view kRemoveSocketOption = "--remove-socket";
constexpr std::string_view kEventOption = "--event";
constexpr std::string_view kRenameClipOption = "--rename-clip";

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

// Parses "<name>=<node>@<x>,<y>,<z>[/<dx>,<dy>,<dz>]".
std::optional<PrefabEdit> ParseSocket(std::string_view value) {
  const auto parts = Split(value, '=');
  const auto place = parts ? Split(parts->second, '@') : std::nullopt;
  if (!place || parts->first.empty() || place->first.empty()) {
    return std::nullopt;
  }
  std::string_view coordinates = place->second;
  std::optional<glm::vec3> aim;
  if (const auto with_aim = Split(coordinates, '/')) {
    coordinates = with_aim->first;
    aim = ParseVec3(with_aim->second);
    if (!aim) {
      return std::nullopt;
    }
  }
  const std::optional<glm::vec3> position = ParseVec3(coordinates);
  if (!position) {
    return std::nullopt;
  }
  return PrefabEdit{.kind = PrefabEdit::Kind::kSetSocket,
                    .target = std::string(parts->first),
                    .text = std::string(place->first),
                    .position = *position,
                    .aim = aim};
}

// Parses "<clip>=<name>@<time>".
std::optional<PrefabEdit> ParseEvent(std::string_view value) {
  const auto parts = Split(value, '=');
  const auto event = parts ? Split(parts->second, '@') : std::nullopt;
  const std::optional<float> time =
      event ? ParseFloat(event->second) : std::nullopt;
  if (!time || parts->first.empty() || event->first.empty()) {
    return std::nullopt;
  }
  return PrefabEdit{.kind = PrefabEdit::Kind::kAddEvent,
                    .target = std::string(parts->first),
                    .text = std::string(event->first),
                    .time = *time};
}

// Returns the rotation that turns +Z onto `direction` (unit length) with the
// least turn; a direction opposite to +Z turns about +Y.
glm::quat RotationFromZ(glm::vec3 direction) {
  const glm::vec3 z(0.0f, 0.0f, 1.0f);
  const float cosine = glm::dot(z, direction);
  if (cosine < -0.9999f) {
    return {0.0f, 0.0f, 1.0f, 0.0f};  // w x y z: 180 degrees about +Y
  }
  const glm::vec3 axis = glm::cross(z, direction);
  return glm::normalize(glm::quat(1.0f + cosine, axis.x, axis.y, axis.z));
}

Status ApplySetSocket(prefab::Prefab& prefab, const PrefabEdit& edit) {
  int node = -1;
  if (edit.text != "-") {
    node = animation::FindNode(prefab.hierarchy, edit.text);
    if (node < 0) {
      return Status::Error(
          std::format("socket '{}': no node '{}'", edit.target, edit.text));
    }
  }
  animation::Transform local{.translation = edit.position};
  if (edit.aim) {
    const float length = glm::length(*edit.aim);
    if (!(length > 0.0f) || !std::isfinite(length)) {
      return Status::Error(std::format(
          "socket '{}': the direction must not be zero", edit.target));
    }
    local.rotation = RotationFromZ(*edit.aim / length);
  }
  const prefab::Socket socket{
      .name = edit.target, .node = node, .local = local};
  const int existing = prefab::FindSocket(prefab, edit.target);
  if (existing >= 0) {
    prefab.sockets[static_cast<size_t>(existing)] = socket;
  } else {
    prefab.sockets.push_back(socket);
  }
  return Status::Ok();
}

Status ApplyClipChange(prefab::Prefab& prefab, const PrefabEdit& edit) {
  const int clip = prefab::FindClip(prefab, edit.target);
  if (clip < 0) {
    return Status::Error(std::format("no clip '{}'", edit.target));
  }
  if (edit.kind == PrefabEdit::Kind::kRenameClip) {
    if (!RenameClip(prefab, clip, edit.text).ok()) {
      return Status::Error(
          std::format("clip '{}': the new name is empty", edit.target));
    }
    return Status::Ok();
  }
  if (!AddClipEvent(prefab, clip, edit.text, edit.time).ok()) {
    return Status::Error(std::format(
        "clip '{}': event '{}' at {} s is outside [0, {}] s or unnamed",
        edit.target, edit.text, edit.time,
        prefab.clips[static_cast<size_t>(clip)].duration));
  }
  return Status::Ok();
}

}  // namespace

bool IsPrefabEditOption(std::string_view option) {
  for (const std::string_view known :
       {kTagOption, kRemoveTagOption, kSocketOption, kRemoveSocketOption,
        kEventOption, kRenameClipOption}) {
    if (option == known) {
      return true;
    }
  }
  return false;
}

core::Result<PrefabEdit, std::string> ParsePrefabEdit(std::string_view option,
                                                      std::string_view value) {
  const auto fail = [&](std::string_view syntax) {
    return EditResult::Error(
        std::format("{}: expected {}, got '{}'", option, syntax, value));
  };
  if (option == kTagOption) {
    const auto parts = Split(value, '=');
    if (!parts) {
      return fail("<key>=<value>");
    }
    return EditResult::Ok({.kind = PrefabEdit::Kind::kSetTag,
                           .target = std::string(parts->first),
                           .text = std::string(parts->second)});
  }
  if (option == kRemoveTagOption || option == kRemoveSocketOption) {
    if (value.empty()) {
      return fail("a name");
    }
    return EditResult::Ok({.kind = option == kRemoveTagOption
                                       ? PrefabEdit::Kind::kRemoveTag
                                       : PrefabEdit::Kind::kRemoveSocket,
                           .target = std::string(value)});
  }
  if (option == kSocketOption) {
    std::optional<PrefabEdit> socket = ParseSocket(value);
    if (!socket) {
      return fail("<name>=<node>@<x>,<y>,<z>[/<dx>,<dy>,<dz>]");
    }
    return EditResult::Ok(std::move(*socket));
  }
  if (option == kEventOption) {
    std::optional<PrefabEdit> event = ParseEvent(value);
    if (!event) {
      return fail("<clip>=<name>@<time>");
    }
    return EditResult::Ok(std::move(*event));
  }
  if (option == kRenameClipOption) {
    const auto parts = Split(value, '=');
    if (!parts || parts->first.empty() || parts->second.empty()) {
      return fail("<old>=<new>");
    }
    return EditResult::Ok({.kind = PrefabEdit::Kind::kRenameClip,
                           .target = std::string(parts->first),
                           .text = std::string(parts->second)});
  }
  return EditResult::Error(std::format("unknown edit option '{}'", option));
}

core::Result<void, std::string> ApplyPrefabEdit(prefab::Prefab& prefab,
                                                const PrefabEdit& edit) {
  switch (edit.kind) {
    case PrefabEdit::Kind::kSetTag:
      if (!prefab::IsValidTagKey(edit.target)) {
        return Status::Error(
            std::format("tag key '{}' must be 1 to {} characters of [a-z0-9_.]",
                        edit.target, prefab::kMaxTagKeyLength));
      }
      if (edit.text.size() > prefab::kMaxTagValueLength) {
        return Status::Error(std::format("tag '{}': value longer than {} bytes",
                                         edit.target,
                                         prefab::kMaxTagValueLength));
      }
      if (!prefab::FindTag(prefab, edit.target) &&
          prefab.tags.size() >= prefab::kMaxTagCount) {
        return Status::Error(
            std::format("at most {} tags allowed", prefab::kMaxTagCount));
      }
      prefab::SetTag(prefab, edit.target, edit.text);
      return Status::Ok();
    case PrefabEdit::Kind::kRemoveTag:
      if (!prefab::RemoveTag(prefab, edit.target)) {
        return Status::Error(std::format("no tag '{}'", edit.target));
      }
      return Status::Ok();
    case PrefabEdit::Kind::kSetSocket:
      return ApplySetSocket(prefab, edit);
    case PrefabEdit::Kind::kRemoveSocket: {
      const int socket = prefab::FindSocket(prefab, edit.target);
      if (socket < 0) {
        return Status::Error(std::format("no socket '{}'", edit.target));
      }
      prefab.sockets.erase(prefab.sockets.begin() + socket);
      return Status::Ok();
    }
    case PrefabEdit::Kind::kAddEvent:
    case PrefabEdit::Kind::kRenameClip:
      return ApplyClipChange(prefab, edit);
  }
  return Status::Error("unknown edit");
}

core::Result<prefab::Prefab, std::string> ApplyPrefabEdits(
    const prefab::Prefab& prefab, std::span<const PrefabEdit> edits) {
  using PrefabResult = core::Result<prefab::Prefab, std::string>;
  prefab::Prefab edited = prefab;  // Shares the bulk blocks: cheap.
  for (size_t i = 0; i < edits.size(); ++i) {
    Status applied = ApplyPrefabEdit(edited, edits[i]);
    if (!applied.ok()) {
      return PrefabResult::Error(
          std::format("edit {}: {}", i + 1, applied.error()));
    }
  }
  return PrefabResult::Ok(std::move(edited));
}

core::Result<void, std::string> EditPrefabFile(
    const std::string& input, const std::string& output,
    std::span<const PrefabEdit> edits) {
  core::Result<prefab::Prefab, prefab::PrefabError> loaded =
      prefab::LoadPrefab(input);
  if (!loaded.ok()) {
    return Status::Error(std::format("cannot load {}: {}", input,
                                     prefab::ToString(loaded.error())));
  }
  core::Result<prefab::Prefab, std::string> edited =
      ApplyPrefabEdits(loaded.value(), edits);
  if (!edited.ok()) {
    return Status::Error(edited.error());
  }
  if (std::optional<std::string> problem =
          prefab::ValidatePrefab(edited.value())) {
    return Status::Error(
        std::format("the edited prefab is invalid: {}", *problem));
  }
  // SavePrefab writes a temporary file and renames it over `output`; the
  // mapping of `input` keeps its old contents meanwhile.
  if (core::Result<void, prefab::PrefabError> saved =
          prefab::SavePrefab(edited.value(), output);
      !saved.ok()) {
    return Status::Error(std::format("cannot write {}: {}", output,
                                     prefab::ToString(saved.error())));
  }
  return Status::Ok();
}

}  // namespace blue::cook
