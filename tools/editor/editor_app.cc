#include "editor/editor_app.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_process.h>
#include <SDL3/SDL_timer.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/log.h"
#include "core/path.h"
#include "core/result.h"
#include "editor/asset_browser.h"
#include "editor/commands.h"
#include "editor/dialogs.h"
#include "editor/document.h"
#include "editor/editor_context.h"
#include "editor/editor_settings.h"
#include "editor/mcp/mcp_host.h"
#include "editor/panel.h"
#include "editor/selection.h"
#include "editor/viewport/viewport_panel.h"
#include "mcp/server.h"
#include "platform/file_system.h"
#include "platform/window.h"
#include "shader/shader_compiler.h"

namespace blue::editor {

namespace {

using VoidResult = core::Result<void, std::string>;

constexpr const char* kWindowTitle = "Blue editor";
constexpr const char* kPrefOrganization = "project-blue";
constexpr const char* kPrefApplication = "blue-editor";
constexpr const char* kLayoutFileName = "editor_layout.ini";
constexpr const char* kRecentFileName = "recent_files.txt";
constexpr const char* kSettingsFileName = "editor_settings.cfg";
// Recent MCP activity lines shown in the Settings window.
constexpr size_t kShownActivity = 12;
constexpr const char* kAssetDirectory = "assets";
constexpr const char* kDockSpaceName = "EditorDockSpace";
constexpr size_t kMaxRecentFiles = 10;
constexpr size_t kMaxRecentFileBytes = 64 * 1024;
// How often the shader sources are checked for edits, in frames.
constexpr uint64_t kShaderPollFrames = 15;

#ifdef NDEBUG
constexpr shader::CompileMode kShaderCompileMode =
    shader::CompileMode::kRelease;
#else
constexpr shader::CompileMode kShaderCompileMode = shader::CompileMode::kDebug;
#endif

#ifdef _WIN32
constexpr const char* kGameExecutable = "project-blue.exe";
#else
constexpr const char* kGameExecutable = "project-blue";
#endif

// Returns the shader source directory: the repository's in Debug (live
// edits), else the copy next to the executable.
std::string ShaderDirectory() {
#ifdef BLUE_DEV_SHADER_DIR
  if (platform::PathExists(BLUE_DEV_SHADER_DIR)) {
    return BLUE_DEV_SHADER_DIR;
  }
#endif
  return core::JoinPath(platform::BasePath(), "shaders");
}

// Returns the share of the dock space a slot's split takes.
constexpr float kSideShare = 0.2f;
constexpr float kRightShare = 0.25f;
constexpr float kBottomShare = 0.28f;
constexpr float kLowerShare = 0.45f;

std::string ModeName(EditorMode mode) {
  return mode == EditorMode::kPrefab ? "Prefab" : "Scene";
}

}  // namespace

EditorApp::EditorApp(EditorOptions options)
    : options_(std::move(options)),
      window_(kWindowTitle, options_.window_size.x, options_.window_size.y,
              options_.window_pos),
      renderer_(window_, ShaderDirectory(), kShaderCompileMode),
      imgui_(window_, renderer_,
             {.scale_mode = ui::UiScaleMode::kDisplayScale,
              .docking = true,
              .ini_path = core::JoinPath(
                  platform::PrefPath(kPrefOrganization, kPrefApplication),
                  kLayoutFileName),
              .change_mouse_cursor = true}),
      gpu_(renderer_),
      prefab_cache_(gpu_),
      context_(std::make_unique<EditorContext>(
          renderer_, imgui_, prefab_cache_,
          core::JoinPath(platform::BasePath(), kAssetDirectory))),
      pref_dir_(platform::PrefPath(kPrefOrganization, kPrefApplication)) {
  context_->dialogs().set_window(window_.handle());
  // The viewport image is no ImGui item (gizmos need that), so dragging in
  // an undocked viewport must not move its window.
  ImGui::GetIO().ConfigWindowsMoveFromTitleBarOnly = true;

  auto viewport = std::make_unique<ViewportPanel>();
  context_->set_viewport(viewport.get());
  AddPanel(ViewportPanel::Desc(), std::move(viewport));
  AddPanel(
      {.name = "Assets", .modes = kAllModes, .slot = DockSlot::kLeftBottom},
      std::make_unique<AssetBrowser>());

  RegisterShellCommands();
  LoadRecentFiles();

  settings_path_ = core::JoinPath(pref_dir_, kSettingsFileName);
  settings_ = LoadEditorSettings(settings_path_);
  mcp_ = std::make_unique<McpHost>(*context_);
  McpSettings mcp = settings_.mcp;
  if (options_.mcp) {  // Command-line override, not saved.
    mcp.enabled = true;
    mcp.port = options_.mcp_port.value_or(mcp.port);
  }
  SetMcpSettings(mcp, /*persist=*/false);
}

EditorApp::~EditorApp() {
  // Stop the server first: its tool handlers use the context and panels.
  mcp_.reset();
  if (game_process_ != nullptr) {
    SDL_DestroyProcess(game_process_);  // Leaves the game running.
  }
  // Panels may hold GPU or ImGui textures: drop them while both live.
  panels_.clear();
}

void EditorApp::AddPanel(PanelDesc desc, std::unique_ptr<Panel> panel) {
  const bool open = desc.open_by_default;
  panels_.push_back({std::move(desc), std::move(panel), open});
}

int EditorApp::Run() {
  if (!options_.file.empty()) {
    if (context_->OpenFile(options_.file).ok()) {
      AddRecentFile(options_.file);
    }
  }
  uint64_t last = SDL_GetTicksNS();
  bool first_frame = true;
  while (running_) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      HandleEvent(event);
    }
    const uint64_t now = SDL_GetTicksNS();
    const float dt = static_cast<float>(now - last) * 1.0e-9f;
    last = now;
    frame_ms_ = dt * 1000.0f;
    Frame(std::min(dt, 0.1f));
    if (first_frame && !options_.exec.empty()) {
      context_->commands().Enqueue(options_.exec);
    }
    first_frame = false;
  }
  SaveRecentFiles();
  return 0;
}

