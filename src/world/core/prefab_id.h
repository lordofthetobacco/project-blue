#ifndef BLUE_WORLD_CORE_PREFAB_ID_H_
#define BLUE_WORLD_CORE_PREFAB_ID_H_

#include <cstdint>

namespace blue::world {

// Handle of a prefab held by a PrefabSource (PrefabCache, PrefabStore).
// Stays valid for the source's lifetime; PrefabCache::Replace() keeps the id
// and swaps what it refers to. Ids are handed out in load order, so they are
// process-local: never send them over the network or store them.
enum class PrefabId : uint32_t {};

}  // namespace blue::world

#endif  // BLUE_WORLD_CORE_PREFAB_ID_H_
