#include "editor/editor_settings.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include "core/log.h"
#include "platform/file_system.h"

namespace blue::editor {
namespace {

constexpr size_t kMaxSettingsFileSize = 64 * 1024;

// Returns `text` without leading and trailing whitespace.
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

// Returns the integer spelled by all of `text`, or nullopt.
std::optional<int> ParseInt(std::string_view text) {
  int value = 0;
  const auto [end, ec] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

// Returns the boolean spelled by `text` ("true"/"false"/"1"/"0", any case),
// or nullopt for anything else.
std::optional<bool> ParseBool(std::string_view text) {
  std::string lower(text);
  std::ranges::transform(lower, lower.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (lower == "true" || lower == "1") {
    return true;
  }
  if (lower == "false" || lower == "0") {
    return false;
  }
  return std::nullopt;
}

// Applies one non-empty, non-comment line to `settings`.
void ApplyLine(EditorSettings& settings, std::string_view line) {
  const size_t equals = line.find('=');
  if (equals == std::string_view::npos) {
    core::log::Warn("editor settings: malformed line '{}'", line);
    return;
  }
  const std::string_view key = Trim(line.substr(0, equals));
  const std::string_view value = Trim(line.substr(equals + 1));
  if (key == "mcp.enabled") {
    if (const std::optional<bool> parsed = ParseBool(value)) {
      settings.mcp.enabled = *parsed;
      return;
    }
  } else if (key == "mcp.port") {
    if (const std::optional<int> parsed = ParseInt(value)) {
      settings.mcp.port = std::clamp(*parsed, kMinMcpPort, kMaxMcpPort);
      return;
    }
  } else {
    core::log::Warn("editor settings: unknown key '{}'", key);
    return;
  }
  core::log::Warn("editor settings: bad value '{}' for {}", value, key);
}

}  // namespace

std::string McpUrl(int port) {
  return std::format("http://127.0.0.1:{}/mcp", port);
}

std::string McpClaudeCommand(int port) {
  return std::format("claude mcp add --transport http blue-editor {}",
                     McpUrl(port));
}

EditorSettings ParseEditorSettings(std::string_view text) {
  EditorSettings settings;
  size_t line_start = 0;
  while (line_start < text.size()) {
    size_t line_end = text.find('\n', line_start);
    if (line_end == std::string_view::npos) {
      line_end = text.size();
    }
    const std::string_view line =
        Trim(text.substr(line_start, line_end - line_start));
    line_start = line_end + 1;
    if (!line.empty() && line.front() != '#') {
      ApplyLine(settings, line);
    }
  }
  return settings;
}

std::string FormatEditorSettings(const EditorSettings& settings) {
  return std::format(
      "# blue-editor settings\n"
      "\n"
      "# MCP server (Edit > Settings...): http://127.0.0.1:<port>/mcp\n"
      "mcp.enabled = {}\n"
      "mcp.port = {}\n",
      settings.mcp.enabled, settings.mcp.port);
}

EditorSettings LoadEditorSettings(const std::string& path) {
  const auto bytes = platform::ReadFile(path, kMaxSettingsFileSize);
  if (!bytes.ok()) {
    return EditorSettings{};
  }
  const std::string_view text(
      reinterpret_cast<const char*>(bytes.value().data()),
      bytes.value().size());
  return ParseEditorSettings(text);
}

bool SaveEditorSettings(const EditorSettings& settings,
                        const std::string& path) {
  const std::string text = FormatEditorSettings(settings);
  const auto written =
      platform::WriteFileAtomic(path, std::as_bytes(std::span(text)));
  if (!written.ok()) {
    core::log::Warn("cannot write editor settings '{}': {}", path,
                    platform::ToString(written.error()));
    return false;
  }
  return true;
}

}  // namespace blue::editor
