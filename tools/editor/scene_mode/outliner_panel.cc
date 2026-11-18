// Scene mode's Outliner: the instance forest (groups, visibility, selection,
// drag to reparent, prefab drops), the scene's lights and singletons, and
// the toolbar. Owns the Scene mode state and runs its per-frame Sync() and
// the Scene mode shortcuts.

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glm/vec3.hpp>

#include "editor/dialogs.h"
#include "editor/drag_drop.h"
#include "editor/editor_context.h"
#include "editor/panel.h"
#include "editor/scene_mode/scene_edit.h"
#include "editor/scene_mode/scene_mode.h"
#include "editor/selection.h"
#include "editor/viewport/viewport_panel.h"
#include "scene/scene_data.h"

namespace blue::editor {
namespace {

// ImGui drag-and-drop payload of an outliner row: the scene::InstanceId.
constexpr const char* kInstancePayload = "BLUE_SCENE_INSTANCE";

class OutlinerPanel final : public Panel {
 public:
  OutlinerPanel(std::unique_ptr<SceneModeState> state,
                std::unique_ptr<ViewportClient> client)
      : state_(std::move(state)), client_(std::move(client)) {}

  void Update(EditorContext& context, float dt) override;
  void Draw(EditorContext& context) override;

 private:
  void HandleShortcuts(EditorContext& context);
  void DrawToolbar(EditorContext& context);
  void DrawInstanceRow(SceneDocument& doc, const OutlinerRow& row);
  // Makes the last item a drop target that reparents dragged instances
  // under `parent` (kNoInstance = root) and places dropped prefabs there.
  void AcceptDrops(SceneDocument& doc, scene::InstanceId parent);
  void DrawSceneObjects(SceneDocument& doc);

  std::unique_ptr<SceneModeState> state_;
  std::unique_ptr<ViewportClient> client_;  // registered with the viewport
  std::vector<scene::InstanceId> collapsed_;
  std::vector<OutlinerRow> rows_;
};

void OutlinerPanel::Update(EditorContext& context, float /*dt*/) {
  state_->Sync();
  if (context.mode() == EditorMode::kScene) {
    HandleShortcuts(context);
  }
}

void OutlinerPanel::HandleShortcuts(EditorContext& /*context*/) {
  // Text fields and other popups keep their keys.
  if (ImGui::GetIO().WantTextInput ||
      ImGui::IsPopupOpen(
          "", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
    return;
  }
  const ImGuiInputFlags global = ImGuiInputFlags_RouteGlobal;
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D, global)) {
    state_->DuplicateSelection();
  }
  if (ImGui::Shortcut(ImGuiKey_Delete, global)) {
    state_->DeleteSelection();
  }
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_G, global)) {
    state_->GroupSelection();
  }
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_G, global)) {
    state_->UngroupSelection();
  }
  if (ImGui::Shortcut(ImGuiKey_PageDown, global)) {
    state_->StepSelection(1);
  }
  if (ImGui::Shortcut(ImGuiKey_PageUp, global)) {
    state_->StepSelection(-1);
  }
  if (ImGui::Shortcut(ImGuiKey_Escape, global)) {
    if (SceneDocument* doc = state_->document()) {
      doc->selection().Clear();
    }
  }
}

void OutlinerPanel::DrawToolbar(EditorContext& context) {
  if (ImGui::Button("Add instance...")) {
    SceneModeState* state = state_.get();
    context.dialogs().Open(
        FileKind::kPrefab, context.asset_root(), [state](std::string path) {
          state->PlaceInstance(path, state->DropPoint(std::nullopt));
        });
  }
  ImGui::SameLine();
  if (ImGui::Button("Group")) {
    state_->GroupSelection();
  }
  ImGui::SetItemTooltip("Ctrl+G");
  ImGui::SameLine();
  if (ImGui::Button("Ungroup")) {
    state_->UngroupSelection();
  }
  ImGui::SetItemTooltip("Ctrl+Shift+G");
  ImGui::SameLine();
  if (ImGui::Button("Duplicate")) {
    state_->DuplicateSelection();
  }
  ImGui::SetItemTooltip("Ctrl+D");
  ImGui::SameLine();
  if (ImGui::Button("Delete")) {
    state_->DeleteSelection();
  }
  ImGui::SetItemTooltip("Del");
}