void EditorApp::HandleEvent(const SDL_Event& event) {
  imgui_.ProcessEvent(event);
  switch (event.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
      RequestQuit();
      break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
      renderer_.RequestSwapchainRebuild();
      break;
    case SDL_EVENT_KEY_DOWN:
      // Typing into a text field keeps every key.
      if (!event.key.repeat && !ImGui::GetIO().WantTextInput) {
        HandleShortcut(event.key);
      }
      break;
    default:
      break;
  }
}

bool EditorApp::HandleShortcut(const SDL_KeyboardEvent& key) {
  const bool ctrl = (key.mod & SDL_KMOD_CTRL) != 0;
  const bool shift = (key.mod & SDL_KMOD_SHIFT) != 0;
  std::string_view command;
  if (ctrl) {
    switch (key.key) {
      case SDLK_N:
        command = "new-scene";
        break;
      case SDLK_O:
        command = "open";
        break;
      case SDLK_I:
        command = "import";
        break;
      case SDLK_S:
        command = shift ? "save-as" : "save";
        break;
      case SDLK_Z:
        command = shift ? "redo" : "undo";
        break;
      case SDLK_Y:
        command = "redo";
        break;
      case SDLK_Q:
        command = "quit";
        break;
      default:
        break;
    }
  } else {
    switch (key.key) {
      case SDLK_F1:
        command = "mode prefab";
        break;
      case SDLK_F2:
        command = "mode scene";
        break;
      case SDLK_F5:
        command = "play";
        break;
      case SDLK_TAB:
        command = shift ? "select prev" : "select next";
        break;
      default:
        break;
    }
  }
  if (command.empty()) {
    return false;
  }
  context_->commands().Enqueue(command);
  return true;
}

void EditorApp::Frame(float dt) {
  context_->dialogs().RunCompleted();
  context_->commands().RunQueued();
  mcp_->Update();
  if (pending_mcp_) {
    SetMcpSettings(*pending_mcp_, /*persist=*/true);
    pending_mcp_.reset();
  }
  context_->SyncPrefabPreview();
  if (context_->frame_count() % kShaderPollFrames == 0) {
    renderer_.ReloadChangedShaders();
  }

  imgui_.BeginFrame();
  context_->BeginFrame();
  for (PanelEntry& entry : panels_) {
    entry.panel->Update(*context_, dt);
  }
  DrawMenuBar();
  DrawStatusBar();
  DrawDockSpace();
  DrawPanels();
  DrawModals();
  DrawSettingsWindow();
  ImDrawData* const ui = imgui_.EndFrame();

  if (renderer_.viewport().generation == 0) {
    // No viewport panel has prepared an image yet (e.g. it is closed).
    (void)renderer_.PrepareViewport({1, 1});
  }
  const render::SceneView* view = context_->frame_view();
  renderer_.DrawViewportFrame(view != nullptr ? *view : empty_view_, ui);
}

