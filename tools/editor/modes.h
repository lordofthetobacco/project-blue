#ifndef BLUE_EDITOR_MODES_H_
#define BLUE_EDITOR_MODES_H_

namespace blue::editor {

class EditorApp;

// Each part of the editor adds its panels, commands and hooks here; called
// once by main() before EditorApp::Run(), after EditorApp created the
// viewport panel (EditorContext::viewport()). Implemented in the part's own
// directory (owners: prefab_mode/ editor-prefab, scene_mode/ editor-scene).
void RegisterPrefabMode(EditorApp& app);
void RegisterSceneMode(EditorApp& app);

}  // namespace blue::editor

#endif  // BLUE_EDITOR_MODES_H_
