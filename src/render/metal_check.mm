#include "render/metal_check.h"

#include "core/log.h"

namespace blue::render {

namespace {

// Returns the UTF-8 text of `text`, or `fallback` when it is nil.
const char* Utf8Or(NSString* text, const char* fallback) {
  return text != nil ? text.UTF8String : fallback;
}

}  // namespace

void FatalIfNil(id object, NSError* error, std::string_view what) {
  if (object == nil) {
    core::log::Fatal("Metal: {} failed: {}", what,
                     Utf8Or(error.localizedDescription, "no error reported"));
  }
}

void CheckCommandBuffer(id<MTLCommandBuffer> command_buffer) {
  if (command_buffer.status == MTLCommandBufferStatusError) {
    core::log::Fatal(
        "Metal: command buffer '{}' failed: {}",
        Utf8Or(command_buffer.label, "unlabelled"),
        Utf8Or(command_buffer.error.localizedDescription, "unknown error"));
  }
}

}  // namespace blue::render
