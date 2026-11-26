#include "ui/main_menu.h"

#include <imgui.h>

#include <span>
#include <string_view>

#include "ui/main_menu_rules.h"

namespace blue::ui {
namespace {

// Window and button widths in font sizes (so they follow the UI scale).
constexpr float kMenuWidthEm = 24.0f;
constexpr float kPauseWidthEm = 14.0f;
constexpr ImVec4 kErrorColor{1.0f, 0.45f, 0.4f, 1.0f};
constexpr ImGuiWindowFlags kWindowFlags =
    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
    ImGuiWindowFlags_AlwaysAutoResize;

void TextView(std::string_view text) {
  ImGui::TextUnformatted(text.data(), text.data() + text.size());
}

// Starts a window of `width_em` font sizes centred on the display.
bool BeginCentred(const char* title, float width_em) {
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(width_em * ImGui::GetFontSize(), 0.0f));
  return ImGui::Begin(title, nullptr, kWindowFlags);
}

// A text field over `buffer`; returns true when Enter was pressed in it.
bool Field(const char* label, std::span<char> buffer) {
  if (buffer.empty()) {
    return false;
  }
  return ImGui::InputText(label, buffer.data(), buffer.size(),
                          ImGuiInputTextFlags_EnterReturnsTrue);
}

bool FullWidthButton(const char* label) {
  return ImGui::Button(label, ImVec2(-1.0f, 0.0f));
}

}  // namespace

MainMenuResult DrawMainMenu(const MainMenuInfo& info) {
  MainMenuResult result;
  if (!BeginCentred("Project Blue", kMenuWidthEm)) {
    ImGui::End();
    return result;
  }
  const bool connecting = info.state == GameState::kConnecting;
  ImGui::BeginDisabled(connecting);
  ImGui::PushItemWidth(-8.0f * ImGui::GetFontSize());
  const bool enter_address = Field("Server address", info.address);
  const bool enter_name = Field("Player name", info.player_name);
  ImGui::PopItemWidth();
  ImGui::Spacing();
  result.connect = FullWidthButton("Connect") || enter_address || enter_name;
  result.play_offline = FullWidthButton("Play offline");
  ImGui::EndDisabled();
  if (connecting) {
    result.cancel = FullWidthButton("Cancel");
    result.connect = false;
  }
  if (info.state == GameState::kDisconnected) {
    result.back = FullWidthButton("Back");
  }
  result.quit = FullWidthButton("Quit");
  if (!info.status.empty()) {
    ImGui::Separator();
    ImGui::PushTextWrapPos(0.0f);
    if (info.status_is_error) {
      ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
    }
    TextView(info.status);
    if (info.status_is_error) {
      ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
  }
  ImGui::End();
  return result;
}

PauseMenuResult DrawPauseMenu(const PauseMenuInfo& info) {
  PauseMenuResult result;
  if (!BeginCentred("Paused", kPauseWidthEm)) {
    ImGui::End();
    return result;
  }
  if (!info.server.empty()) {
    ImGui::TextDisabled("Online: %.*s", static_cast<int>(info.server.size()),
                        info.server.data());
  }
  ImGui::TextDisabled("The game keeps running.");
  result.resume = FullWidthButton("Resume");
  result.disconnect = FullWidthButton("Disconnect");
  result.quit = FullWidthButton("Quit");
  ImGui::End();
  return result;
}

}  // namespace blue::ui
