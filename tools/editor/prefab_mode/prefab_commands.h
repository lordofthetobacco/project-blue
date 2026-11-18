#ifndef BLUE_EDITOR_PREFAB_MODE_PREFAB_COMMANDS_H_
#define BLUE_EDITOR_PREFAB_MODE_PREFAB_COMMANDS_H_

#include "editor/editor_context.h"
#include "editor/prefab_mode/prefab_mode_state.h"

namespace blue::editor {

// Registers Prefab mode's text commands (for --exec scripts, the command
// line and automated checks) in context.commands(). They act on the open
// prefab document like the panels do, each as one undo step, and fail with
// a readable error when there is none or an argument is bad. "help" lists
// them with their usage. Borrows `context` and `shared` for the lifetime of
// the registry.
void RegisterPrefabCommands(EditorContext& context, PrefabModeShared& shared);

}  // namespace blue::editor

#endif  // BLUE_EDITOR_PREFAB_MODE_PREFAB_COMMANDS_H_