void OutlinerPanel::Draw(EditorContext& context) {
  SceneDocument* doc = state_->document();
  if (doc == nullptr) {
    ImGui::TextDisabled("No scene open.");
    if (ImGui::Button("New scene")) {
      context.NewSceneDocument();
    }
    return;
  }
  DrawToolbar(context);
  ImGui::Separator();

  // The instance list scrolls; the scene objects stay below it.
  const float footer = ImGui::GetFrameHeightWithSpacing() * 6.0f;
  if (ImGui::BeginChild("instances", ImVec2(0.0f, -footer),
                        ImGuiChildFlags_None)) {
    rows_ = OutlinerRows(doc->state(), collapsed_);
    for (const OutlinerRow& row : rows_) {
      DrawInstanceRow(*doc, row);
    }
    if (rows_.empty()) {
      ImGui::TextDisabled("Drop prefabs here or use Add instance...");
    }
    // The empty space below the rows: drop to the root, click to deselect.
    const ImVec2 space = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##root_drop", ImVec2(std::max(space.x, 1.0f),
                                                 std::max(space.y, 20.0f)));
    if (ImGui::IsItemClicked()) {
      doc->selection().Clear();
    }
    AcceptDrops(*doc, scene::kNoInstance);
  }
  ImGui::EndChild();
  ImGui::Separator();
  DrawSceneObjects(*doc);
}

void OutlinerPanel::DrawInstanceRow(SceneDocument& doc,
                                    const OutlinerRow& row) {
  const scene::Instance* instance = scene::FindInstance(doc.state(), row.id);
  if (instance == nullptr) {
    return;
  }
  ImGui::PushID(static_cast<int>(row.id));

  // Visibility toggle (one undo step).
  bool visible = instance->visible;
  if (ImGui::Checkbox("##visible", &visible)) {
    const scene::InstanceId id = row.id;
    doc.Edit(visible ? "Show" : "Hide", ChangeKind::kStructure,
             [&](scene::SceneData& scene) {
               scene::FindInstance(scene, id)->visible = visible;
             });
  }
  ImGui::SetItemTooltip("Visible");
  ImGui::SameLine();

  ImGui::Indent(static_cast<float>(row.depth) *
                ImGui::GetTreeNodeToLabelSpacing());
  if (row.has_children) {
    const auto collapsed = std::ranges::find(collapsed_, row.id);
    const bool is_collapsed = collapsed != collapsed_.end();
    if (ImGui::ArrowButton("##fold",
                           is_collapsed ? ImGuiDir_Right : ImGuiDir_Down)) {
      if (is_collapsed) {
        collapsed_.erase(collapsed);
      } else {
        collapsed_.push_back(row.id);
      }
    }
    ImGui::SameLine();
  } else {
    ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));
    ImGui::SameLine();
  }

  const SelectionItem item = InstanceItem(row.id);
  const std::string label =
      instance->prefab == scene::kNoPrefab
          ? std::format("[{}]", instance->name)  // a group node
          : instance->name;
  if (!instance->visible) {
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  }
  if (ImGui::Selectable(label.c_str(), doc.selection().Contains(item))) {
    if (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift) {
      doc.selection().Toggle(item);
    } else {
      doc.selection().Set(item);
    }
  }
  if (!instance->visible) {
    ImGui::PopStyleColor();
  }
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
    if (const auto bounds = client_->FocusBounds(state_->context())) {
      state_->context().viewport().FrameBounds(*bounds);
    }
  }
  if (ImGui::BeginDragDropSource()) {
    ImGui::SetDragDropPayload(kInstancePayload, &row.id, sizeof(row.id));
    ImGui::TextUnformatted(instance->name.c_str());
    ImGui::EndDragDropSource();
  }
  AcceptDrops(doc, row.id);

  ImGui::Unindent(static_cast<float>(row.depth) *
                  ImGui::GetTreeNodeToLabelSpacing());
  ImGui::PopID();
}

