#include "editor/asset_browser.h"

#include <SDL3/SDL_filesystem.h>
#include <imgui.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "core/path.h"
#include "editor/dialogs.h"
#include "editor/drag_drop.h"
#include "editor/editor_context.h"

namespace blue::editor {

namespace {

// Returns whether the browser lists `path` (a file): the kinds the editor
// knows about.
bool IsListedFile(const std::string& path) {
  return MatchesFileKind(path, FileKind::kPrefab) ||
         MatchesFileKind(path, FileKind::kScene) ||
         MatchesFileKind(path, FileKind::kModel) ||
         MatchesFileKind(path, FileKind::kImage) ||
         MatchesFileKind(path, FileKind::kHdr);
}

// Returns a short tag shown before a file name.
const char* KindTag(const std::string& path) {
  if (MatchesFileKind(path, FileKind::kPrefab)) {
    return "[pbo] ";
  }
  if (MatchesFileKind(path, FileKind::kScene)) {
    return "[scn] ";
  }
  if (MatchesFileKind(path, FileKind::kModel)) {
    return "[mdl] ";
  }
  if (MatchesFileKind(path, FileKind::kHdr)) {
    return "[hdr] ";
  }
  return "[img] ";
}

// SDL_EnumerateDirectory callback: collects entry names.
SDL_EnumerationResult CollectName(void* user_data, const char* /*dirname*/,
                                  const char* name) {
  static_cast<std::vector<std::string>*>(user_data)->emplace_back(name);
  return SDL_ENUM_CONTINUE;
}

}  // namespace

void AssetBrowser::List(Entry& entry) {
  entry.children.clear();
  entry.listed = true;
  std::vector<std::string> names;
  if (!SDL_EnumerateDirectory(entry.path.c_str(), &CollectName, &names)) {
    return;
  }
  for (std::string& name : names) {
    std::string path = core::JoinPath(entry.path, name);
    SDL_PathInfo info{};
    if (!SDL_GetPathInfo(path.c_str(), &info)) {
      continue;
    }
    const bool directory = info.type == SDL_PATHTYPE_DIRECTORY;
    if (!directory && !IsListedFile(path)) {
      continue;
    }
    entry.children.push_back({.name = std::move(name),
                              .path = std::move(path),
                              .directory = directory});
  }
  // Directories first, then by name.
  std::sort(entry.children.begin(), entry.children.end(),
            [](const Entry& a, const Entry& b) {
              return a.directory != b.directory ? a.directory : a.name < b.name;
            });
}

void AssetBrowser::Draw(EditorContext& context) {
  if (root_path_ != context.asset_root()) {
    root_path_ = context.asset_root();
    root_ = {.name = core::FileName(root_path_),
             .path = root_path_,
             .directory = true};
  }
  if (ImGui::SmallButton("Refresh")) {
    root_.listed = false;
  }
  ImGui::SameLine();
  // The root's own name; the full path in the tooltip.
  ImGui::TextDisabled("%s/", root_.name.c_str());
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", root_path_.c_str());
  }
  ImGui::Separator();
  if (!root_.listed) {
    List(root_);
  }
  if (root_.children.empty()) {
    ImGui::TextDisabled("No assets here");
  }
  for (Entry& child : root_.children) {
    DrawEntry(context, child);
  }
}

void AssetBrowser::DrawEntry(EditorContext& context, Entry& entry) {
  if (entry.directory) {
    const bool open =
        ImGui::TreeNodeEx(entry.path.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth,
                          "%s", entry.name.c_str());
    if (ImGui::IsItemToggledOpen() || (open && !entry.listed)) {
      List(entry);  // Re-read on every expand.
    }
    if (open) {
      for (Entry& child : entry.children) {
        DrawEntry(context, child);
      }
      ImGui::TreePop();
    }
    return;
  }
  const std::string label = std::string(KindTag(entry.path)) + entry.name;
  ImGui::PushID(entry.path.c_str());
  ImGui::Selectable(label.c_str(), false,
                    ImGuiSelectableFlags_AllowDoubleClick);
  const bool opened = ImGui::IsItemHovered() &&
                      ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
  AssetDragSource(entry.path);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", entry.path.c_str());
  }
  ImGui::PopID();
  if (opened && (MatchesFileKind(entry.path, FileKind::kPrefab) ||
                 MatchesFileKind(entry.path, FileKind::kScene))) {
    (void)context.OpenFile(entry.path);
  }
}

}  // namespace blue::editor
