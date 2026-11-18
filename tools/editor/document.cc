#include "editor/document.h"

#include <format>
#include <optional>
#include <string>
#include <utility>

#include "core/path.h"
#include "core/result.h"
#include "prefab/prefab.h"
#include "prefab/prefab_file.h"
#include "scene/mscn_serializer.h"
#include "scene/scene_data.h"

namespace blue::editor {

namespace {

using VoidResult = core::Result<void, std::string>;

// Returns `path`, or the document's own path when `path` is empty; nullopt
// when both are empty.
template <typename State>
std::optional<std::string> TargetPath(const Document<State>& document,
                                      const std::string& path) {
  if (!path.empty()) {
    return path;
  }
  if (!document.path().empty()) {
    return document.path();
  }
  return std::nullopt;
}

}  // namespace

core::Result<PrefabDocument, std::string> LoadPrefabDocument(
    const std::string& path) {
  core::Result<prefab::Prefab, prefab::PrefabError> loaded =
      prefab::LoadPrefab(path);
  if (!loaded.ok()) {
    return core::Result<PrefabDocument, std::string>::Error(
        std::format("{}: {}", path, prefab::ToString(loaded.error())));
  }
  return core::Result<PrefabDocument, std::string>::Ok(
      PrefabDocument(std::move(loaded.value()), path));
}

VoidResult SavePrefabDocument(PrefabDocument& document,
                              const std::string& path) {
  document.CommitEdit();
  const std::optional<std::string> target = TargetPath(document, path);
  if (!target) {
    return VoidResult::Error("the prefab has no file name yet");
  }
  if (std::optional<std::string> problem =
          prefab::ValidatePrefab(document.state())) {
    return VoidResult::Error(
        std::format("{}: cannot save: {}", *target, *problem));
  }
  core::Result<void, prefab::PrefabError> saved =
      prefab::SavePrefab(document.state(), *target);
  if (!saved.ok()) {
    return VoidResult::Error(
        std::format("{}: {}", *target, prefab::ToString(saved.error())));
  }
  document.set_path(*target);
  document.MarkSaved();
  return VoidResult::Ok();
}

core::Result<SceneDocument, std::string> LoadSceneDocument(
    const std::string& path) {
  core::Result<scene::SceneData, scene::MscnError> loaded =
      scene::LoadMscn(path);
  if (!loaded.ok()) {
    return core::Result<SceneDocument, std::string>::Error(
        std::format("{}: {}", path, scene::ToString(loaded.error())));
  }
  return core::Result<SceneDocument, std::string>::Ok(
      SceneDocument(std::move(loaded.value()), path));
}

VoidResult SaveSceneDocument(SceneDocument& document, const std::string& path) {
  document.CommitEdit();
  const std::optional<std::string> target = TargetPath(document, path);
  if (!target) {
    return VoidResult::Error("the scene has no file name yet");
  }
  if (std::optional<std::string> problem =
          scene::CheckMscnSerializable(document.state(), *target)) {
    return VoidResult::Error(
        std::format("{}: cannot save: {}", *target, *problem));
  }
  core::Result<void, scene::MscnError> saved =
      scene::SaveMscn(document.state(), *target);
  if (!saved.ok()) {
    return VoidResult::Error(
        std::format("{}: {}", *target, scene::ToString(saved.error())));
  }
  document.set_path(*target);
  document.MarkSaved();
  return VoidResult::Ok();
}

std::string DisplayPath(const std::string& path, const std::string& root) {
  if (root.empty()) {
    return path;
  }
  const std::optional<std::string> relative = core::RelativePath(path, root);
  if (!relative || *relative == "." || relative->starts_with("..")) {
    return path;
  }
  return *relative;
}

std::string DocumentTitle(const std::string& path) {
  return path.empty() ? std::string("Untitled") : core::FileName(path);
}

}  // namespace blue::editor
