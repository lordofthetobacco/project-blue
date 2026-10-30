#ifndef BLUE_INPUT_BINDINGS_H_
#define BLUE_INPUT_BINDINGS_H_

#include <SDL3/SDL_scancode.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "input/action.h"

namespace blue::input {

enum class BindingKind : uint8_t {
  kKey,          // `code` is an SDL_Scancode (physical key, layout agnostic).
  kMouseButton,  // `code` is SDL_BUTTON_LEFT..SDL_BUTTON_X2 (1..5).
};

inline constexpr int kMinMouseButton = 1;
inline constexpr int kMaxMouseButton = 5;

// One physical input that can trigger an action.
struct Binding {
  BindingKind kind = BindingKind::kKey;
  int code = SDL_SCANCODE_UNKNOWN;

  [[nodiscard]] static Binding Key(SDL_Scancode scancode) {
    return {BindingKind::kKey, scancode};
  }
  [[nodiscard]] static Binding MouseButton(int button) {
    return {BindingKind::kMouseButton, button};
  }

  friend bool operator==(const Binding&, const Binding&) = default;
};

// Returns the config spelling of `binding`: SDL's key name ("Space",
// "Left Shift", "[") or "Mouse1".."Mouse5". Empty for an unknown key.
[[nodiscard]] std::string FormatBinding(const Binding& binding);

// Parses what FormatBinding produces, ignoring case and surrounding
// whitespace. Returns nullopt for names SDL does not know and for mouse
// buttons outside 1..5.
[[nodiscard]] std::optional<Binding> ParseBinding(std::string_view text);

// Joins the bindings with ", ".
[[nodiscard]] std::string FormatBindingList(std::span<const Binding> bindings);

// Splits a comma-separated list and parses each entry. An empty or blank
// string is an empty (unbound) list; any bad entry makes the whole list
// nullopt.
[[nodiscard]] std::optional<std::vector<Binding>> ParseBindingList(
    std::string_view text);

// Which physical inputs trigger each action. Default-constructed instances
// carry the built-in bindings. Copyable.
class InputBindings {
 public:
  InputBindings();

  [[nodiscard]] static InputBindings Defaults() { return InputBindings(); }

  [[nodiscard]] std::span<const Binding> For(Action action) const {
    return bindings_[static_cast<size_t>(action)];
  }
  // Replaces the bindings of `action`; an empty list unbinds it.
  void Set(Action action, std::vector<Binding> bindings) {
    bindings_[static_cast<size_t>(action)] = std::move(bindings);
  }
  [[nodiscard]] bool IsBound(Action action, const Binding& binding) const;

  friend bool operator==(const InputBindings&, const InputBindings&) = default;

 private:
  std::array<std::vector<Binding>, kActionCount> bindings_;
};

}  // namespace blue::input

#endif  // BLUE_INPUT_BINDINGS_H_
