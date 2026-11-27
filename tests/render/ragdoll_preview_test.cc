// Offscreen preview of the ragdoll of a killed player (game::Ragdolls drawing
// kyle.pbo through game::CharacterModel), through the real renderer: PNG
// strips at 0, 0.1, 0.25, 0.5, 1 and 2 s for a hit from the left and from the
// front. Not a golden test: skipped unless asked for, it needs the cooked
// sample assets (blue_sample_assets) and writes PNGs for a person to look at:
//
//   blue_render_tests -tc="*ragdoll*" --no-skip --out=<dir>
//
// Cases are added by the manager work package.

#include <doctest/doctest.h>

#include "game/ragdolls.h"

namespace blue::render_test {
namespace {}  // namespace
}  // namespace blue::render_test
