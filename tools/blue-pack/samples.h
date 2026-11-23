#ifndef BLUE_TOOLS_BLUE_PACK_SAMPLES_H_
#define BLUE_TOOLS_BLUE_PACK_SAMPLES_H_

#include <string>

// `blue-pack samples`: builds the game's sample content (prefabs and the
// default range scene) from the sources in the repository's samples/
// directory. The definitions live in samples.cc.

namespace blue::pack {

struct SamplesOptions {
  // The repository's samples/ directory.
  std::string source_dir;
  // Where the .pbo / .mscn files (and the scene's HDRI) are written.
  std::string out_dir;
  // Fast texture compression (iteration builds) instead of high quality.
  bool fast = false;
};

// Cooks every sample into options.out_dir:
//   m16.pbo       rifle, metres (scale baked), sidecar materials, sockets
//                 muzzle / sight / grip / lens (lens scale.x = lens radius);
//   glock17.pbo   pistol, root node "glock17" holding the scale, the
//                 duplicate merged node dropped, one clip "fire" with events
//                 "shot" (0.333 s) and "slide_lock" (0.4 s), sockets muzzle /
//                 sight / grip;
//   weapons/glock17/parts/*.pbo   the Glock 17 as part prefabs and the
//                 placeholder parts (see sample_parts.h);
//   weapons/catalog/base.cfg      the parts catalogue, written like
//                 range.mscn (an edited copy is kept, the fresh one goes to
//                 base.generated.cfg);
//   house.pbo     merged static mesh, static triangle-mesh collider;
//   ground.pbo    48 x 724 m plane with the Ground037 material, static box;
//   target.pbo    0.5 x 1.0 x 0.05 m steel plate, static box;
//   crate.pbo     1 m cube, dynamic 10 kg box;
//   sphere.pbo    1 m sphere, static sphere, grey material "sphere";
//   materials.pbo the ten preview materials, ground and steel (no meshes);
//   range.mscn    the default range scene, plus env/<hdri> it refers to;
//                 a range.mscn edited since it was generated is kept and
//                 the fresh scene written as range.generated.mscn instead
//                 (cook::WriteGeneratedFile(), sidecar
//                 range.mscn.fingerprint).
// Socket conventions: every socket hangs off the prefab root (node -1) in
// prefab space (metres); "sight" is placed at the sight's eye point and its
// rotation maps +Z onto the sight axis. Returns false (after logging what
// failed) if any asset cannot be built or written. Needs no window or GPU;
// creates its own physics::PhysicsRuntime.
[[nodiscard]] bool CookSamples(const SamplesOptions& options);

}  // namespace blue::pack

#endif  // BLUE_TOOLS_BLUE_PACK_SAMPLES_H_
