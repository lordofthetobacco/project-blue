#include "editor/dialogs.h"

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_error.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/log.h"
#include "core/path.h"

namespace blue::editor {

namespace {

// SDL keeps pointers to the filters until the dialog closes: static storage.
constexpr std::array<SDL_DialogFileFilter, 1> kPrefabFilter{
    {{"Prefabs (*.pbo)", "pbo"}}};
constexpr std::array<SDL_DialogFileFilter, 1> kSceneFilter{
    {{"Scenes (*.mscn)", "mscn"}}};
constexpr std::array<SDL_DialogFileFilter, 1> kModelFilter{
    {{"Models (*.gltf, *.glb, *.obj, *.fbx)", "gltf;glb;obj;fbx"}}};
constexpr std::array<SDL_DialogFileFilter, 1> kImageFilter{
    {{"Images (*.png, *.jpg, *.tga, *.bmp)", "png;jpg;jpeg;tga;bmp"}}};
constexpr std::array<SDL_DialogFileFilter, 1> kHdrFilter{
    {{"HDR images (*.hdr)", "hdr"}}};

std::span<const SDL_DialogFileFilter> FiltersOf(FileKind kind) {
  switch (kind) {
    case FileKind::kPrefab:
      return kPrefabFilter;
    case FileKind::kScene:
      return kSceneFilter;
    case FileKind::kModel:
      return kModelFilter;
    case FileKind::kImage:
      return kImageFilter;
    case FileKind::kHdr:
      return kHdrFilter;
    case FileKind::kAny:
      return {};
  }
  return {};
}

// Returns the lower-case extension of `path` without the dot, or "".
std::string LowerExtension(const std::string& path) {
  const std::string name = core::FileName(path);
  const size_t dot = name.rfind('.');
  if (dot == std::string::npos) {
    return {};
  }
  std::string extension = name.substr(dot + 1);
  std::transform(
      extension.begin(), extension.end(), extension.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return extension;
}

}  // namespace

bool MatchesFileKind(const std::string& path, FileKind kind) {
  if (kind == FileKind::kAny) {
    return true;
  }
  const std::string extension = LowerExtension(path);
  if (extension.empty()) {
    return false;
  }
  for (const SDL_DialogFileFilter& filter : FiltersOf(kind)) {
    std::string_view pattern = filter.pattern;
    while (!pattern.empty()) {
      const size_t end = std::min(pattern.find(';'), pattern.size());
      if (pattern.substr(0, end) == extension) {
        return true;
      }
      pattern.remove_prefix(std::min(end + 1, pattern.size()));
    }
  }
  return false;
}

Dialogs::Dialogs() : shared_(std::make_shared<Shared>()) {}

// Dialogs still open keep `shared_` alive through their requests; their
// results are never run.
Dialogs::~Dialogs() = default;

void Dialogs::Open(FileKind kind, const std::string& directory,
                   PathCallback done) {
  Show(kind, directory, std::move(done), /*save=*/false);
}

void Dialogs::Save(FileKind kind, const std::string& default_path,
                   PathCallback done) {
  Show(kind, default_path, std::move(done), /*save=*/true);
}

void Dialogs::Show(FileKind kind, const std::string& location,
                   PathCallback done, bool save) {
  {
    std::lock_guard lock(shared_->mutex);
    ++shared_->open;
  }
  // Owned by SDL until OnClosed() takes it back.
  auto request =
      std::make_unique<Request>(Request{shared_, std::move(done), kind, save});
  const std::span<const SDL_DialogFileFilter> filters = FiltersOf(kind);
  const char* const default_location =
      location.empty() ? nullptr : location.c_str();
  if (save) {
    SDL_ShowSaveFileDialog(&Dialogs::OnClosed, request.release(), window_,
                           filters.data(), static_cast<int>(filters.size()),
                           default_location);
  } else {
    SDL_ShowOpenFileDialog(&Dialogs::OnClosed, request.release(), window_,
                           filters.data(), static_cast<int>(filters.size()),
                           default_location, /*allow_many=*/false);
  }
}

void Dialogs::OnClosed(void* user_data, const char* const* files,
                       int /*filter*/) {
  std::unique_ptr<Request> request(static_cast<Request*>(user_data));
  std::string path;
  if (files == nullptr) {
    core::log::Error("File dialog failed: {}", SDL_GetError());
  } else if (files[0] != nullptr) {
    path = files[0];
    // A save dialog may return a name without the extension.
    const std::span<const SDL_DialogFileFilter> filters =
        FiltersOf(request->kind);
    if (request->save && !filters.empty() &&
        !MatchesFileKind(path, request->kind)) {
      const std::string_view pattern = filters.front().pattern;
      path += '.';
      path += pattern.substr(0, pattern.find(';'));
    }
  }
  Shared& shared = *request->shared;
  std::lock_guard lock(shared.mutex);
  --shared.open;
  if (!path.empty()) {
    shared.completed.push_back(
        [done = std::move(request->done), path = std::move(path)]() mutable {
          done(std::move(path));
        });
  }
}

void Dialogs::RunCompleted() {
  std::vector<std::function<void()>> completed;
  {
    std::lock_guard lock(shared_->mutex);
    completed.swap(shared_->completed);
  }
  for (std::function<void()>& callback : completed) {
    callback();
  }
}

bool Dialogs::busy() const {
  std::lock_guard lock(shared_->mutex);
  return shared_->open > 0;
}

}  // namespace blue::editor
