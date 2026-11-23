#ifndef BLUE_WEAPON_PARTS_WEAPON_ASSEMBLY_H_
#define BLUE_WEAPON_PARTS_WEAPON_ASSEMBLY_H_

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "core/result.h"
#include "prefab/prefab.h"
#include "weapon/parts/part_catalog.h"
#include "weapon/parts/weapon_build.h"
#include "weapon/weapon_def.h"

// Puts a weapon together from its parts: where every part's model goes, the
// weapon's calibration from the parts' functional sockets, its bounds, fire
// animation, spec and feel (out/briefs/G0-gunsmith-contract.md, "Prefab
// conventions").

namespace blue::weapon::parts {

// A part's model as the caller has it loaded.
struct PartModel {
  uint32_t key = 0;  // the caller's prefab handle (PartPlacement::prefab_key)
  // Must stay valid until AssembleWeapon returns, while it loads others.
  const prefab::Prefab* prefab = nullptr;
};

// Returns the model at `path` (PartDef::prefab, relative to the asset
// root), loading it if needed, or a message saying why it cannot.
using ModelSource =
    std::function<core::Result<PartModel, std::string>(std::string_view path)>;

// Returns the name the player sees for `build`: that of the first preset
// that builds exactly it, else of the first preset on the same root part,
// else the root part's. Empty for an empty build.
[[nodiscard]] std::string BuildName(const PartCatalog& catalog,
                                    const WeaponBuild& build);

// Returns the definition of `build` (not empty), its models from `models`:
//
// - Placements: the root, then each installed part with a model, depth
//   first in slot order. A part hangs on its parent's socket named
//   "slot.<slot name>" (ignoring case), on whatever node of the parent that
//   socket is on, so parts on animated nodes of the root follow the fire
//   animation, and so do parts mounted on those parts. Its matrix is the
//   socket's times the inverse of the part's own "mount" socket (its origin
//   without one). A part without a model is listed without a prefab and
//   cannot hold parts.
// - Geometry, in root space at rest, from the functional sockets of the
//   installed parts (weapon_prefab.h for what each socket gives):
//     muzzle:   a muzzle device, else a barrel, else any part (root first);
//     sight:    an optic, else a rear sight, else a slide, else any part;
//     lens, eyepiece: the part that gave the sight, if it has them;
//     grip:     the root, else any part.
//   Fallbacks when no part has the socket: muzzle = the sight's eye point;
//   sight = the root's origin looking at the muzzle (+Z when they meet);
//   grip = the root's origin. A degenerate sight rotation looks along +Z.
//   Nothing fails for a missing functional socket.
// - Bounds: every part's prefab bounds at rest, combined.
// - The fire animation of the root's prefab (ReadFireAnimation).
// - Spec and feel: TuneFromStats(ComputeStats()) named BuildName(); the
//   stats, their summaries, accuracy, zero range and switch time scale.
// - A `complete` root with a legacy_type is today's single-piece weapon:
//   MakeWeaponDef() of its prefab tagged `weapon = <legacy_type>`, exactly
//   (spec, feel, scope, no dispersion, no zero range), with the build, its
//   stats and their summaries added.
//
// Errors (with the part id): a root or a part holding a slot without a
// model, a model `models` cannot load, a slot socket missing on the parent,
// a fire clip without its events, an unknown legacy type.
[[nodiscard]] core::Result<WeaponDef, std::string> AssembleWeapon(
    const PartCatalog& catalog, const WeaponBuild& build,
    const ModelSource& models);

}  // namespace blue::weapon::parts

#endif  // BLUE_WEAPON_PARTS_WEAPON_ASSEMBLY_H_
