#include "world/core/prefab_store.h"

#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "core/log.h"
#include "platform/file_system.h"
#include "prefab/prefab.h"
#include "prefab/prefab_file.h"

namespace blue::world {
namespace {

using GetResult = core::Result<PrefabId, prefab::PrefabError>;

// Returns the store key of `path`: an absolute, normalised path (as
// PrefabCache keys it).
std::string KeyOf(std::string_view path) {
  return platform::AbsolutePath(path);
}

}  // namespace

GetResult PrefabStore::Get(std::string_view path) {
  std::string key = KeyOf(path);
  if (auto found = by_key_.find(key); found != by_key_.end()) {
    return GetResult::Ok(found->second);
  }
  auto loaded = prefab::LoadPrefab(key);
  if (!loaded.ok()) {
    core::log::Warn("Prefab {}: {}", key, prefab::ToString(loaded.error()));
    return GetResult::Error(loaded.error());
  }
  return GetResult::Ok(Add(std::move(loaded).value(), key));
}

std::optional<PrefabId> PrefabStore::Find(std::string_view path) const {
  if (auto found = by_key_.find(KeyOf(path)); found != by_key_.end()) {
    return found->second;
  }
  if (auto found = by_key_.find(std::string(path)); found != by_key_.end()) {
    return found->second;
  }
  return std::nullopt;
}

PrefabId PrefabStore::Add(prefab::Prefab prefab, std::string_view key) {
  std::string stored_key =
      key.empty() ? std::format("<memory {}>", entries_.size()) : KeyOf(key);
  if (auto found = by_key_.find(stored_key); found != by_key_.end()) {
    entries_[Index(found->second)].prefab = std::move(prefab);
    return found->second;
  }
  const auto id = static_cast<PrefabId>(entries_.size());
  entries_.push_back({.key = stored_key, .prefab = std::move(prefab)});
  by_key_.emplace(std::move(stored_key), id);
  return id;
}

}  // namespace blue::world
