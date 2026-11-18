#include "editor/drag_drop.h"

#include <imgui.h>

#include <optional>
#include <string>

#include "core/path.h"
#include "editor/dialogs.h"

namespace blue::editor {

void AssetDragSource(const std::string& path) {
  if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
    ImGui::SetDragDropPayload(kAssetPathPayload, path.c_str(), path.size() + 1);
    ImGui::TextUnformatted(core::FileName(path).c_str());
    ImGui::EndDragDropSource();
  }
}

std::optional<std::string> AcceptAssetDrop(FileKind kind) {
  std::optional<std::string> dropped;
  if (!ImGui::BeginDragDropTarget()) {
    return dropped;
  }
  // Peek first so the item only highlights for paths of the right kind.
  const ImGuiPayload* peek = ImGui::GetDragDropPayload();
  if (peek != nullptr && peek->IsDataType(kAssetPathPayload) &&
      peek->DataSize > 0 &&
      MatchesFileKind(static_cast<const char*>(peek->Data), kind)) {
    if (const ImGuiPayload* payload =
            ImGui::AcceptDragDropPayload(kAssetPathPayload)) {
      dropped = std::string(static_cast<const char*>(payload->Data));
    }
  }
  ImGui::EndDragDropTarget();
  return dropped;
}

}  // namespace blue::editor
