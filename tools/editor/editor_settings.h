#ifndef BLUE_EDITOR_EDITOR_SETTINGS_H_
#define BLUE_EDITOR_EDITOR_SETTINGS_H_

#include <string>
#include <string_view>

namespace blue::editor {

// Ports the MCP server may listen on (below 1024 needs privileges).
inline constexpr int kMinMcpPort = 1024;
inline constexpr int kMaxMcpPort = 65535;
inline constexpr int kDefaultMcpPort = 8765;

// The MCP server of the editor (Edit > Settings... > MCP server). Off by
// default: opening a local port is opt-in.
struct McpSettings {
  bool enabled = false;
  int port = kDefaultMcpPort;  // kMinMcpPort..kMaxMcpPort

  friend bool operator==(const McpSettings&, const McpSettings&) = default;
};

// Returns the endpoint an MCP client connects to:
// "http://127.0.0.1:<port>/mcp".
[[nodiscard]] std::string McpUrl(int port);
// Returns the command that adds the editor's server to Claude Code.
[[nodiscard]] std::string McpClaudeCommand(int port);

// Bulk data (textures, meshes) the prefab document's undo history may keep
// alive on top of the document, in MiB (see TrimHistoryBulk()).
inline constexpr int kMinHistoryBudgetMb = 16;
inline constexpr int kMaxHistoryBudgetMb = 65536;
inline constexpr int kDefaultHistoryBudgetMb = 512;

// User preferences of the editor, persisted in a small "key = value" text
// file in the pref dir (next to the layout ini). Add new settings here as
// fields with their defaults and a key in editor_settings.cc. Copyable.
struct EditorSettings {
  McpSettings mcp;  // keys "mcp.enabled", "mcp.port"
  // key "history.budget_mb", kMinHistoryBudgetMb..kMaxHistoryBudgetMb
  int history_budget_mb = kDefaultHistoryBudgetMb;

  friend bool operator==(const EditorSettings&,
                         const EditorSettings&) = default;
};

// Parses settings text: one "key = value" per line, '#' starts a comment.
// Unknown keys, malformed lines and bad values are logged and skipped (the
// key keeps its default); numbers are clamped to their documented ranges.
[[nodiscard]] EditorSettings ParseEditorSettings(std::string_view text);

// Returns `settings` as text that ParseEditorSettings() reads back.
[[nodiscard]] std::string FormatEditorSettings(const EditorSettings& settings);

// Loads the settings file at `path`; a missing or unreadable file gives the
// defaults.
[[nodiscard]] EditorSettings LoadEditorSettings(const std::string& path);

// Writes `settings` to `path` atomically. Returns false (and logs) on
// failure.
[[nodiscard]] bool SaveEditorSettings(const EditorSettings& settings,
                                      const std::string& path);

}  // namespace blue::editor

#endif  // BLUE_EDITOR_EDITOR_SETTINGS_H_
