#include "ui/weapon_inspect_screen.h"

#include <imgui.h>

#include <algorithm>
#include <format>
#include <string>
#include <string_view>

#include "game/weapon_modding.h"

namespace blue::ui {
namespace {

// Panel width and the indent of one slot depth, in font sizes (so they
// follow the UI scale).
constexpr float kPanelWidthEm = 30.0f;
constexpr float kDepthIndentEm = 1.2f;
// The slot and stash lists share what is left of the panel's height.
constexpr float kSlotListShare = 0.45f;

constexpr ImVec4 kBetterColor{0.45f, 0.9f, 0.45f, 1.0f};
constexpr ImVec4 kWorseColor{1.0f, 0.4f, 0.35f, 1.0f};
constexpr ImVec4 kDifferentColor{0.95f, 0.8f, 0.35f, 1.0f};
constexpr ImVec4 kRequiredColor{1.0f, 0.75f, 0.35f, 1.0f};

void TextView(std::string_view text) {
  ImGui::TextUnformatted(text.data(), text.data() + text.size());
}

void ColoredText(const ImVec4& color, std::string_view text) {
  ImGui::PushStyleColor(ImGuiCol_Text, color);
  TextView(text);
  ImGui::PopStyleColor();
}

const ImVec4* ChangeColor(game::StatChange change) {
  switch (change) {
    case game::StatChange::kBetter:
      return &kBetterColor;
    case game::StatChange::kWorse:
      return &kWorseColor;
    case game::StatChange::kDifferent:
      return &kDifferentColor;
    case game::StatChange::kNone:
      break;
  }
  return nullptr;
}

void DrawStats(const InspectScreenInfo& info) {
  const int columns = info.has_preview ? 3 : 2;
  constexpr ImGuiTableFlags kFlags =
      ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg;
  if (!ImGui::BeginTable("stats", columns, kFlags)) {
    return;
  }
  for (const game::StatLine& line : info.stats) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    TextView(line.label);
    ImGui::TableNextColumn();
    TextView(line.value);
    if (!info.has_preview) {
      continue;
    }
    ImGui::TableNextColumn();
    const ImVec4* color = ChangeColor(line.change);
    if (color == nullptr) {
      ImGui::TextDisabled("%s", line.preview.c_str());
      continue;
    }
    const std::string text =
        line.delta.empty() ? line.preview
                           : std::format("{} ({})", line.preview, line.delta);
    ColoredText(*color, text);
  }
  ImGui::EndTable();
}

void DrawSlots(const InspectScreenInfo& info, float height,
               InspectScreenResult& result) {
  if (!ImGui::BeginChild("slots", {0.0f, height}, ImGuiChildFlags_Borders)) {
    ImGui::EndChild();
    return;
  }
  const float indent = ImGui::GetFontSize() * kDepthIndentEm;
  for (int i = 0; i < static_cast<int>(info.rows.size()); ++i) {
    const game::SlotRow& row = info.rows[static_cast<size_t>(i)];
    const float depth_indent = indent * static_cast<float>(row.depth);
    if (depth_indent > 0.0f) {
      ImGui::Indent(depth_indent);
    }
    ImGui::PushID(i);
    const std::string label =
        std::format("{}{}", row.name, row.required ? " *" : "");
    if (ImGui::Selectable(label.c_str(), i == info.selected_row)) {
      result.select_row = i;
    }
    ImGui::SameLine(ImGui::GetFontSize() * 9.0f);
    if (row.installed.valid()) {
      TextView(row.installed_name);
    } else if (row.required) {
      ColoredText(kWorseColor, "- empty -");
    } else {
      ImGui::TextDisabled("- empty -");
    }
    ImGui::PopID();
    if (depth_indent > 0.0f) {
      ImGui::Unindent(depth_indent);
    }
  }
  if (info.rows.empty()) {
    ImGui::TextDisabled("This weapon has no slots");
  }
  ImGui::EndChild();
  ColoredText(kRequiredColor, "* required");
  if (info.can_remove) {
    ImGui::SameLine();
    result.remove = ImGui::SmallButton("Remove");
    result.remove_hovered = ImGui::IsItemHovered();
  }
}

void DrawStash(const InspectScreenInfo& info, InspectScreenResult& result) {
  ImGui::TextUnformatted("Stash");
  // Leaves one line under the list for the screen's hint.
  const float height = std::max(
      ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing(),
      ImGui::GetFrameHeight());
  if (!ImGui::BeginChild("stash", {0.0f, height}, ImGuiChildFlags_Borders)) {
    ImGui::EndChild();
    return;
  }
  for (const game::StashLine& line : info.stash) {
    ImGui::PushID(line.stash_index);
    // Two lines per entry: the name (clickable) and its key numbers.
    const std::string label = std::format("{}\n", line.name);
    if (ImGui::Selectable(label.c_str(), false,
                          ImGuiSelectableFlags_AllowOverlap)) {
      result.install = line.stash_index;
    }
    if (ImGui::IsItemHovered()) {
      result.hovered_stash = line.stash_index;
    }
    const float line_height = ImGui::GetTextLineHeightWithSpacing();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - line_height);
    ImGui::Indent();
    ImGui::TextDisabled("%.*s  %s", static_cast<int>(line.category.size()),
                        line.category.data(), line.numbers.c_str());
    ImGui::Unindent();
    ImGui::PopID();
  }
  if (info.stash.empty()) {
    ImGui::TextDisabled(info.selected_row < 0 ? "Select a slot"
                                              : "Nothing in the stash fits");
  }
  ImGui::EndChild();
}

}  // namespace

InspectScreenResult DrawInspectScreen(const InspectScreenInfo& info) {
  InspectScreenResult result;
  const ImGuiStyle& style = ImGui::GetStyle();
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  const float width = std::min(ImGui::GetFontSize() * kPanelWidthEm,
                               display.x - 2.0f * style.WindowPadding.x);
  ImGui::SetNextWindowPos(
      {display.x - style.WindowPadding.x, style.WindowPadding.y},
      ImGuiCond_Always, {1.0f, 0.0f});
  ImGui::SetNextWindowSize({width, display.y - 2.0f * style.WindowPadding.y},
                           ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.8f);
  constexpr ImGuiWindowFlags kFlags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing;
  if (ImGui::Begin("Weapon", nullptr, kFlags)) {
    TextView(info.weapon_name);
    if (!info.missing.empty()) {
      ColoredText(kWorseColor, info.missing);
    }
    if (!info.message.empty()) {
      ImGui::PushTextWrapPos(0.0f);
      ColoredText(kDifferentColor, info.message);
      ImGui::PopTextWrapPos();
    }
    ImGui::Separator();
    DrawStats(info);
    ImGui::Separator();
    if (info.no_parts) {
      ImGui::TextDisabled("This weapon is not built from parts");
    } else {
      const float lists = ImGui::GetContentRegionAvail().y -
                          3.0f * ImGui::GetFrameHeightWithSpacing();
      DrawSlots(info, std::max(lists * kSlotListShare, 0.0f), result);
      DrawStash(info, result);
    }
    ImGui::TextDisabled("Drag to turn the weapon  -  T / Esc closes");
  }
  ImGui::End();
  return result;
}

}  // namespace blue::ui
