#ifndef BLUE_EDITOR_PREFAB_MODE_PREFAB_WIDGETS_H_
#define BLUE_EDITOR_PREFAB_MODE_PREFAB_WIDGETS_H_

#include <imgui.h>

#include <string>

#include <glm/vec3.hpp>

#include "animation/transform.h"
#include "editor/document.h"
#include "editor/editor_context.h"
#include "editor/prefab_mode/prefab_mode_state.h"
#include "editor/undo.h"
#include "prefab/prefab.h"

// ImGui building blocks of the Prefab mode panels. Every function must run
// inside an ImGui window. Edits go through the document's undo history:
// drags and text fields form one step from activation to release.

namespace blue::editor {

// Shows a text field holding `value`. Returns true once the user finished
// editing (Enter or leaving the field) with a different text, which is
// written to `edited`. The typed text lives in `shared` while active.
bool InputTextCommit(PrefabModeShared& shared, const char* label,
                     const std::string& value, std::string& edited);

// Shows a text field editing `text` directly (modal dialogs); returns true
// when it changed.
bool InputString(const char* label, std::string& text);

// What a group of fields did this frame.
struct FieldEdit {
  bool changed = false;   // a value changed
  bool released = false;  // a field was released after changing
};

// Shows translation, rotation (Euler degrees) and scale fields for
// `transform`. `show_scale` false hides the scale row.
FieldEdit TransformFields(const char* id, animation::Transform& transform,
                          EulerCache& euler, bool show_scale = true);

// Shows a combo choosing a node of `prefab` (or "(prefab root)" = -1);
// returns true when `node` changed.
bool NodeCombo(const char* label, const prefab::Prefab& prefab, int& node);

// Ends the open edit of `document` when `released` (fields were released
// after changing them). `structural` upgrades the step to a structural change
// first (e.g. node rest transforms, which the preview only picks up on
// re-upload).
void CommitOnRelease(PrefabDocument& document, bool released,
                     bool structural = false);

// Shows the editors of material `index`: name, every parameter, the texture
// slots (thumbnail, replace from file or by dropping an image, remove, UV
// set, ORM packing) and the texture import settings.
void DrawMaterialEditor(EditorContext& context, PrefabDocument& document,
                        PrefabModeShared& shared, int index);

// Shows the physics body editor: collider kind and parameters, fit to
// bounds, re-cook, body settings.
void DrawPhysicsEditor(EditorContext& context, PrefabDocument& document,
                       PrefabModeShared& shared);

// Opens the image dialog and imports the chosen file into slot `slot` of
// material `material` of the prefab document open when it closes.
void ReplaceSlotTextureFromDialog(EditorContext& context,
                                  PrefabModeShared& shared, int material,
                                  prefab::TextureSlot slot);

// Imports `path` into slot `slot` of material `material` as one undo step;
// errors are shown. Returns success.
bool ImportSlotTextureEdit(EditorContext& context, PrefabDocument& document,
                           const PrefabModeShared& shared, int material,
                           prefab::TextureSlot slot, const std::string& path);

// Returns the label of `slot` ("Base colour", "ORM", ...).
[[nodiscard]] const char* SlotLabel(prefab::TextureSlot slot);

}  // namespace blue::editor

#endif  // BLUE_EDITOR_PREFAB_MODE_PREFAB_WIDGETS_H_
