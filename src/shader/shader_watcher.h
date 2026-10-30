#ifndef BLUE_SHADER_SHADER_WATCHER_H_
#define BLUE_SHADER_SHADER_WATCHER_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace blue::shader {

// Detects edits to shader sources by polling modification times. Tracks
// "root" shaders (the files that get compiled) together with the files each
// one includes, and reports the roots that need recompiling.
//
// A change is reported only after it has been stable across two consecutive
// polls, so an editor that saves in several writes does not trigger a
// compile of a half-written file. A file that disappears counts as changed
// but is not reported until it exists again (and is stable), which catches
// save-by-rename. Each change is reported once; after compiling, call Watch()
// again to take a new snapshot (a later edit is reported again even without
// it, e.g. when a compile failed and the user fixes the file).
//
// The caller drives the polling rate. No threads, no file system events.
// Copyable. Not thread-safe.
class ShaderWatcher {
 public:
  // Starts watching `root` and `dependencies` (the files it includes), or
  // replaces the dependency set of an already watched root. Takes the
  // baseline snapshot of the current modification times and discards any
  // pending change of that root. Call it after every successful compile.
  void Watch(const std::string& root,
             const std::vector<std::string>& dependencies);

  // Returns the roots to recompile, in the order they were first watched.
  [[nodiscard]] std::vector<std::string> Poll();

 private:
  using Times = std::vector<std::optional<int64_t>>;

  struct WatchedRoot {
    std::string root;
    // The root first, then its dependencies.
    std::vector<std::string> files;
    // Modification times (parallel to `files`; nullopt = missing) at the last
    // Watch() and at the last Poll().
    Times snapshot;
    Times previous;
    // True once the change in `previous` has been returned from Poll().
    bool reported = false;
  };

  [[nodiscard]] static Times ReadTimes(const std::vector<std::string>& files);

  std::vector<WatchedRoot> roots_;
};

}  // namespace blue::shader

#endif  // BLUE_SHADER_SHADER_WATCHER_H_