void OutlinerPanel::AcceptDrops(SceneDocument& doc, scene::InstanceId parent) {
  if (!ImGui::BeginDragDropTarget()) {
    return;
  }
  if (const ImGuiPayload* payload =
          ImGui::AcceptDragDropPayload(kInstancePayload)) {
    scene::InstanceId dragged = scene::kNoInstance;
    std::memcpy(&dragged, payload->Data, sizeof(dragged));
    // Every selected instance moves when the dragged one is selected.
    std::vector<scene::InstanceId> moving = {dragged};
    if (doc.selection().Contains(InstanceItem(dragged))) {
      moving = doc.selection().Instances();
    }
    int moved = 0;
    doc.Edit("Reparent", ChangeKind::kStructure, [&](scene::SceneData& scene) {
      for (const scene::InstanceId id : moving) {
        if (scene::ReparentInstance(scene, id, parent,
                                    /*keep_world_transform=*/true)) {
          ++moved;
        }
      }
    });
    if (moved == 0) {
      state_->context().Status(
          "Cannot move an instance into itself or its children");
    }
  }
  const ImGuiPayload* peek = ImGui::GetDragDropPayload();
  if (peek != nullptr && peek->IsDataType(kAssetPathPayload) &&
      MatchesFileKind(static_cast<const char*>(peek->Data),
                      FileKind::kPrefab)) {
    if (const ImGuiPayload* payload =
            ImGui::AcceptDragDropPayload(kAssetPathPayload)) {
      const std::string path(static_cast<const char*>(payload->Data));
      const glm::vec3 position = state_->DropPoint(std::nullopt);
      scene::InstanceId placed = scene::kNoInstance;
      doc.Edit("Add instance", ChangeKind::kStructure,
               [&](scene::SceneData& scene) {
                 placed = PlacePrefab(scene, path, position, parent);
               });
      if (placed != scene::kNoInstance) {
        doc.selection().Set(InstanceItem(placed));
      }
    }
  }
  ImGui::EndDragDropTarget();
}

void OutlinerPanel::DrawSceneObjects(SceneDocument& doc) {
  const scene::SceneData& scene = doc.state();
  Selection& selection = doc.selection();
  const auto selectable = [&](const char* label, SelectionItem item) {
    if (ImGui::Selectable(label, selection.Contains(item))) {
      selection.Set(item);
    }
  };
  selectable("Sun", {SelectionKind::kSun, -1});
  selectable("Environment", {SelectionKind::kEnvironment, -1});
  selectable("Player start", {SelectionKind::kPlayerStart, -1});
  for (size_t i = 0; i < scene.point_lights.size(); ++i) {
    const std::string label = std::format("Light: {}##light{}",
                                          scene.point_lights[i].name.empty()
                                              ? "(unnamed)"
                                              : scene.point_lights[i].name,
                                          i);
    selectable(label.c_str(),
               {SelectionKind::kScenePointLight, static_cast<int64_t>(i)});
  }
  if (ImGui::SmallButton("Add point light")) {
    state_->AddPointLight(state_->DropPoint(std::nullopt) +
                          glm::vec3(0.0f, 2.0f, 0.0f));
  }
}

}  // namespace

std::unique_ptr<Panel> MakeOutlinerPanel(
    std::unique_ptr<SceneModeState> state,
    std::unique_ptr<ViewportClient> client) {
  return std::make_unique<OutlinerPanel>(std::move(state), std::move(client));
}

}  // namespace blue::editor
