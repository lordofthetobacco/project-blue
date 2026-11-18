#ifndef BLUE_EDITOR_DRAG_DROP_H_
#define BLUE_EDITOR_DRAG_DROP_H_

#include <optional>
#include <string>

#include "editor/dialogs.h"

namespace blue::editor {

// ImGui drag-and-drop payload type of an asset file path: the UTF-8 path
// with its terminating null, as the asset browser sends it.
inline constexpr const char* kAssetPathPayload = "BLUE_ASSET_PATH";

// Makes the last ImGui item a drag source for `path` (call right after the
// item; shows the file name while dragging).
void AssetDragSource(const std::string& path);

// Makes the last ImGui item a drop target for asset paths of `kind`; returns
// the path dropped on it this frame. Paths of other kinds are not accepted
// (the item does not highlight for them).
[[nodiscard]] std::optional<std::string> AcceptAssetDrop(FileKind kind);

}  // namespace blue::editor

#endif  // BLUE_EDITOR_DRAG_DROP_H_