void EditorApp::DrawMenuBar() {
  if (!ImGui::BeginMainMenuBar()) {
    return;
  }
  CommandRegistry& commands = context_->commands();
  if (ImGui::BeginMenu("File")) {
    if (ImGui::MenuItem("New scene", "Ctrl+N")) {
      commands.Enqueue("new-scene");
    }
    if (ImGui::MenuItem("Open...", "Ctrl+O")) {
      commands.Enqueue("open");
    }
    if (ImGui::MenuItem("Import model...", "Ctrl+I")) {
      commands.Enqueue("import");
    }
    if (ImGui::BeginMenu("Open recent", !recent_files_.empty())) {
      for (const std::string& path : recent_files_) {
        if (ImGui::MenuItem(context_->DisplayPath(path).c_str())) {
          commands.Enqueue(std::format("open \"{}\"", path));
        }
      }
      ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Save", "Ctrl+S")) {
      commands.Enqueue("save");
    }
    if (ImGui::MenuItem("Save as...", "Ctrl+Shift+S")) {
      commands.Enqueue("save-as");
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Play", "F5", false,
                        context_->scene_document() != nullptr)) {
      commands.Enqueue("play");
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Quit", "Ctrl+Q")) {
      commands.Enqueue("quit");
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Edit")) {
    PrefabDocument* prefab = context_->prefab_document();
    SceneDocument* scene = context_->scene_document();
    const bool prefab_mode = context_->mode() == EditorMode::kPrefab;
    const bool can_undo = prefab_mode ? prefab && prefab->history().can_undo()
                                      : scene && scene->history().can_undo();
    const bool can_redo = prefab_mode ? prefab && prefab->history().can_redo()
                                      : scene && scene->history().can_redo();
    const std::string_view undo_label = !can_undo ? std::string_view()
                                        : prefab_mode
                                            ? prefab->history().undo_label()
                                            : scene->history().undo_label();
    const std::string_view redo_label = !can_redo ? std::string_view()
                                        : prefab_mode
                                            ? prefab->history().redo_label()
                                            : scene->history().redo_label();
    if (ImGui::MenuItem(std::format("Undo {}", undo_label).c_str(), "Ctrl+Z",
                        false, can_undo)) {
      commands.Enqueue("undo");
    }
    if (ImGui::MenuItem(std::format("Redo {}", redo_label).c_str(), "Ctrl+Y",
                        false, can_redo)) {
      commands.Enqueue("redo");
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Settings...")) {
      commands.Enqueue("settings");
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("View")) {
    for (PanelEntry& entry : panels_) {
      ImGui::MenuItem(entry.desc.name.c_str(), nullptr, &entry.open);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Reset layout")) {
      reset_layout_ = true;
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Mode")) {
    if (ImGui::MenuItem("Prefab", "F1",
                        context_->mode() == EditorMode::kPrefab)) {
      context_->SetMode(EditorMode::kPrefab);
    }
    if (ImGui::MenuItem("Scene", "F2",
                        context_->mode() == EditorMode::kScene)) {
      context_->SetMode(EditorMode::kScene);
    }
    ImGui::EndMenu();
  }
  // The mode switch as two tabs at the right of the menu bar.
  ImGui::Separator();
  for (const EditorMode mode : {EditorMode::kPrefab, EditorMode::kScene}) {
    const bool active = context_->mode() == mode;
    if (active) {
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::SmallButton(ModeName(mode).c_str())) {
      context_->SetMode(mode);
    }
    if (active) {
      ImGui::PopStyleColor();
    }
  }
  ImGui::EndMainMenuBar();
}

void EditorApp::DrawStatusBar() {
  constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoScrollbar |
                                      ImGuiWindowFlags_NoSavedSettings |
                                      ImGuiWindowFlags_MenuBar;
  const float height = ImGui::GetFrameHeight();
  if (ImGui::BeginViewportSideBar("##status", ImGui::GetMainViewport(),
                                  ImGuiDir_Down, height, kFlags)) {
    if (ImGui::BeginMenuBar()) {
      const bool prefab_mode = context_->mode() == EditorMode::kPrefab;
      std::string document = "no document";
      if (prefab_mode && context_->prefab_document() != nullptr) {
        const PrefabDocument& doc = *context_->prefab_document();
        document = DocumentTitle(doc.path()) + (doc.dirty() ? " *" : "");
      } else if (!prefab_mode && context_->scene_document() != nullptr) {
        const SceneDocument& doc = *context_->scene_document();
        document = DocumentTitle(doc.path()) + (doc.dirty() ? " *" : "");
      }
      ImGui::Text("%s | %s | %.1f ms", ModeName(context_->mode()).c_str(),
                  document.c_str(), static_cast<double>(frame_ms_));
      if (mcp_->listening()) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.4f, 1.0f), "MCP :%d",
                           mcp_->applied().port);
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("MCP server listening on %s",
                            McpUrl(mcp_->applied().port).c_str());
        }
        if (ImGui::IsItemClicked()) {
          settings_open_ = true;
        }
      }
      ImGui::Separator();
      ImGui::TextUnformatted(context_->status().c_str());
      ImGui::EndMenuBar();
    }
  }
  ImGui::End();
}

