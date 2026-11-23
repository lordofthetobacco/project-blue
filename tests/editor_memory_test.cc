#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "editor/document.h"
#include "editor/editor_settings.h"
#include "editor/memory_report.h"
#include "editor/undo.h"
#include "mcp/json.h"
#include "prefab/prefab.h"
#include "prefab/prefab_file.h"
#include "tests/prefab_samples.h"
#include "tests/scratch_directory.h"

namespace blue::editor {
namespace {

constexpr uint64_t kMib = 1024 * 1024;

// Replaces texture 0 of `document` by a new `size` x `size` RGBA8 texture of
// fresh heap blocks, as an undoable structural edit that compacts storage
// (like TryEdit).
void ReplaceTexture(PrefabDocument& document, int size, uint32_t seed) {
  prefab::Prefab copy = document.state();
  copy.textures[0] = testing::MakePatternTexture(
      copy, "big", prefab::TextureFormat::kRgba8Srgb, size, size, seed);
  prefab::CompactStorage(copy);
  document.Edit("Replace texture", ChangeKind::kStructure,
                [&](prefab::Prefab& state) { state = std::move(copy); });
}

uint64_t TextureBytes(int size) {
  // Full RGBA8 mip chain, about 4/3 of the base level.
  uint64_t bytes = 0;
  for (int s = size; s >= 1; s /= 2) {
    bytes += prefab::MipByteSize(prefab::TextureFormat::kRgba8Srgb, s, s);
  }
  return bytes;
}

}  // namespace

TEST_CASE("HistoryOnlyBulk counts blocks only undo steps keep, each once") {
  PrefabDocument document(testing::MakeSamplePrefab());
  const BulkBytes start = DocumentBulk(document);
  CHECK(start.heap > 0);
  CHECK(start.mapped == 0);
  CHECK(HistoryOnlyBulk(document) == BulkBytes{});

  // A parameter edit shares every block with the current state.
  document.Edit("Rename", ChangeKind::kParameters,
                [](prefab::Prefab& p) { p.materials[0].name = "x"; });
  CHECK(HistoryOnlyBulk(document) == BulkBytes{});

  // Replacing the texture: the old one lives on only in the undo step.
  uint64_t old_texture = 0;
  for (const prefab::TextureMip& mip : document.state().textures[0].mips) {
    old_texture += mip.data.size();
  }
  ReplaceTexture(document, 64, 1);
  const uint64_t first = HistoryOnlyBulk(document).heap;
  CHECK(first == old_texture);
  ReplaceTexture(document, 64, 2);
  CHECK(HistoryOnlyBulk(document).heap == first + TextureBytes(64));

  // Undo moves the newest texture into the redo step: still counted once.
  REQUIRE(document.Undo());
  CHECK(HistoryOnlyBulk(document).heap == first + TextureBytes(64));
}

TEST_CASE("TrimHistoryBulk drops the oldest steps beyond the budget") {
  PrefabDocument document(testing::MakeSamplePrefab());
  for (uint32_t i = 0; i < 6; ++i) {
    ReplaceTexture(document, 256, i + 10);  // ~341 KiB each
  }
  REQUIRE(document.history().undo_count() == 6);
  const uint64_t held = HistoryOnlyBulk(document).heap;
  CHECK(held > 5 * TextureBytes(256));

  // A generous budget keeps everything.
  CHECK(TrimHistoryBulk(document, 100 * kMib) == 0);
  CHECK(document.history().undo_count() == 6);

  // Two textures' worth: the oldest steps go until it fits.
  const int dropped = TrimHistoryBulk(document, 2 * TextureBytes(256));
  CHECK(dropped >= 3);
  CHECK(HistoryOnlyBulk(document).heap <= 2 * TextureBytes(256));
  CHECK(document.history().undo_count() == 6 - static_cast<size_t>(dropped));
  // What is left still undoes to valid states.
  while (document.Undo()) {
    CHECK_FALSE(prefab::ValidatePrefab(document.state()).has_value());
  }

  // A budget of 0 drops every step holding unique blocks.
  ReplaceTexture(document, 256, 99);
  TrimHistoryBulk(document, 0);
  CHECK(HistoryOnlyBulk(document).heap == 0);
}

TEST_CASE("Saving a prefab document re-points its bulk data at the file") {
  const testing::ScratchDirectory scratch("doc_rebase");
  prefab::Prefab sample = testing::MakeSamplePrefab();
  sample.children.clear();
  sample.edit = {};
  PrefabDocument document(std::move(sample));
  ReplaceTexture(document, 64, 5);
  REQUIRE(DocumentBulk(document).heap > 0);
  const BulkBytes history_before = HistoryOnlyBulk(document);

  const std::string path = scratch.Child("saved.pbo");
  REQUIRE(SavePrefabDocument(document, path).ok());
  CHECK_FALSE(document.dirty());
  CHECK(DocumentBulk(document).heap == 0);
  CHECK(DocumentBulk(document).mapped > 0);
  // The heap blocks now live only in the undo step, which still works.
  CHECK(HistoryOnlyBulk(document).heap > history_before.heap);
  const auto reloaded = prefab::LoadPrefab(path);
  REQUIRE(reloaded.ok());
  CHECK(document.state().textures[0].mips[0].data.size() ==
        reloaded.value().textures[0].mips[0].data.size());
  CHECK(std::equal(document.state().textures[0].mips[0].data.begin(),
                   document.state().textures[0].mips[0].data.end(),
                   reloaded.value().textures[0].mips[0].data.begin()));
  REQUIRE(document.Undo());
  CHECK_FALSE(prefab::ValidatePrefab(document.state()).has_value());

  // Saving again over the mapped file works and keeps it mapped.
  REQUIRE(document.Redo());
  document.Edit("Rename", ChangeKind::kParameters,
                [](prefab::Prefab& p) { p.materials[0].name = "again"; });
  REQUIRE(SavePrefabDocument(document).ok());
  CHECK(DocumentBulk(document).heap == 0);
  CHECK(prefab::LoadPrefab(path).value().materials[0].name == "again");
}

TEST_CASE("RebaseBulk refuses prefabs whose bulk arrays differ") {
  prefab::Prefab a = testing::MakeSamplePrefab();
  prefab::Prefab b = testing::MakeSamplePrefab();
  CHECK(prefab::RebaseBulk(a, b));
  CHECK(a.textures[0].mips[0].data.data() == b.textures[0].mips[0].data.data());
  CHECK(a.storage.block_count() == b.storage.block_count());

  prefab::Prefab c = testing::MakeSamplePrefab();
  c.textures.pop_back();
  const auto* before = a.meshes[0].vertices.data();
  CHECK_FALSE(prefab::RebaseBulk(a, c));
  CHECK(a.meshes[0].vertices.data() == before);
}

TEST_CASE("The history budget setting parses, clamps and round-trips") {
  CHECK(ParseEditorSettings("").history_budget_mb == kDefaultHistoryBudgetMb);
  CHECK(ParseEditorSettings("history.budget_mb = 1024").history_budget_mb ==
        1024);
  CHECK(ParseEditorSettings("history.budget_mb = 1").history_budget_mb ==
        kMinHistoryBudgetMb);
  CHECK(ParseEditorSettings("history.budget_mb = x").history_budget_mb ==
        kDefaultHistoryBudgetMb);
  EditorSettings settings;
  settings.history_budget_mb = 2048;
  CHECK(ParseEditorSettings(FormatEditorSettings(settings)) == settings);
}

TEST_CASE("The memory report formats bytes and lists every section") {
  CHECK(FormatBytes(512) == "512 B");
  CHECK(FormatBytes(2048) == "2.0 KB");
  CHECK(FormatBytes(3 * kMib / 2) == "1.5 MB");
  CHECK(FormatBytes(3 * 1024 * kMib) == "3.00 GB");

  MemoryReport report;
  report.process = {
      .available = true, .private_bytes = 100 * kMib, .working_set = 50 * kMib};
  report.has_prefab = true;
  report.document = {.heap = kMib, .mapped = 293 * kMib};
  report.gpu.heap_count = 1;
  report.gpu.heaps[0] = {.device_local = true, .usage = 300 * kMib};
  CHECK(DeviceLocalUsage(report.gpu) == 300 * kMib);
  const std::vector<std::string> lines = MemoryReportLines(report);
  REQUIRE(lines.size() >= 5);
  CHECK(lines[0].starts_with("Process: 100.0 MB private"));
  CHECK(lines[1] == "Prefab bulk: 1.0 MB heap, 293.0 MB mapped from file");
  const mcp::Json json = MemoryReportJson(report);
  CHECK(json.is_object());
}

}  // namespace blue::editor
