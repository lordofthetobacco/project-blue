#ifndef BLUE_EDITOR_DIALOGS_H_
#define BLUE_EDITOR_DIALOGS_H_

#include <SDL3/SDL_video.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace blue::editor {

// The kinds of files the editor's dialogs filter for.
enum class FileKind {
  kPrefab,  // .pbo
  kScene,   // .mscn
  kModel,   // .gltf .glb .obj .fbx
  kImage,   // .png .jpg .jpeg .tga .bmp
  kHdr,     // .hdr
  kAny,
};

// Returns whether `path`'s extension (case-insensitive) is one of `kind`'s.
[[nodiscard]] bool MatchesFileKind(const std::string& path, FileKind kind);

// Called with the chosen path; not called when the dialog was cancelled.
using PathCallback = std::function<void(std::string path)>;

// Native file dialogs (SDL_ShowOpenFileDialog / SDL_ShowSaveFileDialog).
// SDL may call back on another thread; the result is queued and the callback
// runs on the main thread in RunCompleted(), between frames. While a dialog
// is open the editor keeps rendering. Neither copyable nor movable (SDL
// holds a pointer to it until the dialog closes). Not thread-safe apart from
// SDL's callback.
class Dialogs {
 public:
  Dialogs();
  ~Dialogs();

  Dialogs(const Dialogs&) = delete;
  Dialogs& operator=(const Dialogs&) = delete;

  // Sets the window the dialogs are modal to (may be null).
  void set_window(SDL_Window* window) { window_ = window; }

  // Shows an open dialog for `kind` starting in `directory` (may be empty).
  void Open(FileKind kind, const std::string& directory, PathCallback done);
  // Shows a save dialog for `kind`; the chosen path gets `kind`'s first
  // extension when it has none.
  void Save(FileKind kind, const std::string& default_path, PathCallback done);

  // Runs the callbacks of dialogs that closed since the last call.
  void RunCompleted();
  // Whether a dialog is open (shortcuts that open another one wait).
  [[nodiscard]] bool busy() const;

 private:
  // What SDL's callback and this object share. Open dialogs hold it too, so
  // a dialog still open at exit does not call into a destroyed object.
  struct Shared {
    std::mutex mutex;
    int open = 0;
    std::vector<std::function<void()>> completed;
  };
  // One open dialog: SDL's user data, deleted by OnClosed().
  struct Request {
    std::shared_ptr<Shared> shared;
    PathCallback done;
    FileKind kind = FileKind::kAny;
    bool save = false;
  };

  static void OnClosed(void* user_data, const char* const* files, int filter);
  void Show(FileKind kind, const std::string& location, PathCallback done,
            bool save);

  SDL_Window* window_ = nullptr;
  std::shared_ptr<Shared> shared_;
};

}  // namespace blue::editor

#endif  // BLUE_EDITOR_DIALOGS_H_
