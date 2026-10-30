#include "input/action.h"

#include <doctest/doctest.h>

#include <set>
#include <string>
#include <string_view>

namespace blue::input {

TEST_CASE("Every action has a unique, non-empty config name") {
  std::set<std::string> names;
  for (const Action action : AllActions()) {
    const std::string_view name = ActionName(action);
    CHECK_FALSE(name.empty());
    CHECK(names.insert(std::string(name)).second);
  }
  CHECK(names.size() == kActionCount);
}

TEST_CASE("Action names round-trip through ParseAction") {
  for (const Action action : AllActions()) {
    CHECK(ParseAction(ActionName(action)) == action);
  }
  CHECK(ParseAction("jump") == Action::kJump);
  CHECK(ParseAction("toggle_shadows") == Action::kToggleShadows);
}

TEST_CASE("Unknown and differently cased names are rejected") {
  CHECK_FALSE(ParseAction("").has_value());
  CHECK_FALSE(ParseAction("Jump").has_value());
  CHECK_FALSE(ParseAction("fly").has_value());
  CHECK_FALSE(ParseAction("count").has_value());
}

TEST_CASE("Movement and stance actions are held, the rest edge-triggered") {
  CHECK_FALSE(IsEdgeTriggered(Action::kMoveForward));
  CHECK_FALSE(IsEdgeTriggered(Action::kJump));
  CHECK_FALSE(IsEdgeTriggered(Action::kCrouch));
  CHECK(IsEdgeTriggered(Action::kCycleCamera));
  CHECK(IsEdgeTriggered(Action::kEscape));
  CHECK(IsEdgeTriggered(Action::kToggleShadows));
}

}  // namespace blue::input
