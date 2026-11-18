#ifndef BLUE_EDITOR_MCP_MCP_TOOLS_H_
#define BLUE_EDITOR_MCP_MCP_TOOLS_H_

#include <cstdint>
#include <string>
#include <vector>

#include "mcp/server.h"

namespace blue::editor {

class EditorContext;

// The `instructions` the server sends in its initialize result: how a model
// uses the editor's tools.
[[nodiscard]] std::string EditorMcpInstructions();

// The editor's MCP tools and resources (listed at the top of
// mcp_tools.cc). Handlers run on the main thread inside mcp::Server::Poll()
// with full access to the context; every edit goes through the documents'
// undo history (one step per tool call, with a readable label) or through
// the same text commands the menus run. Long-running tools
// (viewport_screenshot, prefab_import_model) return "pending" and are
// finished by Update() on a later frame.
//
// Borrows `context` and `server`, which must outlive it (the server must
// not call handlers after this object is gone: destroy or stop the server
// first). Neither copyable nor movable. Main thread only.
class EditorTools {
 public:
  // Registers every tool and resource on `server`.
  EditorTools(EditorContext& context, mcp::Server& server);
  ~EditorTools();
  EditorTools(const EditorTools&) = delete;
  EditorTools& operator=(const EditorTools&) = delete;

  // Advances pending calls (screenshot readbacks, model imports) and
  // completes them; call once per frame after Poll().
  void Update();

 private:
  // A viewport_screenshot waiting for its renderer readback.
  struct PendingScreenshot {
    uint64_t call_id = 0;
    uint64_t ticket = 0;
    int max_width = 0;
    uint64_t requested_frame = 0;
  };
  // A prefab_import_model waiting for the background cook.
  struct PendingImport {
    uint64_t call_id = 0;
    std::string source;
    bool discard_unsaved = false;
    // EditorContext::messages() serial when the import started.
    uint64_t first_message = 0;
    uint64_t requested_frame = 0;
  };

  void RegisterEditorTools();
  void RegisterDocumentTools();
  void RegisterViewportTools();
  void RegisterResources();
  void UpdateScreenshots();
  void UpdateImports();

  EditorContext& context_;
  mcp::Server& server_;
  std::vector<PendingScreenshot> screenshots_;
  std::vector<PendingImport> imports_;
};

}  // namespace blue::editor

#endif  // BLUE_EDITOR_MCP_MCP_TOOLS_H_
