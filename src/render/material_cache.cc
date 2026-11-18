#include "render/material_cache.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "core/log.h"
#include "material/material.h"
#include "material/material_file.h"
#include "platform/file_system.h"

namespace blue::render {

std::optional<MaterialId> MaterialCache::Get(std::string_view path) {
  if (const auto found = by_path_.find(path); found != by_path_.end()) {
    return *found->second;
  }
  return *Entry(path);
}

std::optional<MaterialId> MaterialCache::Reload(std::string_view path) {
  std::optional<MaterialId>* entry = nullptr;
  if (const auto found = by_path_.find(path); found != by_path_.end()) {
    entry = found->second;
  } else {
    const bool known = by_key_.contains(platform::AbsolutePath(path));
    entry = Entry(path);
    if (!known) {
      return *entry;  // Just loaded for the first time.
    }
  }
  const std::optional<MaterialId> loaded = Load(path, *entry);
  if (loaded) {
    *entry = loaded;
  }
  return loaded;
}

std::optional<MaterialId>* MaterialCache::Entry(std::string_view path) {
  std::string key = platform::AbsolutePath(path);
  auto entry = by_key_.find(key);
  if (entry == by_key_.end()) {
    const std::optional<MaterialId> loaded = Load(path, std::nullopt);
    entry = by_key_.emplace(std::move(key), loaded).first;
  }
  std::optional<MaterialId>* result = &entry->second;
  by_path_.emplace(std::string(path), result);
  return result;
}

std::optional<MaterialId> MaterialCache::Load(
    std::string_view path, std::optional<MaterialId> existing) {
  auto loaded = material::LoadMaterialFile(std::string(path));
  if (!loaded.ok()) {
    core::log::Warn("Cannot load material file '{}': {}", path,
                    material::ToString(loaded.error()));
    return std::nullopt;
  }
  const material::MaterialDesc& desc = loaded.value();
  if (existing) {
    renderer_.UpdateMaterial(*existing, desc);
    return existing;
  }
  renderer_.PreloadTextures({&desc, 1});
  return renderer_.CreateMaterial(desc);
}

}  // namespace blue::render
