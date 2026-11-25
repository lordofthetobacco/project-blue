#ifndef BLUE_WORLD_CORE_PREFAB_STORE_H_
#define BLUE_WORLD_CORE_PREFAB_STORE_H_

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "core/result.h"
#include "prefab/prefab.h"
#include "prefab/prefab_file.h"
#include "world/core/prefab_id.h"
#include "world/core/prefab_source.h"

namespace blue::world {

// The headless PrefabSource: loads .pbo files once (prefab::LoadPrefab) and
// keeps their CPU data, nothing is uploaded anywhere. Keys like
// PrefabCache's (platform::AbsolutePath()), and ids are handed out in the
// same order for the same sequence of Get() calls, so a world resolved
// through a store matches one resolved through a cache.
//
// Not thread-safe; neither copyable nor movable.
class PrefabStore final : public PrefabSource {
 public:
  PrefabStore() = default;

  // Returns the prefab at `path`, loading it on first use (a failure is
  // logged and not remembered: the next call tries again).
  [[nodiscard]] core::Result<PrefabId, prefab::PrefabError> Get(
      std::string_view path) override;

  // Returns the id `path` is stored under, without loading.
  [[nodiscard]] std::optional<PrefabId> Find(std::string_view path) const;

  // Stores an in-memory prefab under `key` (a path, normalised like Get()'s,
  // or any name; nested prefabs that reference it by that path then use this
  // copy). An already stored key is replaced, keeping its id.
  PrefabId Add(prefab::Prefab prefab, std::string_view key);

  [[nodiscard]] const prefab::Prefab& prefab(PrefabId id) const override {
    return entries_[Index(id)].prefab;
  }
  [[nodiscard]] const std::string& key(PrefabId id) const override {
    return entries_[Index(id)].key;
  }
  [[nodiscard]] size_t size() const { return entries_.size(); }

 private:
  struct Entry {
    std::string key;
    prefab::Prefab prefab;
  };

  [[nodiscard]] static size_t Index(PrefabId id) {
    return static_cast<size_t>(id);
  }

  // A deque: references to entries stay valid while new prefabs load.
  std::deque<Entry> entries_;
  std::unordered_map<std::string, PrefabId> by_key_;
};

}  // namespace blue::world

#endif  // BLUE_WORLD_CORE_PREFAB_STORE_H_
