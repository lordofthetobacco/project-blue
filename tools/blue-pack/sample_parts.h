#ifndef BLUE_TOOLS_BLUE_PACK_SAMPLE_PARTS_H_
#define BLUE_TOOLS_BLUE_PACK_SAMPLE_PARTS_H_

#include <string>
#include <vector>

#include "cook/model_cook.h"
#include "core/result.h"
#include "prefab/prefab.h"

// The sample weapon parts of `blue-pack samples`: the Glock 17 cut into part
// prefabs, generated placeholder parts and the base parts catalogue. See
// out/briefs/G0-gunsmith-contract.md for the conventions (tags, slot
// sockets, catalogue format).

namespace blue::pack {

// Tag every part prefab carries: part = <catalogue part id>.
inline constexpr const char* kPartTag = "part";

struct SamplePart {
  std::string path;  // relative to the asset root, e.g. weapons/.../frame.pbo
  prefab::Prefab prefab;
};

// Makes the Glock 17 part prefabs from the cooked sample pistol (the prefab
// glock17.pbo is saved from, in metres, bore along -X) under
// weapons/glock17/parts/. Every part is in the part frame of
// cook/prefab_split.h (+Y up, bore along +Z) and tagged part = <id>:
//   frame.pbo           skeleton + clip "fire"; meshes Frame, Trigger,
//                       Frame_Parts, Guide_Rod; sockets grip, slot.slide (on
//                       node Slide), slot.barrel (on node Barrel),
//                       slot.magazine, slot.tactical (on node Frame);
//   slide.pbo, slide_mos.pbo   origin: the slide's rear face on the bore
//                       axis; sight, slot.sight_rear, slot.sight_front;
//   barrel.pbo          origin: the barrel's rear face on the bore axis;
//                       muzzle;
//   barrel_threaded.pbo the barrel plus a placeholder thread stub; muzzle,
//                       slot.muzzle (the barrel's front face);
//   placeholders (plain material "placeholder"): suppressor.pbo,
//   compensator.pbo (mount, muzzle), mag_17/21/33.pbo (origin: top centre),
//   sight_rear.pbo (sight), sight_front.pbo (origin: base centre),
//   reddot.pbo (sight, lens; scale = lens radius).
// Every position is derived from the cooked meshes (node bounds and
// vertices) or from the measured weapon::Glock17Geometry(); see the .cc.
[[nodiscard]] core::Result<std::vector<SamplePart>, std::string> MakeGlockParts(
    const prefab::Prefab& pistol, const cook::ModelCookOptions& cook_options);

// Returns the text of weapons/catalog/base.cfg: the Glock parts above, the
// complete rifles m16 and spr_a3g and the presets glock17, m16, spr_a3g.
[[nodiscard]] std::string BaseCatalogText();

}  // namespace blue::pack

#endif  // BLUE_TOOLS_BLUE_PACK_SAMPLE_PARTS_H_
