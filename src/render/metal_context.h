#ifndef BLUE_RENDER_METAL_CONTEXT_H_
#define BLUE_RENDER_METAL_CONTEXT_H_

#include "render/metal_types.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "platform/window.h"

namespace blue::render {

struct Recording;

// The completion of one commit to the queue: Metal 4 reports it through the
// commit's feedback handler, on a queue of its own, instead of a command
// buffer status. Created by MetalContext::Commit() and shared with the
// feedback handler. Thread-safe.
class Submission {
 public:
  explicit Submission(std::string label) : label_(std::move(label)) {}

  Submission(const Submission&) = delete;
  Submission& operator=(const Submission&) = delete;

  // Blocks until the GPU has finished the committed work. A GPU error in it
  // is fatal (logged with the label).
  void Wait() const;
  // Whether the GPU has finished the work; never blocks. A GPU error is
  // fatal here too.
  [[nodiscard]] bool completed() const;
  // The time the GPU spent on the work, in seconds: 0 until completed().
  [[nodiscard]] double gpu_seconds() const;

  // Called once by the feedback handler. `error` is empty on success.
  void Complete(std::string error, double gpu_start, double gpu_end);

 private:
  // Terminates the process when the work failed. Needs mutex_ held.
  void CheckLocked() const;

  const std::string label_;
  mutable std::mutex mutex_;
  mutable std::condition_variable done_changed_;
  bool done_ = false;
  std::string error_;
  double gpu_seconds_ = 0.0;
};

// Core Metal 4 objects shared by the whole renderer: the system default
// device, its single MTL4CommandQueue, the MTL4Compiler that builds
// pipelines, the residency set and the argument table. Startup is fatal
// unless the device supports the Metal 4 family and BC texture compression
// (cooked prefab textures are BC7 / BC5). There is no validation layer to
// enable in code: run with MTL_DEBUG_LAYER=1 (and MTL_SHADER_VALIDATION=1
// for shaders) to get Metal's.
//
// Residency: Metal 4 only lets the GPU touch memory that a residency set of
// the queue (or of the command buffer) lists. The context owns one
// long-lived set, attached to the queue. Every Buffer and Image adds its
// allocation when it is created and removes it when it is destroyed
// (AddResident() / RemoveResident()); the changes are applied in bulk
// (-[MTLResidencySet commit]) before the next commit to the queue. The
// window's drawables are in the layer's own set, which the Swapchain
// attaches to the queue. Nothing else needs to be done per pass or per frame.
//
// Lifetime: a Metal 4 command buffer does not retain what its commands use.
// Whatever a committed command buffer references must be kept alive by its
// owner until that work has completed (Submission::Wait(), WaitIdle()).
//
// Neither copyable nor movable. Not thread-safe.
class MetalContext {
 public:
  // `window` is not needed by Metal itself (the Swapchain takes its layer);
  // the parameter keeps the rule that the context is created for a window.
  explicit MetalContext(const platform::Window& window);
  ~MetalContext();

  MetalContext(const MetalContext&) = delete;
  MetalContext& operator=(const MetalContext&) = delete;

  [[nodiscard]] id<MTLDevice> device() const { return device_; }
  [[nodiscard]] id<MTL4CommandQueue> queue() const { return queue_; }
  // Compiles pipeline states (render/pipeline.h).
  [[nodiscard]] id<MTL4Compiler> compiler() const { return compiler_; }
  // The one argument table every encoder binds through (render/encoders.h).
  [[nodiscard]] id<MTL4ArgumentTable> argument_table() const {
    return argument_table_;
  }
  [[nodiscard]] std::string_view device_name() const { return device_name_; }

  // Adds `allocation` (a buffer or texture) to the residency set / removes
  // it; applied before the next commit. An allocation must stay in the set
  // until the GPU no longer uses it. Buffer and Image call these.
  void AddResident(id<MTLAllocation> allocation) const;
  void RemoveResident(id<MTLAllocation> allocation) const;

  // Upload staging memory allocated right now and its high-water mark, in
  // bytes (see StagingUploader).
  struct StagingUsage {
    uint64_t current = 0;
    uint64_t peak = 0;
  };
  [[nodiscard]] StagingUsage staging_usage() const { return staging_usage_; }
  // Bookkeeping for StagingUploader: `bytes` of staging memory were created
  // (Add) or destroyed (Remove).
  void AddStagingBytes(uint64_t bytes) const;
  void RemoveStagingBytes(uint64_t bytes) const;

  // Records commands with `record` into the context's own command buffer and
  // blocks until the GPU has executed them; a GPU error is fatal. Intended
  // for setup work such as clears and filtering, not per-frame use. Not
  // reentrant.
  void SubmitImmediate(const std::function<void(const Recording&)>& record)
      const;

  // Commits `command_buffer`, whose recording has ended (endCommandBuffer),
  // after applying pending residency changes, and remembers it for
  // WaitIdle(). With a `drawable` the work is ordered after the display's
  // use of it (waitForDrawable:) and the drawable is presented once the work
  // is committed (signalDrawable: + present). `label` names the work in the
  // message of a GPU error. Returns its completion.
  std::shared_ptr<const Submission> Commit(
      id<MTL4CommandBuffer> command_buffer, const char* label,
      id<MTLDrawable> drawable = nil) const;

  // Blocks until everything committed through Commit() has completed. One
  // queue executes its command buffers in commit order, so waiting for the
  // last one is enough.
  void WaitIdle() const;

 private:
  // The command allocator, command buffer and constants of
  // SubmitImmediate().
  struct Immediate;

  // Applies AddResident() / RemoveResident() calls made since the last one.
  void CommitResidency() const;

  id<MTLDevice> device_ = nil;
  id<MTL4CommandQueue> queue_ = nil;
  id<MTL4Compiler> compiler_ = nil;
  id<MTLResidencySet> residency_ = nil;
  id<MTL4ArgumentTable> argument_table_ = nil;
  std::string device_name_;
  std::unique_ptr<Immediate> immediate_;
  // Bookkeeping of the const methods, which callers holding a const context
  // (every GPU object does) must be able to use; it is not part of the
  // context's observable state.
  mutable bool residency_dirty_ = false;
  mutable std::shared_ptr<const Submission> last_committed_;
  // Statistics only; uploads update them through a const context.
  mutable StagingUsage staging_usage_;
};

// Keeps one allocation (a buffer or a texture) in the residency set of a
// context for as long as it lives: what Buffer and Image hold next to their
// Metal object. Move-only; a moved-from or default-constructed one is empty.
// Not thread-safe.
class Resident {
 public:
  Resident() = default;
  // Borrows `context`, which must outlive this object.
  Resident(const MetalContext& context, id<MTLAllocation> allocation);
  ~Resident();

  Resident(const Resident&) = delete;
  Resident& operator=(const Resident&) = delete;
  Resident(Resident&& other) noexcept;
  Resident& operator=(Resident&& other) noexcept;

 private:
  void Release();

  const MetalContext* context_ = nullptr;
  id<MTLAllocation> allocation_ = nil;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_METAL_CONTEXT_H_
