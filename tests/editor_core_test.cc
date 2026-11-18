#include <doctest/doctest.h>

#include <ostream>
#include <string>
#include <vector>

#include "core/result.h"
#include "editor/commands.h"
#include "editor/document.h"
#include "editor/editor_options.h"
#include "editor/selection.h"
#include "editor/undo.h"
#include "scene/scene_data.h"

namespace blue::editor {

namespace {

// A tiny document state for the history tests.
struct Counter {
  int value = 0;
};

}  // namespace

TEST_CASE("A drag of several Begin calls is one undo step") {
  SnapshotHistory<Counter> history;
  Counter state;
  for (int frame = 1; frame <= 5; ++frame) {
    history.Begin(state, "Drag", ChangeKind::kParameters);
    state.value = frame;
  }
  history.Commit();
  CHECK(history.undo_count() == 1);
  CHECK(history.undo_label() == "Drag");

  REQUIRE(history.Undo(state) == ChangeKind::kParameters);
  CHECK(state.value == 0);
  REQUIRE(history.Redo(state).has_value());
  CHECK(state.value == 5);
  CHECK_FALSE(history.Redo(state).has_value());
}

TEST_CASE("A new edit clears the redo steps") {
  SnapshotHistory<Counter> history;
  Counter state;
  history.Begin(state, "One", ChangeKind::kParameters);
  state.value = 1;
  history.Commit();
  REQUIRE(history.Undo(state).has_value());
  CHECK(history.can_redo());
  history.Begin(state, "Two", ChangeKind::kParameters);
  state.value = 2;
  history.Commit();
  CHECK_FALSE(history.can_redo());
}

TEST_CASE("A structural Begin upgrades an open parameter edit") {
  SnapshotHistory<Counter> history;
  Counter state;
  history.Begin(state, "Edit", ChangeKind::kParameters);
  history.Begin(state, "Edit", ChangeKind::kStructure);
  state.value = 3;
  history.Commit();
  CHECK(history.Undo(state) == ChangeKind::kStructure);
}

TEST_CASE("The history drops its oldest steps past its capacity") {
  SnapshotHistory<Counter> history(3);
  Counter state;
  for (int i = 1; i <= 5; ++i) {
    history.Begin(state, "Step", ChangeKind::kParameters);
    state.value = i;
    history.Commit();
  }
  CHECK(history.undo_count() == 3);
  while (history.Undo(state)) {
  }
  CHECK(state.value == 2);
}

TEST_CASE("Cancel returns the state from before the edit") {
  SnapshotHistory<Counter> history;
  Counter state{.value = 7};
  history.Begin(state, "Edit", ChangeKind::kParameters);
  state.value = 9;
  const std::optional<Counter> before = history.Cancel();
  REQUIRE(before.has_value());
  CHECK(before->value == 7);
  CHECK_FALSE(history.can_undo());
}

TEST_CASE("Document edits mark it dirty and advance its revision") {
  SceneDocument document(scene::SceneData{}, "level.mscn");
  CHECK_FALSE(document.dirty());
  const DocumentRevision start = document.revision();

  document.Edit("Gravity", ChangeKind::kParameters,
                [](scene::SceneData& scene) { scene.gravity.y = -5.0f; });
  CHECK(document.dirty());
  CHECK(document.revision().any == start.any + 1);
  CHECK(document.revision().structure == start.structure);

  document.Edit("Add", ChangeKind::kStructure, [](scene::SceneData& scene) {
    scene.instances.push_back({.id = 4, .name = "crate"});
  });
  CHECK(document.revision().structure == start.structure + 1);

  document.MarkSaved();
  CHECK_FALSE(document.dirty());
  REQUIRE(document.Undo());
  CHECK(document.dirty());
  CHECK(document.state().instances.empty());
  CHECK(document.state().gravity.y == -5.0f);
  // Undoing a structural step is a structural change for the GPU side.
  CHECK(document.revision().structure == start.structure + 2);
}

TEST_CASE("CancelEdit restores the document") {
  SceneDocument document(scene::SceneData{});
  const float gravity = document.state().gravity.y;
  document.BeginEdit("Gravity", ChangeKind::kParameters).gravity.y = 1.0f;
  document.CancelEdit();
  CHECK(document.state().gravity.y == gravity);
  CHECK_FALSE(document.history().can_undo());
}

TEST_CASE("Saving an untitled document needs a path") {
  SceneDocument document(scene::SceneData{});
  const core::Result<void, std::string> saved = SaveSceneDocument(document);
  CHECK_FALSE(saved.ok());
  CHECK(DocumentTitle("") == "Untitled");
  CHECK(DocumentTitle("a/b/range.mscn") == "range.mscn");
}

TEST_CASE("Display paths are relative to the asset root when inside it") {
  CHECK(DisplayPath("C:/game/bin/assets/props/crate.pbo",
                    "C:/game/bin/assets") == "props/crate.pbo");
  CHECK(DisplayPath("C:/elsewhere/a.pbo", "C:/game/bin/assets") ==
        "C:/elsewhere/a.pbo");
  CHECK(DisplayPath("D:/a.pbo", "C:/game") == "D:/a.pbo");
  CHECK(DisplayPath("C:/game/a.pbo", "") == "C:/game/a.pbo");
}

TEST_CASE("Selection keeps the last added item as primary") {
  Selection selection;
  CHECK_FALSE(selection.primary().has_value());
  selection.Set(InstanceItem(3));
  selection.Add(InstanceItem(5));
  selection.Add(InstanceItem(3));  // Moves to primary.
  REQUIRE(selection.primary().has_value());
  CHECK(selection.PrimaryIndex(SelectionKind::kInstance) == 3);
  CHECK(selection.Instances() == std::vector<scene::InstanceId>{5, 3});

  selection.Toggle(InstanceItem(5));
  CHECK(selection.Instances() == std::vector<scene::InstanceId>{3});
  CHECK(selection.PrimaryIndex(SelectionKind::kNode) == -1);
}

TEST_CASE("Selection drops items that no longer exist") {
  scene::SceneData scene;
  scene.instances.push_back({.id = 1, .name = "a"});
  Selection selection;
  selection.Add(InstanceItem(1));
  selection.Add(InstanceItem(2));
  selection.Add({.kind = SelectionKind::kScenePointLight, .index = 4});
  selection.PruneInstances(scene);
  selection.PruneIndices(SelectionKind::kScenePointLight, 2);
  CHECK(selection.Instances() == std::vector<scene::InstanceId>{1});
  CHECK(selection.items().size() == 1);
}

TEST_CASE("Selection kind names round trip") {
  for (const SelectionKind kind :
       {SelectionKind::kNode, SelectionKind::kMaterial,
        SelectionKind::kInstance, SelectionKind::kScenePointLight,
        SelectionKind::kPlayerStart}) {
    CHECK(ParseSelectionKind(SelectionKindName(kind)) == kind);
  }
  CHECK_FALSE(ParseSelectionKind("banana").has_value());
}

TEST_CASE("Command lines split at semicolons and keep quoted words") {
  const std::vector<std::vector<std::string>> commands =
      ParseCommandLine("save-as \"my file.pbo\"; select material 0;; wait 3");
  REQUIRE(commands.size() == 3);
  CHECK(commands[0] == std::vector<std::string>{"save-as", "my file.pbo"});
  CHECK(commands[1] == std::vector<std::string>{"select", "material", "0"});
  CHECK(commands[2] == std::vector<std::string>{"wait", "3"});
}

TEST_CASE("Commands run with their arguments and report failures") {
  CommandRegistry registry;
  std::vector<std::string> seen;
  registry.Register(
      "set", "set <name> <value>", [&seen](std::span<const std::string> args) {
        if (args.size() != 2) {
          return core::Result<void, std::string>::Error("expects 2 words");
        }
        seen.push_back(args[0] + "=" + args[1]);
        return core::Result<void, std::string>::Ok();
      });
  CHECK(registry.Run("set roughness 0.3; set metallic 1").ok());
  CHECK(seen == std::vector<std::string>{"roughness=0.3", "metallic=1"});

  const core::Result<void, std::string> bad = registry.Run("set x");
  REQUIRE_FALSE(bad.ok());
  CHECK(bad.error() == "set: expects 2 words");
  CHECK_FALSE(registry.Run("fly away").ok());
}

TEST_CASE("Queued commands pause on wait") {
  CommandRegistry registry;
  int runs = 0;
  registry.Register("tick", "tick", [&runs](std::span<const std::string>) {
    ++runs;
    return core::Result<void, std::string>::Ok();
  });
  registry.Enqueue("tick; wait 2; tick");
  CHECK(registry.RunQueued().empty());
  CHECK(runs == 1);
  (void)registry.RunQueued();
  (void)registry.RunQueued();
  CHECK(runs == 1);
  (void)registry.RunQueued();
  CHECK(runs == 2);
  CHECK(registry.idle());

  registry.Enqueue("nope; tick");
  const std::vector<std::string> errors = registry.RunQueued();
  CHECK(errors.size() == 1);
  CHECK(runs == 3);
}

TEST_CASE("Number arguments are parsed strictly") {
  CHECK(ParseFloatArg("0.25", "value").value() == 0.25f);
  CHECK_FALSE(ParseFloatArg("0.25x", "value").ok());
  CHECK(ParseIntArg("-3", "index").value() == -3);
  CHECK_FALSE(ParseIntArg("1.5", "index").ok());
}

TEST_CASE("Editor options parse the documented command line") {
  const core::Result<EditorOptions, std::string> options = ParseEditorOptions(
      {"assets/range.mscn", "--window-pos", "-1920,40", "--window-size",
       "1280,720", "--exec", "select next; save"});
  REQUIRE(options.ok());
  CHECK(options.value().file == "assets/range.mscn");
  REQUIRE(options.value().window_pos.has_value());
  CHECK(options.value().window_pos->x == -1920);
  CHECK(options.value().window_size.y == 720);
  CHECK(options.value().exec == "select next; save");

  CHECK_FALSE(ParseEditorOptions({"--window-pos"}).ok());
  CHECK_FALSE(ParseEditorOptions({"--window-size", "0,10"}).ok());
  CHECK_FALSE(ParseEditorOptions({"--bogus"}).ok());
  CHECK_FALSE(ParseEditorOptions({"a.pbo", "b.pbo"}).ok());
}

}  // namespace blue::editor
