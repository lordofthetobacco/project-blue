#ifndef BLUE_EDITOR_MEMORY_REPORT_H_
#define BLUE_EDITOR_MEMORY_REPORT_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "editor/document.h"
#include "mcp/json.h"
#include "platform/process_memory.h"
#include "render/memory_stats.h"

// What the editor's memory readout shows (status bar, Edit > Settings...,
// the Prefab info panel, the MCP tool editor_get_memory and the "memory"
// command), and the byte budget of the prefab document's undo history.

namespace blue::editor {

// Bulk data bytes split by where they live (prefab::BlockKind).
struct BulkBytes {
  uint64_t heap = 0;    // private memory
  uint64_t mapped = 0;  // a mapped .pbo, backed by the file

  friend bool operator==(const BulkBytes&, const BulkBytes&) = default;
};

// Returns the bulk blocks of `document`'s current state.
[[nodiscard]] BulkBytes DocumentBulk(const PrefabDocument& document);

// Returns the bulk blocks that only the undo history (undo and redo steps,
// the open edit) of `document` keeps alive: each block once, without those
// the current state also holds. This is what the history costs on top of the
// document.
[[nodiscard]] BulkBytes HistoryOnlyBulk(const PrefabDocument& document);

// Drops the oldest undo steps of `document` while the heap bytes only its
// history holds exceed `budget_bytes`; returns the number of steps dropped.
// Mapped blocks are not counted (they cost no private memory). The newest
// step is dropped too if it alone exceeds the budget.
int TrimHistoryBulk(PrefabDocument& document, uint64_t budget_bytes);

// Returns `bytes` for display: "512 B", "12.3 KB", "45.6 MB", "1.23 GB".
[[nodiscard]] std::string FormatBytes(uint64_t bytes);

// Everything the readout shows, gathered by EditorContext::memory_report().
struct MemoryReport {
  platform::ProcessMemory process;
  bool has_prefab = false;
  BulkBytes document;  // the open prefab document's current state
  BulkBytes history;   // HistoryOnlyBulk() of it
  uint64_t history_budget = 0;
  int undo_steps = 0;
  int redo_steps = 0;
  // The prefab cache: entries alive and their uploaded mesh/texture bytes.
  int prefab_cache_entries = 0;
  uint64_t prefab_cache_gpu_bytes = 0;
  // Thumbnails and other UI images (EditorContext::CreateImageTexture()).
  int thumbnail_count = 0;
  uint64_t thumbnail_bytes = 0;
  render::MemoryStats gpu;
};

// Returns the device-local heap usage of `gpu` (VRAM), summed over heaps.
[[nodiscard]] uint64_t DeviceLocalUsage(const render::MemoryStats& gpu);

// Returns `report` as JSON (bytes as integers, plus "text": the lines of
// MemoryReportLines()).
[[nodiscard]] mcp::Json MemoryReportJson(const MemoryReport& report);

// Returns `report` as readable lines, e.g. "Process: 1.10 GB private, ...".
[[nodiscard]] std::vector<std::string> MemoryReportLines(
    const MemoryReport& report);

}  // namespace blue::editor

#endif  // BLUE_EDITOR_MEMORY_REPORT_H_
