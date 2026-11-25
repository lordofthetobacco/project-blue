#ifndef BLUE_WORLD_CORE_PREFAB_SOURCE_H_
#define BLUE_WORLD_CORE_PREFAB_SOURCE_H_

#include <string>
#include <string_view>

#include "core/result.h"
#include "prefab/prefab.h"
#include "prefab/prefab_file.h"
#include "world/core/prefab_id.h"

namespace blue::world {

// Where placement resolution (ResolvePlacements), collision building
// (CollisionWorld) and weapon building get prefabs from: the runtime's
// PrefabCache (CPU data plus GPU upload) or the headless PrefabStore (CPU
// data only). Both key prefabs by absolute path, so the same scene resolves
// to the same placements through either.
//
// An interface; implementations are neither copyable nor movable (callers
// keep references to them and to the prefabs they return).
class PrefabSource {
 public:
  PrefabSource(const PrefabSource&) = delete;
  PrefabSource& operator=(const PrefabSource&) = delete;
  virtual ~PrefabSource() = default;

  // Returns the prefab at `path`, loading it on first use.
  [[nodiscard]] virtual core::Result<PrefabId, prefab::PrefabError> Get(
      std::string_view path) = 0;

  // Returns the CPU data of `id`, which must come from this source. The
  // reference stays valid while other prefabs load.
  [[nodiscard]] virtual const prefab::Prefab& prefab(PrefabId id) const = 0;
  // Returns the key (absolute path, or an in-memory name) of `id`.
  [[nodiscard]] virtual const std::string& key(PrefabId id) const = 0;

 protected:
  PrefabSource() = default;
};

}  // namespace blue::world

#endif  // BLUE_WORLD_CORE_PREFAB_SOURCE_H_
