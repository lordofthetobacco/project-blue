#include <imgui.h>

#include <cstddef>
#include <format>
#include <optional>
#include <string>

#include "core/path.h"
#include "editor/document.h"
#include "editor/editor_context.h"
#include "editor/prefab_mode/prefab_edit.h"
#include "editor/prefab_mode/prefab_mode_state.h"
#include "editor/prefab_mode/prefab_panels.h"
#include "editor/prefab_mode/prefab_widgets.h"
#include "editor/selection.h"
#include "prefab/prefab.h"
#include "world/prefab_cache.h"

namespace blue::editor {

namespace {

[[nodiscard]] double Kib(size_t bytes) {
  return static_cast<double>(bytes) / 1024.0;
}

}  // namespace

void RequestReimport(EditorContext& context, PrefabModeShared& shared) {
  const PrefabDocument* document = context.prefab_document();
  if (document == nullptr) {
    return;
  }
  const prefab::Prefab& prefab = document->state();
  if (prefab.edit.source_model.empty()) {
    context.ShowError("Re-import",
                      "This prefab records no source model to re-import.");
    return;
  }
  ImportDialog& dialog = shared.import_dialog;
  RequestImport(shared, prefab.edit.source_model);
  dialog.target = ImportTarget::kReimport;
  dialog.options = StoredImportOptions(prefab);
  std::string nodes;
  for (const std::string& node : dialog.options.drop_nodes) {
    nodes += nodes.empty() ? node : ", " + node;
  }
  dialog.drop_nodes = nodes;
}

void PrefabPhysicsPanel::Draw(EditorContext& context) {
  PrefabDocument* document = context.prefab_document();
  if (document == nullptr) {
    ImGui::TextDisabled("No prefab open.");
    return;
  }
  DrawPhysicsEditor(context, *document, shared_);
  ImGui::TextDisabled("Viewport toolbar > colliders shows the wireframe.");
}

void PrefabInfoPanel::Draw(EditorContext& context) {
  PrefabDocument* document = context.prefab_document();
  if (document == nullptr) {
    ImGui::TextDisabled("No prefab open.");
    return;
  }
  const prefab::Prefab& prefab = document->state();
  ImGui::Text("%s%s", DocumentTitle(document->path()).c_str(),
              document->dirty() ? " (unsaved)" : "");
  if (!document->path().empty()) {
    ImGui::TextDisabled("%s", document->path().c_str());
  }
  const std::optional<std::string> problem = CheckBeforeSave(prefab);
  if (problem) {
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 120, 90, 255));
    ImGui::TextWrapped("Cannot save: %s", problem->c_str());
    ImGui::PopStyleColor();
  } else {
    ImGui::TextDisabled("Valid: ready to save.");
  }

  const PrefabSummary summary = Summarize(prefab);
  if (ImGui::BeginTable("##summary", 2, ImGuiTableFlags_RowBg)) {
    const auto row = [](const char* label, const std::string& value) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(label);
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(value.c_str());
    };
    row("Nodes", std::to_string(summary.node_count));
    row("Meshes / submeshes",
        std::format("{} / {}", summary.mesh_count, summary.submesh_count));
    row("Vertices / triangles",
        std::format("{} / {}", summary.vertex_count, summary.triangle_count));
    row("Materials", std::to_string(summary.material_count));
    row("Textures", std::to_string(summary.texture_count));
    row("Clips", std::to_string(summary.clip_count));
    row("Sockets / lights / children",
        std::format("{} / {} / {}", summary.socket_count, summary.light_count,
                    summary.child_count));
    row("Vertex data", std::format("{:.1f} KiB", Kib(summary.vertex_bytes)));
    row("Index data", std::format("{:.1f} KiB", Kib(summary.index_bytes)));
    row("Texture data", std::format("{:.1f} KiB", Kib(summary.texture_bytes)));
    row("Collision data",
        std::format("{:.1f} KiB", Kib(summary.collision_bytes)));
    row("Memory held (incl. undo)",
        std::format("{:.1f} KiB", Kib(summary.storage_bytes)));
    row("GPU memory (all cached prefabs)",
        std::format("{:.1f} KiB", Kib(context.prefab_cache().gpu_bytes())));
    const glm::vec3 size = prefab.bounds.size();
    row("Bounds",
        std::format("{:.3f} x {:.3f} x {:.3f} m", size.x, size.y, size.z));
    ImGui::EndTable();
  }

  ImGui::SeparatorText("Source");
  if (prefab.edit.source_model.empty()) {
    ImGui::TextDisabled("No source model recorded.");
  } else {
    ImGui::TextWrapped("%s", prefab.edit.source_model.c_str());
    if (!prefab.edit.import_settings.empty() &&
        ImGui::TreeNode("Import settings")) {
      ImGui::TextUnformatted(prefab.edit.import_settings.c_str());
      ImGui::TreePop();
    }
    ImGui::BeginDisabled(shared_.cook.busy());
    if (ImGui::Button("Re-import...")) {
      RequestReimport(context, shared_);
    }
    ImGui::EndDisabled();
  }
}

}  // namespace blue::editor
