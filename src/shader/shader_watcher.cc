#include "shader/shader_watcher.h"

#include <algorithm>
#include <string>
#include <vector>

#include "platform/file_system.h"

namespace blue::shader {

ShaderWatcher::Times ShaderWatcher::ReadTimes(
    const std::vector<std::string>& files) {
  Times times;
  times.reserve(files.size());
  for (const std::string& file : files) {
    times.push_back(platform::LastWriteTime(file));
  }
  return times;
}

void ShaderWatcher::Watch(const std::string& root,
                          const std::vector<std::string>& dependencies) {
  auto it = std::find_if(
      roots_.begin(), roots_.end(),
      [&root](const WatchedRoot& watched) { return watched.root == root; });
  if (it == roots_.end()) {
    roots_.push_back({.root = root});
    it = roots_.end() - 1;
  }

  it->files.assign(1, root);
  for (const std::string& dependency : dependencies) {
    if (std::find(it->files.begin(), it->files.end(), dependency) ==
        it->files.end()) {
      it->files.push_back(dependency);
    }
  }
  it->snapshot = ReadTimes(it->files);
  it->previous = it->snapshot;
  it->reported = false;
}

std::vector<std::string> ShaderWatcher::Poll() {
  std::vector<std::string> changed;
  for (WatchedRoot& watched : roots_) {
    const Times current = ReadTimes(watched.files);
    if (current == watched.snapshot) {
      // Nothing differs from the compiled state (also after an edit was
      // undone).
      watched.previous = current;
      watched.reported = false;
      continue;
    }
    if (current != watched.previous) {
      // Still changing; wait for it to settle.
      watched.previous = current;
      watched.reported = false;
      continue;
    }
    const bool all_exist = std::all_of(
        current.begin(), current.end(),
        [](const std::optional<int64_t>& time) { return time.has_value(); });
    if (all_exist && !watched.reported) {
      watched.reported = true;
      changed.push_back(watched.root);
    }
  }
  return changed;
}

}  // namespace blue::shader
