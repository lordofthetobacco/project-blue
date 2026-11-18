// Registration of the editor's Prefab mode: its panels, viewport client,
// File > Import model... and save hooks, and its text commands.

#include <memory>
#include <string>
#include <utility>

#include "core/result.h"
#include "editor/dialogs.h"
#include "editor/document.h"
#include "editor/editor_app.h"
#include "editor/editor_context.h"
#include "editor/modes.h"
#include "editor/panel.h"
#include "editor/prefab_mode/prefab_commands.h"
#include "editor/prefab_mode/prefab_mode_state.h"
#include "editor/prefab_mode/prefab_panels.h"
#include "editor/prefab_mode/prefab_viewport.h"
#include "editor/viewport/viewport_panel.h"

namespace blue::editor {

void RegisterPrefabMode(EditorApp& app) {
  EditorContext& context = app.context();
  auto shared = std::make_unique<PrefabModeShared>();
  auto client = std::make_unique<PrefabViewportClient>(*shared);
  PrefabModeShared& state = *shared;
  context.viewport().SetClient(EditorMode::kPrefab, client.get());

  context.SetImportModelHandler([&context, &state] {
    context.dialogs().Open(
        FileKind::kModel, context.asset_root(),
        [&state](std::string path) { RequestImport(state, std::move(path)); });
  });
  context.SetPrefabSaveHandler(
      [](PrefabDocument& document, const std::string& path) {
        return SavePrefabChecked(document, path);
      });
  RegisterPrefabCommands(context, state);

  const auto desc = [](const char* name, DockSlot slot, bool open = true) {
    return PanelDesc{.name = name,
                     .modes = kPrefabModeOnly,
                     .slot = slot,
                     .open_by_default = open};
  };
  // The hierarchy panel owns the shared state and the viewport client.
  app.AddPanel(desc("Hierarchy", DockSlot::kLeft),
               std::make_unique<PrefabHierarchyPanel>(std::move(shared),
                                                      std::move(client)));
  app.AddPanel(desc("Inspector", DockSlot::kRight),
               std::make_unique<PrefabInspectorPanel>(state));
  app.AddPanel(desc("Materials", DockSlot::kRightBottom),
               std::make_unique<PrefabMaterialsPanel>(state));
  app.AddPanel(desc("Physics", DockSlot::kRightBottom),
               std::make_unique<PrefabPhysicsPanel>(state));
  app.AddPanel(desc("Prefab info", DockSlot::kRightBottom),
               std::make_unique<PrefabInfoPanel>(state));
  app.AddPanel(desc("Animation", DockSlot::kBottom),
               std::make_unique<PrefabAnimationPanel>(state));
  app.AddPanel(desc("UV layout", DockSlot::kBottom, /*open=*/false),
               std::make_unique<PrefabUvPanel>(state));
}

}  // namespace blue::editor
