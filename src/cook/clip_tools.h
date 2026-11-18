#ifndef BLUE_COOK_CLIP_TOOLS_H_
#define BLUE_COOK_CLIP_TOOLS_H_

#include <span>
#include <string>

#include "cook/cook_error.h"
#include "core/result.h"
#include "prefab/prefab.h"

// Editing of a prefab's animation clips, used by the editor's timeline and
// by blue-pack. Every function changes only prefab.clips and either succeeds
// completely or leaves the prefab unchanged. Clip indices refer to
// prefab.clips at the time of the call.

namespace blue::cook {

// Renames clip `clip`. Fails with kInvalidArgument for a bad index or an
// empty name.
[[nodiscard]] core::Result<void, CookError> RenameClip(prefab::Prefab& prefab,
                                                       int clip,
                                                       std::string name);

// Replaces the clips at `clips` (distinct indices, at least one) by one clip
// named `name` (animation::MergeClips: all channels and events, the longest
// duration) inserted at the position of the smallest index, and returns
// that index. Fails with kInvalidArgument for a bad or repeated index or an
// empty name.
[[nodiscard]] core::Result<int, CookError> MergeClips(
    prefab::Prefab& prefab, std::span<const int> clips, std::string name);

// Cuts clip `clip` down to [start, end] seconds and moves it to start at 0:
// keys inside the range are kept, a key holding the sampled value is added at
// each end of the range where none exists, keys outside are dropped, events
// outside the range are removed and the rest re-based. The duration becomes
// end - start. Linear and step channels stay exact; cubic-spline channels get
// boundary keys with zero tangents. Fails with kInvalidArgument for a bad
// index or unless 0 <= start < end (both finite).
[[nodiscard]] core::Result<void, CookError> TrimClip(prefab::Prefab& prefab,
                                                     int clip, float start,
                                                     float end);

// Sets whether clip `clip` loops. Fails with kInvalidArgument for a bad
// index.
[[nodiscard]] core::Result<void, CookError> SetClipLoop(prefab::Prefab& prefab,
                                                        int clip, bool loop);

// Adds event `name` at `time` seconds to clip `clip` (kept sorted by time)
// and returns its index in the clip's events. Fails with kInvalidArgument
// for a bad index, an empty name or a time outside [0, duration].
[[nodiscard]] core::Result<int, CookError> AddClipEvent(prefab::Prefab& prefab,
                                                        int clip,
                                                        std::string name,
                                                        float time);

// Removes event `event` of clip `clip`. Fails with kInvalidArgument for a bad
// index.
[[nodiscard]] core::Result<void, CookError> RemoveClipEvent(
    prefab::Prefab& prefab, int clip, int event);

}  // namespace blue::cook

#endif  // BLUE_COOK_CLIP_TOOLS_H_
