#ifndef BLUE_EDITOR_ASSET_BROWSER_H_
#define BLUE_EDITOR_ASSET_BROWSER_H_

#include <string>
#include <vector>

#include "editor/editor_context.h"
#include "editor/panel.h"

namespace blue::editor {

// The "Assets" panel: a directory tree under the context's asset root
// showing prefabs, scenes, models, images and HDRIs. Double-click opens a
// .pbo / .mscn; every file is a drag source (kAssetPathPayload) for the mode
// panels and the viewport. The listing is cached and re-read on "Refresh"
// or when a directory is expanded, not every frame.
class AssetBrowser final : public Panel {
 public:
  AssetBrowser() = default;

  void Draw(EditorContext& context) override;

 private:
  struct Entry {
    std::string name;
    std::string path;
    bool directory = false;
    // Children, read when first expanded.
    std::vector<Entry> children;
    bool listed = false;
  };

  // Reads the entries of `entry` (a directory) from disk.
  static void List(Entry& entry);
  void DrawEntry(EditorContext& context, Entry& entry);

  std::string root_path_;
  Entry root_;
};

}  // namespace blue::editor

#endif  // BLUE_EDITOR_ASSET_BROWSER_H_
