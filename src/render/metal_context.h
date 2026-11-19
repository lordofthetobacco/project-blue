#ifndef BLUE_RENDER_METAL_CONTEXT_H_
#define BLUE_RENDER_METAL_CONTEXT_H_

#include "render/metal_types.h"

#include <functional>
#include <string>
#include <string_view>

#include "platform/window.h"

namespace blue::render {

// Core Metal objects shared by the whole renderer: the system default device
// and its single command queue. Startup is fatal unless the device supports
// the Metal 3 family and BC texture compression (cooked prefab textures are
// BC7 / BC5). There is no validation layer to enable in code: run with
// MTL_DEBUG_LAYER=1 (and MTL_SHADER_VALIDATION=1 for shaders) to get Metal's.
// Neither copyable nor movable. Not thread-safe.
class MetalContext {
 public:
  // `window` is not needed by Metal itself (the Swapchain takes its layer);
  // the parameter keeps the rule that the context is created for a window.
  explicit MetalContext(const platform::Window& window);

  MetalContext(const MetalContext&) = delete;
  MetalContext& operator=(const MetalContext&) = delete;

  [[nodiscard]] id<MTLDevice> device() const { return device_; }
  [[nodiscard]] id<MTLCommandQueue> queue() const { return queue_; }
  [[nodiscard]] std::string_view device_name() const { return device_name_; }

  // Records commands into a new command buffer with `record` and blocks until
  // the GPU has executed them; a GPU error is fatal. Intended for setup work
  // such as resource uploads, not per-frame use.
  void SubmitImmediate(
      const std::function<void(id<MTLCommandBuffer>)>& record) const;

  // Commits `command_buffer` and remembers it for WaitIdle().
  void Commit(id<MTLCommandBuffer> command_buffer) const;

  // Blocks until every command buffer committed through Commit() has
  // completed. One queue executes its command buffers in commit order, so
  // waiting for the last one is enough.
  void WaitIdle() const;

 private:
  id<MTLDevice> device_ = nil;
  id<MTLCommandQueue> queue_ = nil;
  std::string device_name_;
  // Bookkeeping of the const Commit()/WaitIdle(), which callers holding a
  // const context (every GPU object does) must be able to use; it is not
  // part of the context's observable state.
  mutable id<MTLCommandBuffer> last_committed_ = nil;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_METAL_CONTEXT_H_
