#include "input/bindings.h"

#include <SDL3/SDL.h>
#include <doctest/doctest.h>

#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "input/action.h"

namespace blue::input {

TEST_CASE("Key bindings format to SDL key names and parse back") {
  for (const SDL_Scancode scancode :
       {SDL_SCANCODE_SPACE, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_LEFTBRACKET,
        SDL_SCANCODE_F1, SDL_SCANCODE_W, SDL_SCANCODE_KP_ENTER}) {
    const Binding binding = Binding::Key(scancode);
    const std::string text = FormatBinding(binding);
    CHECK_FALSE(text.empty());
    CHECK(ParseBinding(text) == binding);
  }
  CHECK(FormatBinding(Binding::Key(SDL_SCANCODE_SPACE)) == "Space");
  CHECK(FormatBinding(Binding::Key(SDL_SCANCODE_LSHIFT)) == "Left Shift");
}

TEST_CASE("Mouse buttons format as Mouse1..Mouse5 and parse back") {
  for (int button = kMinMouseButton; button <= kMaxMouseButton; ++button) {
    const Binding binding = Binding::MouseButton(button);
    CHECK(FormatBinding(binding) == "Mouse" + std::to_string(button));
    CHECK(ParseBinding(FormatBinding(binding)) == binding);
  }
  CHECK(ParseBinding("Mouse1") == Binding::MouseButton(SDL_BUTTON_LEFT));
}

TEST_CASE("Parsing ignores case and surrounding whitespace") {
  CHECK(ParseBinding("space") == Binding::Key(SDL_SCANCODE_SPACE));
  CHECK(ParseBinding("  LEFT SHIFT ") == Binding::Key(SDL_SCANCODE_LSHIFT));
  CHECK(ParseBinding("mouse2") == Binding::MouseButton(2));
  CHECK(ParseBinding("MOUSE5") == Binding::MouseButton(5));
}

TEST_CASE("Unknown names and out-of-range mouse buttons are rejected") {
  for (const char* text :
       {"", "   ", "Hyperspace", "Mouse0", "Mouse6", "Mouse", "Mouse1x"}) {
    CHECK_FALSE(ParseBinding(text).has_value());
  }
}

TEST_CASE("Binding lists round-trip and an empty list is unbound") {
  const std::vector<Binding> list{Binding::Key(SDL_SCANCODE_SPACE),
                                  Binding::MouseButton(5)};
  const std::string text = FormatBindingList(list);
  CHECK(text == "Space, Mouse5");
  CHECK(ParseBindingList(text) == list);
  CHECK(ParseBindingList("Space,Mouse5") == list);

  const std::optional<std::vector<Binding>> empty = ParseBindingList("  ");
  REQUIRE(empty.has_value());
  CHECK(empty->empty());
  CHECK(FormatBindingList({}).empty());
}

TEST_CASE("A list with one bad entry is rejected as a whole") {
  CHECK_FALSE(ParseBindingList("Space, Hyperspace").has_value());
  CHECK_FALSE(ParseBindingList("Space,").has_value());
}

TEST_CASE("Default bindings cover every action with the legacy keys") {
  const InputBindings bindings;
  CHECK(bindings == InputBindings::Defaults());
  for (const Action action : AllActions()) {
    CHECK_FALSE(bindings.For(action).empty());
  }
  CHECK(bindings.IsBound(Action::kMoveForward, Binding::Key(SDL_SCANCODE_W)));
  CHECK(bindings.IsBound(Action::kJump, Binding::Key(SDL_SCANCODE_SPACE)));
  CHECK(bindings.IsBound(Action::kFlyUp, Binding::Key(SDL_SCANCODE_SPACE)));
  CHECK(bindings.IsBound(Action::kCrouch, Binding::Key(SDL_SCANCODE_LCTRL)));
  CHECK(bindings.IsBound(Action::kSprint, Binding::Key(SDL_SCANCODE_LSHIFT)));
  CHECK(bindings.IsBound(Action::kCycleCamera, Binding::Key(SDL_SCANCODE_TAB)));
  CHECK(bindings.IsBound(Action::kEscape, Binding::Key(SDL_SCANCODE_ESCAPE)));
  CHECK(bindings.IsBound(Action::kPush, Binding::Key(SDL_SCANCODE_H)));
  CHECK(bindings.IsBound(Action::kSaveScene, Binding::Key(SDL_SCANCODE_F5)));
  CHECK_FALSE(bindings.IsBound(Action::kPush, Binding::Key(SDL_SCANCODE_G)));
}

TEST_CASE("Weapon actions default to the mouse buttons, R and B") {
  const InputBindings bindings;
  CHECK(bindings.IsBound(Action::kFire, Binding::MouseButton(SDL_BUTTON_LEFT)));
  CHECK(bindings.IsBound(Action::kAim, Binding::MouseButton(SDL_BUTTON_RIGHT)));
  CHECK(bindings.IsBound(Action::kReload, Binding::Key(SDL_SCANCODE_R)));
  CHECK(
      bindings.IsBound(Action::kToggleFireMode, Binding::Key(SDL_SCANCODE_B)));
}

TEST_CASE("Weapon slots default to keys 2 (pistol) and 3 (rifle)") {
  const InputBindings bindings;
  CHECK(bindings.IsBound(Action::kEquipPistol, Binding::Key(SDL_SCANCODE_2)));
  CHECK(bindings.IsBound(Action::kEquipRifle, Binding::Key(SDL_SCANCODE_3)));
  CHECK(bindings.For(Action::kEquipPistol).size() == 1);
  CHECK(bindings.For(Action::kEquipRifle).size() == 1);
}

TEST_CASE("Interact defaults to F and dropping the weapon to G") {
  const InputBindings bindings;
  CHECK(bindings.IsBound(Action::kInteract, Binding::Key(SDL_SCANCODE_F)));
  CHECK(bindings.IsBound(Action::kDropWeapon, Binding::Key(SDL_SCANCODE_G)));
  CHECK(bindings.For(Action::kInteract).size() == 1);
  CHECK(bindings.For(Action::kDropWeapon).size() == 1);
  CHECK(IsEdgeTriggered(Action::kInteract));
  CHECK(IsEdgeTriggered(Action::kDropWeapon));
  CHECK(ParseAction("interact") == Action::kInteract);
  CHECK(ParseAction("drop_weapon") == Action::kDropWeapon);
}

TEST_CASE("The scope's zero defaults to Page Up (further) and Page Down") {
  const InputBindings bindings;
  CHECK(bindings.IsBound(Action::kZeroUp, Binding::Key(SDL_SCANCODE_PAGEUP)));
  CHECK(
      bindings.IsBound(Action::kZeroDown, Binding::Key(SDL_SCANCODE_PAGEDOWN)));
  CHECK(IsEdgeTriggered(Action::kZeroUp));
  CHECK(IsEdgeTriggered(Action::kZeroDown));
  CHECK(ParseAction("zero_up") == Action::kZeroUp);
  CHECK(ParseAction("zero_down") == Action::kZeroDown);
}

TEST_CASE("The inspect screen defaults to T") {
  const InputBindings bindings;
  CHECK(bindings.IsBound(Action::kInspectWeapon, Binding::Key(SDL_SCANCODE_T)));
  CHECK(bindings.For(Action::kInspectWeapon).size() == 1);
  CHECK(IsEdgeTriggered(Action::kInspectWeapon));
  CHECK(ParseAction("inspect_weapon") == Action::kInspectWeapon);
  CHECK(ActionName(Action::kInspectWeapon) == "inspect_weapon");
}

TEST_CASE("No two default actions share an input except the mode pairs") {
  // Space (jump / fly up) and Left Ctrl (crouch / fly down) are deliberately
  // shared between the player and the fly camera; nothing else may be.
  const InputBindings bindings;
  for (const Action a : AllActions()) {
    for (const Action b : AllActions()) {
      if (a >= b) {
        continue;
      }
      const bool allowed_pair = (a == Action::kFlyUp && b == Action::kJump) ||
                                (a == Action::kFlyDown && b == Action::kCrouch);
      for (const Binding& binding : bindings.For(a)) {
        CHECK_MESSAGE((!bindings.IsBound(b, binding) || allowed_pair),
                      ActionName(a), " and ", ActionName(b), " share ",
                      FormatBinding(binding));
      }
    }
  }
}

TEST_CASE("Set replaces the bindings of one action only") {
  InputBindings bindings;
  bindings.Set(Action::kJump, {Binding::MouseButton(5)});
  CHECK(bindings.IsBound(Action::kJump, Binding::MouseButton(5)));
  CHECK_FALSE(
      bindings.IsBound(Action::kJump, Binding::Key(SDL_SCANCODE_SPACE)));
  CHECK(bindings.IsBound(Action::kFlyUp, Binding::Key(SDL_SCANCODE_SPACE)));
  CHECK(bindings != InputBindings::Defaults());

  bindings.Set(Action::kJump, {});
  CHECK(bindings.For(Action::kJump).empty());
}

}  // namespace blue::input
