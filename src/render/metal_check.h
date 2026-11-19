#ifndef BLUE_RENDER_METAL_CHECK_H_
#define BLUE_RENDER_METAL_CHECK_H_

#include "render/metal_types.h"

#include <string_view>

namespace blue::render {

// Returns normally when `object` is not nil. Otherwise logs `what` (e.g. the
// name of the creating method) with the text of `error`, which may be nil,
// and terminates the process through core::log::Fatal. Wrap the result of
// every device-object creation that cannot be recovered from.
void FatalIfNil(id object, NSError* error, std::string_view what);

// Returns normally unless `command_buffer`, which must have completed, ended
// with an error; then logs its label and error and terminates the process.
void CheckCommandBuffer(id<MTLCommandBuffer> command_buffer);

}  // namespace blue::render

#endif  // BLUE_RENDER_METAL_CHECK_H_
