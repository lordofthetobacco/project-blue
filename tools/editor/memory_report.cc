#include "editor/memory_report.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <unordered_set>
#include <vector>

#include "editor/document.h"
#include "mcp/json.h"
#include "prefab/prefab.h"
#include "render/memory_stats.h"

namespace blue::editor {
namespace {

using BlockSet = std::unordered_set<const std::byte*>;

void Add(BulkBytes& bytes, const prefab::BulkBlock& block) {
  (block.kind == prefab::BlockKind::kMapped ? bytes.mapped : bytes.heap) +=
      block.size;
}

int64_t AsInt(uint64_t value) { return static_cast<int64_t>(value); }

}  // namespace

BulkBytes DocumentBulk(const PrefabDocument& document) {
  BulkBytes bytes;
  for (const prefab::BulkBlock& block : document.state().storage.blocks()) {
    Add(bytes, block);
  }
  return bytes;
}

BulkBytes HistoryOnlyBulk(const PrefabDocument& document) {
  BlockSet seen;
  for (const prefab::BulkBlock& block : document.state().storage.blocks()) {
    seen.insert(block.data.get());
  }
  BulkBytes bytes;
  document.history().ForEachState([&](const prefab::Prefab& state) {
    for (const prefab::BulkBlock& block : state.storage.blocks()) {
      if (seen.insert(block.data.get()).second) {
        Add(bytes, block);
      }
    }
  });
  return bytes;
}

int TrimHistoryBulk(PrefabDocument& document, uint64_t budget_bytes) {
  int dropped = 0;
  while (HistoryOnlyBulk(document).heap > budget_bytes &&
         document.DropOldestUndoStep()) {
    ++dropped;
  }
  return dropped;
}

std::string FormatBytes(uint64_t bytes) {
  constexpr double kKilo = 1024.0;
  const auto value = static_cast<double>(bytes);
  if (bytes < 1024) {
    return std::format("{} B", bytes);
  }
  if (value < kKilo * kKilo) {
    return std::format("{:.1f} KB", value / kKilo);
  }
  if (value < kKilo * kKilo * kKilo) {
    return std::format("{:.1f} MB", value / (kKilo * kKilo));
  }
  return std::format("{:.2f} GB", value / (kKilo * kKilo * kKilo));
}

uint64_t DeviceLocalUsage(const render::MemoryStats& gpu) {
  uint64_t usage = 0;
  for (uint32_t i = 0; i < gpu.heap_count && i < gpu.heaps.size(); ++i) {
    if (gpu.heaps[i].device_local) {
      usage += gpu.heaps[i].usage;
    }
  }
  return usage;
}

mcp::Json MemoryReportJson(const MemoryReport& report) {
  mcp::Json process = mcp::Json::Object();
  process.Set("available", report.process.available)
      .Set("private_bytes", AsInt(report.process.private_bytes))
      .Set("working_set", AsInt(report.process.working_set))
      .Set("peak_private_bytes", AsInt(report.process.peak_private_bytes))
      .Set("peak_working_set", AsInt(report.process.peak_working_set));

  mcp::Json prefab = mcp::Json::Object();
  prefab.Set("open", report.has_prefab)
      .Set("bulk_heap_bytes", AsInt(report.document.heap))
      .Set("bulk_mapped_bytes", AsInt(report.document.mapped))
      .Set("history_heap_bytes", AsInt(report.history.heap))
      .Set("history_mapped_bytes", AsInt(report.history.mapped))
      .Set("history_budget_bytes", AsInt(report.history_budget))
      .Set("undo_steps", report.undo_steps)
      .Set("redo_steps", report.redo_steps);

  const render::MemoryStats& stats = report.gpu;
  mcp::Json heaps = mcp::Json::Array();
  for (uint32_t i = 0; i < stats.heap_count && i < stats.heaps.size(); ++i) {
    const render::HeapMemoryStats& heap = stats.heaps[i];
    mcp::Json entry = mcp::Json::Object();
    entry.Set("index", static_cast<int>(i))
        .Set("device_local", heap.device_local)
        .Set("heap_size", AsInt(heap.heap_size))
        .Set("block_bytes", AsInt(heap.block_bytes))
        .Set("allocation_bytes", AsInt(heap.allocation_bytes))
        .Set("block_count", static_cast<int64_t>(heap.block_count))
        .Set("allocation_count", static_cast<int64_t>(heap.allocation_count))
        .Set("usage", AsInt(heap.usage))
        .Set("budget", AsInt(heap.budget));
    heaps.Push(std::move(entry));
  }
  mcp::Json gpu = mcp::Json::Object();
  gpu.Set("driver_budget", stats.driver_budget)
      .Set("heaps", std::move(heaps))
      .Set("texture_bytes", AsInt(stats.texture_bytes))
      .Set("texture_count", static_cast<int64_t>(stats.texture_count))
      .Set("mesh_bytes", AsInt(stats.mesh_bytes))
      .Set("mesh_count", static_cast<int64_t>(stats.mesh_count))
      .Set("render_target_bytes", AsInt(stats.render_target_bytes))
      .Set("environment_bytes", AsInt(stats.environment_bytes))
      .Set("retired_bytes", AsInt(stats.retired_bytes))
      .Set("staging_budget_bytes", AsInt(stats.staging_budget_bytes))
      .Set("staging_peak_bytes", AsInt(stats.staging_peak_bytes))
      .Set("staging_current_bytes", AsInt(stats.staging_current_bytes))
      .Set("prefab_cache_entries", report.prefab_cache_entries)
      .Set("prefab_cache_bytes", AsInt(report.prefab_cache_gpu_bytes))
      .Set("thumbnail_count", report.thumbnail_count)
      .Set("thumbnail_bytes", AsInt(report.thumbnail_bytes));

  mcp::Json text = mcp::Json::Array();
  for (std::string& line : MemoryReportLines(report)) {
    text.Push(mcp::Json::String(std::move(line)));
  }
  mcp::Json json = mcp::Json::Object();
  json.Set("process", std::move(process))
      .Set("prefab_document", std::move(prefab))
      .Set("gpu", std::move(gpu))
      .Set("text", std::move(text));
  return json;
}

std::vector<std::string> MemoryReportLines(const MemoryReport& report) {
  std::vector<std::string> lines;
  if (report.process.available) {
    lines.push_back(
        std::format("Process: {} private (peak {}), {} working set (peak {})",
                    FormatBytes(report.process.private_bytes),
                    FormatBytes(report.process.peak_private_bytes),
                    FormatBytes(report.process.working_set),
                    FormatBytes(report.process.peak_working_set)));
  }
  if (report.has_prefab) {
    lines.push_back(std::format("Prefab bulk: {} heap, {} mapped from file",
                                FormatBytes(report.document.heap),
                                FormatBytes(report.document.mapped)));
    lines.push_back(std::format(
        "Undo history: {} heap (budget {}), {} mapped; {} undo / {} redo "
        "steps",
        FormatBytes(report.history.heap), FormatBytes(report.history_budget),
        FormatBytes(report.history.mapped), report.undo_steps,
        report.redo_steps));
  } else {
    lines.emplace_back("Prefab bulk: no prefab document");
  }
  const render::MemoryStats& gpu = report.gpu;
  lines.push_back(std::format(
      "GPU: {} textures ({}), {} meshes ({}), {} render targets, {} "
      "environment, {} retired",
      FormatBytes(gpu.texture_bytes), gpu.texture_count,
      FormatBytes(gpu.mesh_bytes), gpu.mesh_count,
      FormatBytes(gpu.render_target_bytes), FormatBytes(gpu.environment_bytes),
      FormatBytes(gpu.retired_bytes)));
  for (uint32_t i = 0; i < gpu.heap_count && i < gpu.heaps.size(); ++i) {
    const render::HeapMemoryStats& heap = gpu.heaps[i];
    lines.push_back(std::format(
        "GPU heap {} ({}): {} in {} blocks, {} used by {} allocations, usage "
        "{} of {} budget{}",
        i, heap.device_local ? "device" : "host", FormatBytes(heap.block_bytes),
        heap.block_count, FormatBytes(heap.allocation_bytes),
        heap.allocation_count, FormatBytes(heap.usage),
        FormatBytes(heap.budget), gpu.driver_budget ? "" : " (estimated)"));
  }
  lines.push_back(std::format("Staging: budget {}, peak {}, now {}",
                              FormatBytes(gpu.staging_budget_bytes),
                              FormatBytes(gpu.staging_peak_bytes),
                              FormatBytes(gpu.staging_current_bytes)));
  lines.push_back(std::format(
      "Prefab cache: {} prefabs, {} uploaded; thumbnails: {} ({})",
      report.prefab_cache_entries, FormatBytes(report.prefab_cache_gpu_bytes),
      report.thumbnail_count, FormatBytes(report.thumbnail_bytes)));
  return lines;
}

}  // namespace blue::editor
