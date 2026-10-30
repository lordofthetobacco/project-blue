#include "input/bindings.h"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace blue::input {
namespace {

constexpr std::string_view kMousePrefix = "mouse";

std::string_view Trim(std::string_view text) {
  const auto is_space = [](char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
  };
  while (!text.empty() && is_space(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && is_space(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

std::string ToLower(std::string_view text) {
  std::string lower(text);
  std::ranges::transform(lower, lower.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return lower;
}

// Parses "Mouse<n>" (any case). Returns nullopt if `lower` is not of that
// form; the button range is checked by the caller.
std::optional<int> ParseMouseButton(const std::string& lower) {
  if (!lower.starts_with(kMousePrefix)) {
    return std::nullopt;
  }
  const std::string_view digits =
      std::string_view(lower).substr(kMousePrefix.size());
  int button = 0;
  const auto [end, ec] =
      std::from_chars(digits.data(), digits.data() + digits.size(), button);
  if (ec != std::errc{} || end != digits.data() + digits.size()) {
    return std::nullopt;
  }
  return button;
}

}  // namespace

std::string FormatBinding(const Binding& binding) {
  if (binding.kind == BindingKind::kMouseButton) {
    return "Mouse" + std::to_string(binding.code);
  }
  return SDL_GetScancodeName(static_cast<SDL_Scancode>(binding.code));
}

std::optional<Binding> ParseBinding(std::string_view text) {
  const std::string_view trimmed = Trim(text);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  const std::string lower = ToLower(trimmed);
  if (const std::optional<int> button = ParseMouseButton(lower)) {
    if (*button < kMinMouseButton || *button > kMaxMouseButton) {
      return std::nullopt;
    }
    return Binding::MouseButton(*button);
  }
  // SDL compares key names case-insensitively and needs a terminated string.
  const SDL_Scancode scancode =
      SDL_GetScancodeFromName(std::string(trimmed).c_str());
  if (scancode == SDL_SCANCODE_UNKNOWN) {
    return std::nullopt;
  }
  return Binding::Key(scancode);
}

std::string FormatBindingList(std::span<const Binding> bindings) {
  std::string text;
  for (const Binding& binding : bindings) {
    if (!text.empty()) {
      text += ", ";
    }
    text += FormatBinding(binding);
  }
  return text;
}

std::optional<std::vector<Binding>> ParseBindingList(std::string_view text) {
  std::vector<Binding> bindings;
  if (Trim(text).empty()) {
    return bindings;
  }
  size_t start = 0;
  while (start <= text.size()) {
    size_t comma = text.find(',', start);
    if (comma == std::string_view::npos) {
      comma = text.size();
    }
    const std::optional<Binding> binding =
        ParseBinding(text.substr(start, comma - start));
    if (!binding) {
      return std::nullopt;
    }
    bindings.push_back(*binding);
    start = comma + 1;
  }
  return bindings;
}

InputBindings::InputBindings() {
  const auto key = [](SDL_Scancode scancode) {
    return std::vector<Binding>{Binding::Key(scancode)};
  };
  Set(Action::kMoveForward, key(SDL_SCANCODE_W));
  Set(Action::kMoveBack, key(SDL_SCANCODE_S));
  Set(Action::kMoveLeft, key(SDL_SCANCODE_A));
  Set(Action::kMoveRight, key(SDL_SCANCODE_D));
  Set(Action::kFlyUp, key(SDL_SCANCODE_SPACE));
  Set(Action::kFlyDown, key(SDL_SCANCODE_LCTRL));
  Set(Action::kJump, key(SDL_SCANCODE_SPACE));
  Set(Action::kSprint, key(SDL_SCANCODE_LSHIFT));
  Set(Action::kCrouch, key(SDL_SCANCODE_LCTRL));
  Set(Action::kCycleCamera, key(SDL_SCANCODE_TAB));
  Set(Action::kEscape, key(SDL_SCANCODE_ESCAPE));
  Set(Action::kToggleInvertY, key(SDL_SCANCODE_I));
  Set(Action::kSensitivityDown, key(SDL_SCANCODE_LEFTBRACKET));
  Set(Action::kSensitivityUp, key(SDL_SCANCODE_RIGHTBRACKET));
  Set(Action::kToggleOverlay, key(SDL_SCANCODE_F1));
  Set(Action::kSaveScene, key(SDL_SCANCODE_F5));
  Set(Action::kLoadScene, key(SDL_SCANCODE_F9));
  Set(Action::kToggleVsync, key(SDL_SCANCODE_V));
  Set(Action::kPush, key(SDL_SCANCODE_F));
  Set(Action::kToggleCascadeDebug, key(SDL_SCANCODE_C));
  Set(Action::kToggleShadows, key(SDL_SCANCODE_X));
}

bool InputBindings::IsBound(Action action, const Binding& binding) const {
  const std::span<const Binding> list = For(action);
  return std::ranges::find(list, binding) != list.end();
}

}  // namespace blue::input