void EditorApp::DrawDockSpace() {
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  const ImGuiID dock_id = ImGui::GetID(kDockSpaceName);
  if (reset_layout_ || ImGui::DockBuilderGetNode(dock_id) == nullptr) {
    BuildDefaultLayout(dock_id);
    reset_layout_ = false;
  }
  ImGui::DockSpaceOverViewport(dock_id, viewport);
}

void EditorApp::BuildDefaultLayout(ImGuiID dock_id) {
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::DockBuilderRemoveNode(dock_id);
  ImGui::DockBuilderAddNode(dock_id, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(dock_id, viewport->WorkSize);

  ImGuiID center = dock_id;
  const ImGuiID left = ImGui::DockBuilderSplitNode(
      center, ImGuiDir_Left, kSideShare, nullptr, &center);
  const ImGuiID right = ImGui::DockBuilderSplitNode(
      center, ImGuiDir_Right, kRightShare, nullptr, &center);
  const ImGuiID bottom = ImGui::DockBuilderSplitNode(
      center, ImGuiDir_Down, kBottomShare, nullptr, &center);
  ImGuiID left_top = left;
  const ImGuiID left_bottom = ImGui::DockBuilderSplitNode(
      left, ImGuiDir_Down, kLowerShare, nullptr, &left_top);
  ImGuiID right_top = right;
  const ImGuiID right_bottom = ImGui::DockBuilderSplitNode(
      right, ImGuiDir_Down, kLowerShare, nullptr, &right_top);

  for (const PanelEntry& entry : panels_) {
    ImGuiID target = center;
    switch (entry.desc.slot) {
      case DockSlot::kCenter:
        target = center;
        break;
      case DockSlot::kLeft:
        target = left_top;
        break;
      case DockSlot::kLeftBottom:
        target = left_bottom;
        break;
      case DockSlot::kRight:
        target = right_top;
        break;
      case DockSlot::kRightBottom:
        target = right_bottom;
        break;
      case DockSlot::kBottom:
        target = bottom;
        break;
    }
    ImGui::DockBuilderDockWindow(entry.desc.name.c_str(), target);
  }
  ImGui::DockBuilderFinish(dock_id);
}

void EditorApp::DrawPanels() {
  const auto mode_bit =
      static_cast<uint8_t>(1u << static_cast<int>(context_->mode()));
  for (PanelEntry& entry : panels_) {
    if (!entry.open || (entry.desc.modes & mode_bit) == 0) {
      continue;
    }
    if (entry.desc.no_padding) {
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    }
    const bool visible = ImGui::Begin(entry.desc.name.c_str(), &entry.open,
                                      entry.desc.window_flags);
    if (entry.desc.no_padding) {
      ImGui::PopStyleVar();
    }
    if (visible) {
      entry.panel->Draw(*context_);
    }
    ImGui::End();
  }
}

void EditorApp::DrawModals() {
  std::vector<EditorContext::ErrorMessage>& errors = context_->pending_errors();
  if (!errors.empty() && !ImGui::IsPopupOpen("Error")) {
    ImGui::OpenPopup("Error");
  }
  if (ImGui::BeginPopupModal("Error", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    if (errors.empty()) {
      ImGui::CloseCurrentPopup();
    } else {
      ImGui::TextUnformatted(errors.front().title.c_str());
      ImGui::Separator();
      ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
      ImGui::TextUnformatted(errors.front().message.c_str());
      ImGui::PopTextWrapPos();
      if (ImGui::Button("OK") || ImGui::IsKeyPressed(ImGuiKey_Enter) ||
          ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        errors.erase(errors.begin());
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }

  const std::optional<EditorContext::PendingReplace>& pending =
      context_->pending_replace();
  if (pending && !ImGui::IsPopupOpen("Unsaved changes")) {
    ImGui::OpenPopup("Unsaved changes");
  }
  if (ImGui::BeginPopupModal("Unsaved changes", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    if (!pending) {
      ImGui::CloseCurrentPopup();
    } else {
      ImGui::Text("%s has unsaved changes.", pending->document_title.c_str());
      ImGui::TextDisabled("Enter: save, D: discard, Esc: cancel");
      using Choice = EditorContext::ReplaceChoice;
      std::optional<Choice> choice;
      if (ImGui::Button("Save") || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        choice = Choice::kSave;
      }
      ImGui::SameLine();
      if (ImGui::Button("Discard") || ImGui::IsKeyPressed(ImGuiKey_D)) {
        choice = Choice::kDiscard;
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        choice = Choice::kCancel;
      }
      if (choice) {
        ImGui::CloseCurrentPopup();
        context_->ResolvePendingReplace(*choice);
      }
    }
    ImGui::EndPopup();
  }

  if (quit_modal_ && !ImGui::IsPopupOpen("Quit")) {
    ImGui::OpenPopup("Quit");
  }
  if (ImGui::BeginPopupModal("Quit", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("There are unsaved changes.");
    ImGui::TextDisabled("Enter: save all, D: discard, Esc: cancel");
    if (ImGui::Button("Save all and quit") ||
        ImGui::IsKeyPressed(ImGuiKey_Enter)) {
      context_->commands().Enqueue("save-all; quit");
      quit_modal_ = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard and quit") || ImGui::IsKeyPressed(ImGuiKey_D)) {
      running_ = false;
      quit_modal_ = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      quit_modal_ = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

bool EditorApp::HasUnsavedChanges() {
  const PrefabDocument* prefab = context_->prefab_document();
  const SceneDocument* scene = context_->scene_document();
  return (prefab != nullptr && prefab->dirty()) ||
         (scene != nullptr && scene->dirty());
}

void EditorApp::RequestQuit() {
  if (HasUnsavedChanges()) {
    quit_modal_ = true;
  } else {
    running_ = false;
  }
}

void EditorApp::SelectAdjacent(int step) {
  Selection& selection = context_->selection();
  if (context_->mode() == EditorMode::kScene) {
    SceneDocument* scene = context_->scene_document();
    if (scene == nullptr || scene->state().instances.empty()) {
      return;
    }
    const std::vector<scene::Instance>& instances = scene->state().instances;
    const auto count = static_cast<int>(instances.size());
    const int64_t current = selection.PrimaryIndex(SelectionKind::kInstance);
    int position = -1;
    for (int i = 0; i < count; ++i) {
      if (static_cast<int64_t>(instances[static_cast<size_t>(i)].id) ==
          current) {
        position = i;
      }
    }
    const int next = position < 0 ? (step > 0 ? 0 : count - 1)
                                  : ((position + step) % count + count) % count;
    selection.Set(InstanceItem(instances[static_cast<size_t>(next)].id));
    context_->Status(
        std::format("Selected {}", instances[static_cast<size_t>(next)].name));
    return;
  }
  PrefabDocument* prefab = context_->prefab_document();
  if (prefab == nullptr || prefab->state().hierarchy.nodes.empty()) {
    return;
  }
  const auto count =
      static_cast<int64_t>(prefab->state().hierarchy.nodes.size());
  const int64_t current = selection.PrimaryIndex(SelectionKind::kNode);
  const int64_t next = current < 0 ? (step > 0 ? 0 : count - 1)
                                   : ((current + step) % count + count) % count;
  selection.Set({.kind = SelectionKind::kNode, .index = next});
  context_->Status(std::format(
      "Selected node {}",
      prefab->state().hierarchy.nodes[static_cast<size_t>(next)].name));
}

VoidResult EditorApp::Play() {
  SceneDocument* scene = context_->scene_document();
  if (scene == nullptr) {
    return VoidResult::Error("no scene is open");
  }
  if (scene->path().empty()) {
    return VoidResult::Error("save the scene first (Ctrl+S)");
  }
  if (scene->dirty()) {
    VoidResult saved = SaveSceneDocument(*scene);
    if (!saved.ok()) {
      return saved;
    }
  }
  const std::string game =
      core::JoinPath(platform::BasePath(), kGameExecutable);
  if (!platform::IsRegularFile(game)) {
    return VoidResult::Error(std::format("{} not found", game));
  }
  std::vector<std::string> args{game, "--scene", scene->path()};
  if (options_.window_pos) {
    args.push_back("--window-pos");
    args.push_back(
        std::format("{},{}", options_.window_pos->x, options_.window_pos->y));
  }
  std::vector<const char*> argv;
  argv.reserve(args.size() + 1);
  for (const std::string& arg : args) {
    argv.push_back(arg.c_str());
  }
  argv.push_back(nullptr);
  SDL_Process* process = SDL_CreateProcess(argv.data(), /*pipe_stdio=*/false);
  if (process == nullptr) {
    return VoidResult::Error(
        std::format("cannot start {}: {}", game, SDL_GetError()));
  }
  if (game_process_ != nullptr) {
    SDL_DestroyProcess(game_process_);  // Leaves the older game running.
  }
  game_process_ = process;
  context_->Status(
      std::format("Playing {}", context_->DisplayPath(scene->path())));
  return VoidResult::Ok();
}

void EditorApp::OpenWithDialog() {
  const FileKind kind = context_->mode() == EditorMode::kPrefab
                            ? FileKind::kPrefab
                            : FileKind::kScene;
  context_->dialogs().Open(kind, context_->asset_root(),
                           [this](std::string path) {
                             if (context_->OpenFile(path).ok()) {
                               AddRecentFile(path);
                             }
                           });
}

void EditorApp::AddRecentFile(const std::string& path) {
  const std::string absolute = platform::AbsolutePath(path);
  std::erase(recent_files_, absolute);
  recent_files_.insert(recent_files_.begin(), absolute);
  if (recent_files_.size() > kMaxRecentFiles) {
    recent_files_.resize(kMaxRecentFiles);
  }
}

void EditorApp::LoadRecentFiles() {
  const auto bytes = platform::ReadFile(
      core::JoinPath(pref_dir_, kRecentFileName), kMaxRecentFileBytes);
  if (!bytes.ok()) {
    return;
  }
  const std::string_view text(
      reinterpret_cast<const char*>(bytes.value().data()),
      bytes.value().size());
  size_t start = 0;
  while (start < text.size() && recent_files_.size() < kMaxRecentFiles) {
    const size_t end = std::min(text.find('\n', start), text.size());
    std::string_view line = text.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (!line.empty()) {
      recent_files_.emplace_back(line);
    }
    start = end + 1;
  }
}

void EditorApp::SaveRecentFiles() const {
  std::string text;
  for (const std::string& path : recent_files_) {
    text += path;
    text += '\n';
  }
  (void)platform::WriteFileAtomic(core::JoinPath(pref_dir_, kRecentFileName),
                                  std::as_bytes(std::span(text)));
}

void EditorApp::RegisterShellCommands() {
  CommandRegistry& commands = context_->commands();
  EditorContext& context = *context_;

  commands.Register(
      "open", "open [path]: opens a .pbo / .mscn (a dialog without a path)",
      [this](std::span<const std::string> args) {
        if (args.empty()) {
          OpenWithDialog();
          return VoidResult::Ok();
        }
        VoidResult opened = context_->OpenFile(args[0]);
        if (opened.ok()) {
          AddRecentFile(args[0]);
        }
        return opened;
      });
  commands.Register("new-scene", "new-scene: starts an empty scene",
                    [&context](std::span<const std::string>) {
                      context.NewSceneDocument();
                      return VoidResult::Ok();
                    });
  commands.Register("import", "import: File > Import model...",
                    [&context](std::span<const std::string>) {
                      context.ImportModel();
                      return VoidResult::Ok();
                    });
  commands.Register("save",
                    "save [path]: saves the active document (to path: Save As)",
                    [this, &context](std::span<const std::string> args) {
                      if (args.empty()) {
                        context.SaveActive();
                        return VoidResult::Ok();
                      }
                      VoidResult saved = context.SaveActiveTo(args[0]);
                      if (saved.ok()) {
                        AddRecentFile(args[0]);
                      }
                      return saved;
                    });
  commands.Register("save-as",
                    "save-as [path]: Save As (a dialog without a path)",
                    [this, &context](std::span<const std::string> args) {
                      if (args.empty()) {
                        context.SaveActiveAs();
                        return VoidResult::Ok();
                      }
                      VoidResult saved = context.SaveActiveTo(args[0]);
                      if (saved.ok()) {
                        AddRecentFile(args[0]);
                      }
                      return saved;
                    });
  commands.Register(
      "save-all", "save-all: saves every open titled document",
      [&context](std::span<const std::string>) { return context.SaveAll(); });
  commands.Register("undo", "undo: undoes the last edit of the active document",
                    [&context](std::span<const std::string>) {
                      return context.Undo()
                                 ? VoidResult::Ok()
                                 : VoidResult::Error("nothing to undo");
                    });
  commands.Register("redo", "redo: redoes the last undone edit",
                    [&context](std::span<const std::string>) {
                      return context.Redo()
                                 ? VoidResult::Ok()
                                 : VoidResult::Error("nothing to redo");
                    });
  commands.Register("mode", "mode <prefab|scene>: switches the mode",
                    [&context](std::span<const std::string> args) {
                      if (args.size() == 1 && args[0] == "prefab") {
                        context.SetMode(EditorMode::kPrefab);
                      } else if (args.size() == 1 && args[0] == "scene") {
                        context.SetMode(EditorMode::kScene);
                      } else {
                        return VoidResult::Error("expects prefab or scene");
                      }
                      return VoidResult::Ok();
                    });
  commands.Register("play", "play: saves the scene and starts the game on it",
                    [this](std::span<const std::string>) {
                      VoidResult played = Play();
                      if (!played.ok()) {
                        context_->ShowError("Cannot play", played.error());
                      }
                      return played;
                    });
  commands.Register(
      "select",
      "select <next|prev|none> | select <kind> <index>: kinds node mesh "
      "material texture socket light child clip instance (id) point-light "
      "collider player-start sun environment",
      [this, &context](std::span<const std::string> args) {
        if (args.size() == 1 && (args[0] == "next" || args[0] == "prev")) {
          SelectAdjacent(args[0] == "next" ? 1 : -1);
          return VoidResult::Ok();
        }
        if (args.size() == 1 && args[0] == "none") {
          context.selection().Clear();
          return VoidResult::Ok();
        }
        if (args.empty()) {
          return VoidResult::Error(
              "expects next, prev, none or <kind> <index>");
        }
        const std::optional<SelectionKind> kind = ParseSelectionKind(args[0]);
        if (!kind) {
          return VoidResult::Error(std::format("unknown kind {}", args[0]));
        }
        int64_t index = -1;
        if (args.size() > 1) {
          core::Result<int, std::string> parsed = ParseIntArg(args[1], "index");
          if (!parsed.ok()) {
            return VoidResult::Error(parsed.error());
          }
          index = parsed.value();
        }
        context.selection().Set({.kind = *kind, .index = index});
        return VoidResult::Ok();
      });
  commands.Register(
      "panel", "panel <name> <show|hide>: opens or closes a panel",
      [this](std::span<const std::string> args) {
        if (args.size() != 2 || (args[1] != "show" && args[1] != "hide")) {
          return VoidResult::Error("expects <name> show|hide");
        }
        for (PanelEntry& entry : panels_) {
          if (entry.desc.name == args[0]) {
            entry.open = args[1] == "show";
            return VoidResult::Ok();
          }
        }
        return VoidResult::Error(std::format("no panel {}", args[0]));
      });
  commands.Register("reset-layout",
                    "reset-layout: restores the default docking",
                    [this](std::span<const std::string>) {
                      reset_layout_ = true;
                      return VoidResult::Ok();
                    });
  commands.Register("status", "status <text>: shows text in the status bar",
                    [&context](std::span<const std::string> args) {
                      std::string text;
                      for (const std::string& word : args) {
                        text += text.empty() ? word : " " + word;
                      }
                      context.Status(std::move(text));
                      return VoidResult::Ok();
                    });
  commands.Register("settings", "settings: opens Edit > Settings...",
                    [this](std::span<const std::string>) {
                      settings_open_ = true;
                      return VoidResult::Ok();
                    });
  commands.Register(
      "mcp",
      "mcp on|off|port <n>|status: the MCP server (saved like Edit > "
      "Settings...)",
      [this](std::span<const std::string> args) {
        McpSettings mcp = mcp_->applied();
        if (args.size() == 1 && (args[0] == "on" || args[0] == "off")) {
          mcp.enabled = args[0] == "on";
        } else if (args.size() == 2 && args[0] == "port") {
          core::Result<int, std::string> port = ParseIntArg(args[1], "port");
          if (!port.ok()) {
            return VoidResult::Error(port.error());
          }
          if (port.value() < kMinMcpPort || port.value() > kMaxMcpPort) {
            return VoidResult::Error(std::format("port must lie in {}..{}",
                                                 kMinMcpPort, kMaxMcpPort));
          }
          mcp.port = port.value();
        } else if (args.size() == 1 && args[0] == "status") {
          const mcp::ServerStatus status = mcp_->Status();
          context_->Status(std::format(
              "MCP server: {}; {} open connection(s), {} request(s), {} tool "
              "call(s)",
              status.state == mcp::ServerStatus::State::kListening
                  ? "listening on " + McpUrl(status.port)
              : status.state == mcp::ServerStatus::State::kError
                  ? "error: " + status.last_error
                  : std::string("stopped"),
              status.open_connections, status.requests, status.tool_calls));
          return VoidResult::Ok();
        } else {
          return VoidResult::Error("expects on, off, port <n> or status");
        }
        // Applied by the next frame, outside mcp::Server::Poll(): this
        // command may come from a tool call of the server it restarts.
        pending_mcp_ = mcp;
        context_->Status(
            std::format("MCP server: {} on port {} (applied next "
                        "frame)",
                        mcp.enabled ? "on" : "off", mcp.port));
        return VoidResult::Ok();
      });
  commands.Register("quit", "quit [force]: quits (asks about unsaved changes)",
                    [this](std::span<const std::string> args) {
                      if (!args.empty() && args[0] == "force") {
                        running_ = false;
                      } else {
                        RequestQuit();
                      }
                      return VoidResult::Ok();
                    });
}

void EditorApp::SetMcpSettings(const McpSettings& settings, bool persist) {
  (void)mcp_->Apply(settings);  // A start error shows in the status.
  mcp_draft_ = settings;
  if (persist && settings_.mcp != settings) {
    settings_.mcp = settings;
    (void)SaveEditorSettings(settings_, settings_path_);
  }
}

void EditorApp::DrawSettingsWindow() {
  if (!settings_open_) {
    return;
  }
  ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 36.0f, 0.0f),
                           ImGuiCond_FirstUseEver);
  if (ImGui::Begin("Settings", &settings_open_, ImGuiWindowFlags_NoDocking)) {
    if (ImGui::CollapsingHeader("MCP server", ImGuiTreeNodeFlags_DefaultOpen)) {
      DrawMcpSettings();
    }
  }
  ImGui::End();
}

void EditorApp::DrawMcpSettings() {
  ImGui::TextWrapped(
      "A Model Context Protocol server: MCP clients (Claude Code, Claude "
      "Desktop, MCP Inspector, ...) connect to it to read and edit the open "
      "documents, run editor commands and take viewport screenshots.");
  ImGui::Checkbox("Enabled", &mcp_draft_.enabled);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
  ImGui::InputInt("Port", &mcp_draft_.port, 0);
  const bool port_ok =
      mcp_draft_.port >= kMinMcpPort && mcp_draft_.port <= kMaxMcpPort;
  const bool changed = mcp_draft_ != mcp_->applied();
  ImGui::BeginDisabled(!changed || !port_ok);
  if (ImGui::Button("Apply")) {
    SetMcpSettings(mcp_draft_, /*persist=*/true);
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!changed);
  if (ImGui::Button("Revert")) {
    mcp_draft_ = mcp_->applied();
  }
  ImGui::EndDisabled();
  if (!port_ok) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "Port: %d..%d",
                       kMinMcpPort, kMaxMcpPort);
  } else if (changed) {
    ImGui::SameLine();
    ImGui::TextDisabled("(not applied)");
  }

  const mcp::ServerStatus status = mcp_->Status();
  const std::string url = McpUrl(status.port);
  ImGui::Separator();
  switch (status.state) {
    case mcp::ServerStatus::State::kListening:
      ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.4f, 1.0f), "Listening on %s",
                         url.c_str());
      break;
    case mcp::ServerStatus::State::kError:
      ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "Error: %s",
                         status.last_error.c_str());
      break;
    case mcp::ServerStatus::State::kStopped:
      ImGui::TextDisabled("Stopped");
      break;
  }
  const int port = mcp_->applied().port;
  if (ImGui::Button("Copy URL")) {
    ImGui::SetClipboardText(McpUrl(port).c_str());
  }
  ImGui::SameLine();
  if (ImGui::Button("Copy Claude Code command")) {
    ImGui::SetClipboardText(McpClaudeCommand(port).c_str());
  }
  ImGui::TextDisabled("Add to Claude Code:");
  ImGui::TextWrapped("%s", McpClaudeCommand(port).c_str());
  ImGui::Text(
      "Connections: %d open, %lld total | Requests: %lld | Tool calls: %lld | "
      "Waiting: %d",
      status.open_connections, static_cast<long long>(status.total_connections),
      static_cast<long long>(status.requests),
      static_cast<long long>(status.tool_calls), status.waiting_calls);

  ImGui::TextDisabled("Recent activity");
  if (ImGui::BeginChild("##mcp_activity",
                        ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() *
                                         static_cast<float>(kShownActivity)),
                        ImGuiChildFlags_Borders)) {
    const auto now = std::chrono::system_clock::now();
    const size_t count = status.recent_activity.size();
    for (size_t i = count > kShownActivity ? count - kShownActivity : 0;
         i < count; ++i) {
      const mcp::ActivityEntry& entry = status.recent_activity[i];
      const auto age =
          std::chrono::duration_cast<std::chrono::seconds>(now - entry.time)
              .count();
      ImGui::TextDisabled("%5llds", static_cast<long long>(age));
      ImGui::SameLine();
      ImGui::TextUnformatted(entry.text.c_str());
    }
  }
  ImGui::EndChild();
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f),
                     "While enabled, any program on this computer can connect "
                     "(loopback only, no authentication) and edit or save the "
                     "open documents. Off by default.");
  ImGui::PopTextWrapPos();
}

}  // namespace blue::editor
