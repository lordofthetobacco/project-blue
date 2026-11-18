#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "editor/editor_options.h"
#include "editor/editor_settings.h"

namespace blue::editor {
namespace {

TEST_CASE("Editor settings default to a disabled MCP server on 8765") {
  const EditorSettings settings = ParseEditorSettings("");
  CHECK_FALSE(settings.mcp.enabled);
  CHECK(settings.mcp.port == kDefaultMcpPort);
  CHECK(kDefaultMcpPort == 8765);
}

TEST_CASE("Editor settings round-trip through their text form") {
  EditorSettings settings;
  settings.mcp.enabled = true;
  settings.mcp.port = 9123;
  const std::string text = FormatEditorSettings(settings);
  CHECK(text.find("mcp.enabled = true") != std::string::npos);
  CHECK(ParseEditorSettings(text) == settings);
}

TEST_CASE("Editor settings parse comments, spacing, bad values and ranges") {
  const EditorSettings settings = ParseEditorSettings(
      "# comment\n"
      "  mcp.enabled=1  \r\n"
      "unknown.key = 3\n"
      "no equals sign\n"
      "mcp.port = 80\n");
  CHECK(settings.mcp.enabled);
  CHECK(settings.mcp.port == kMinMcpPort);  // Clamped.

  CHECK(ParseEditorSettings("mcp.port = 70000").mcp.port == kMaxMcpPort);
  // A bad value keeps the default.
  CHECK(ParseEditorSettings("mcp.port = abc").mcp.port == kDefaultMcpPort);
  CHECK_FALSE(ParseEditorSettings("mcp.enabled = maybe").mcp.enabled);
}

TEST_CASE("MCP URLs and the Claude Code command name the port") {
  CHECK(McpUrl(8765) == "http://127.0.0.1:8765/mcp");
  CHECK(McpClaudeCommand(9000) ==
        "claude mcp add --transport http blue-editor "
        "http://127.0.0.1:9000/mcp");
}

TEST_CASE("--mcp and --mcp-port start the server without saving") {
  auto parsed = ParseEditorOptions({"--mcp"});
  REQUIRE(parsed.ok());
  CHECK(parsed.value().mcp);
  CHECK_FALSE(parsed.value().mcp_port.has_value());

  parsed = ParseEditorOptions({"scene.mscn", "--mcp-port", "9001"});
  REQUIRE(parsed.ok());
  CHECK(parsed.value().mcp);
  CHECK(parsed.value().mcp_port == 9001);
  CHECK(parsed.value().file == "scene.mscn");

  CHECK_FALSE(ParseEditorOptions({"--mcp-port"}).ok());
  CHECK_FALSE(ParseEditorOptions({"--mcp-port", "80"}).ok());
  CHECK_FALSE(ParseEditorOptions({"--mcp-port", "x"}).ok());
}

}  // namespace
}  // namespace blue::editor
