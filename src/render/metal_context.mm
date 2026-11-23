#include "render/metal_context.h"

#include <algorithm>
#include <utility>

#include "core/log.h"
#include "platform/window.h"
#include "render/recording.h"
#include "render/metal_check.h"
#include "shaders/bindings.h"

namespace blue::render {

namespace {

// Allocations the residency set has room for before it grows.
constexpr NSUInteger kResidencyCapacity = 1024;

}  // namespace

void Submission::Wait() const {
  std::unique_lock lock(mutex_);
  done_changed_.wait(lock, [this] { return done_; });
  CheckLocked();
}

bool Submission::completed() const {
  const std::scoped_lock lock(mutex_);
  if (done_) {
    CheckLocked();
  }
  return done_;
}

double Submission::gpu_seconds() const {
  const std::scoped_lock lock(mutex_);
  return gpu_seconds_;
}

void Submission::Complete(std::string error, double gpu_start,
                          double gpu_end) {
  {
    const std::scoped_lock lock(mutex_);
    done_ = true;
    error_ = std::move(error);
    gpu_seconds_ = std::max(gpu_end - gpu_start, 0.0);
  }
  done_changed_.notify_all();
}

void Submission::CheckLocked() const {
  if (!error_.empty()) {
    core::log::Fatal("Metal: GPU work '{}' failed: {}", label_, error_);
  }
}

struct MetalContext::Immediate {
  explicit Immediate(const MetalContext& context)
      : constants(context, "Immediate constants") {}

  id<MTL4CommandAllocator> allocator = nil;
  id<MTL4CommandBuffer> command_buffer = nil;
  TransientArena constants;
  bool recording = false;
};

MetalContext::MetalContext(const platform::Window& /*window*/) {
  device_ = MTLCreateSystemDefaultDevice();
  if (device_ == nil) {
    core::log::Fatal("No Metal device available");
  }
  device_name_ = device_.name.UTF8String;
  if (![device_ supportsFamily:MTLGPUFamilyMetal4]) {
    core::log::Fatal(
        "GPU {} does not support Metal 4, which the renderer requires "
        "(supportsFamily:MTLGPUFamilyMetal4; Apple silicon M1 / A14 or "
        "newer on macOS 26 or later)",
        device_name_);
  }
  if (!device_.supportsBCTextureCompression) {
    core::log::Fatal(
        "GPU {} does not support BC texture compression "
        "(supportsBCTextureCompression)",
        device_name_);
  }

  NSError* error = nil;
  MTL4CommandQueueDescriptor* queue_descriptor =
      [[MTL4CommandQueueDescriptor alloc] init];
  queue_descriptor.label = @"Main queue";
  queue_ = [device_ newMTL4CommandQueueWithDescriptor:queue_descriptor
                                                error:&error];
  FatalIfNil(queue_, error, "newMTL4CommandQueueWithDescriptor");

  MTL4CompilerDescriptor* compiler_descriptor =
      [[MTL4CompilerDescriptor alloc] init];
  compiler_descriptor.label = @"Pipeline compiler";
  compiler_ = [device_ newCompilerWithDescriptor:compiler_descriptor
                                           error:&error];
  FatalIfNil(compiler_, error, "newCompilerWithDescriptor");

  MTLResidencySetDescriptor* residency_descriptor =
      [[MTLResidencySetDescriptor alloc] init];
  residency_descriptor.label = @"Renderer resources";
  residency_descriptor.initialCapacity = kResidencyCapacity;
  residency_ = [device_ newResidencySetWithDescriptor:residency_descriptor
                                                error:&error];
  FatalIfNil(residency_, error, "newResidencySetWithDescriptor");
  [queue_ addResidencySet:residency_];

  // Sized for every index of shaders/bindings.h. Unused slots start as nil
  // bindings rather than undefined ones.
  MTL4ArgumentTableDescriptor* table_descriptor =
      [[MTL4ArgumentTableDescriptor alloc] init];
  table_descriptor.label = @"Renderer bindings";
  table_descriptor.maxBufferBindCount = kBufferBindCount;
  table_descriptor.maxTextureBindCount = kTextureBindCount;
  table_descriptor.maxSamplerStateBindCount = kSamplerBindCount;
  table_descriptor.initializeBindings = YES;
  argument_table_ = [device_ newArgumentTableWithDescriptor:table_descriptor
                                                      error:&error];
  FatalIfNil(argument_table_, error, "newArgumentTableWithDescriptor");

  immediate_ = std::make_unique<Immediate>(*this);
  immediate_->allocator = [device_ newCommandAllocator];
  FatalIfNil(immediate_->allocator, nil, "newCommandAllocator");
  immediate_->command_buffer = [device_ newCommandBuffer];
  FatalIfNil(immediate_->command_buffer, nil, "newCommandBuffer");
  immediate_->command_buffer.label = @"Immediate";

  core::log::Info("GPU: {} (Metal 4, {} memory, {} MiB working set)",
                  device_name_,
                  device_.hasUnifiedMemory ? "unified" : "discrete",
                  device_.recommendedMaxWorkingSetSize >> 20);
}

MetalContext::~MetalContext() {
  WaitIdle();
  // The immediate constants leave the residency set while it still exists.
  immediate_.reset();
  [queue_ removeResidencySet:residency_];
}

void MetalContext::AddResident(id<MTLAllocation> allocation) const {
  [residency_ addAllocation:allocation];
  residency_dirty_ = true;
}

void MetalContext::RemoveResident(id<MTLAllocation> allocation) const {
  [residency_ removeAllocation:allocation];
  residency_dirty_ = true;
}

void MetalContext::CommitResidency() const {
  if (residency_dirty_) {
    [residency_ commit];
    residency_dirty_ = false;
  }
}

void MetalContext::AddStagingBytes(uint64_t bytes) const {
  staging_usage_.current += bytes;
  staging_usage_.peak = std::max(staging_usage_.peak, staging_usage_.current);
}

void MetalContext::RemoveStagingBytes(uint64_t bytes) const {
  staging_usage_.current -= std::min(bytes, staging_usage_.current);
}

void MetalContext::SubmitImmediate(
    const std::function<void(const Recording&)>& record) const {
  Immediate& immediate = *immediate_;
  if (immediate.recording) {
    core::log::Fatal("MetalContext::SubmitImmediate() is not reentrant");
  }
  immediate.recording = true;
  // Drains the autoreleased objects of the recording (encoders, descriptors)
  // right away instead of whenever the caller's pool ends.
  @autoreleasepool {
    // The previous immediate work was waited for, so its memory is free.
    [immediate.allocator reset];
    immediate.constants.Reset();
    [immediate.command_buffer
        beginCommandBufferWithAllocator:immediate.allocator];
    record(Recording{.cmd = immediate.command_buffer,
                     .args = argument_table_,
                     .constants = immediate.constants});
    [immediate.command_buffer endCommandBuffer];
    Commit(immediate.command_buffer, "immediate")->Wait();
  }
  immediate.recording = false;
}

std::shared_ptr<const Submission> MetalContext::Commit(
    id<MTL4CommandBuffer> command_buffer, const char* label,
    id<MTLDrawable> drawable) const {
  CommitResidency();
  auto submission = std::make_shared<Submission>(label);
  MTL4CommitOptions* options = [[MTL4CommitOptions alloc] init];
  [options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
    NSError* error = feedback.error;
    submission->Complete(
        error != nil ? std::string(error.localizedDescription.UTF8String)
                     : std::string(),
        feedback.GPUStartTime, feedback.GPUEndTime);
  }];
  if (drawable != nil) {
    [queue_ waitForDrawable:drawable];
  }
  const id<MTL4CommandBuffer> buffers[] = {command_buffer};
  [queue_ commit:buffers count:1 options:options];
  if (drawable != nil) {
    [queue_ signalDrawable:drawable];
    [drawable present];
  }
  last_committed_ = submission;
  return submission;
}

Resident::Resident(const MetalContext& context, id<MTLAllocation> allocation)
    : context_(&context), allocation_(allocation) {
  context.AddResident(allocation);
}

Resident::~Resident() { Release(); }

Resident::Resident(Resident&& other) noexcept
    : context_(std::exchange(other.context_, nullptr)),
      allocation_(std::exchange(other.allocation_, nil)) {}

Resident& Resident::operator=(Resident&& other) noexcept {
  if (this != &other) {
    Release();
    context_ = std::exchange(other.context_, nullptr);
    allocation_ = std::exchange(other.allocation_, nil);
  }
  return *this;
}

void Resident::Release() {
  if (context_ != nullptr) {
    context_->RemoveResident(allocation_);
    context_ = nullptr;
    allocation_ = nil;
  }
}

void MetalContext::WaitIdle() const {
  if (last_committed_ != nullptr) {
    last_committed_->Wait();
    last_committed_.reset();
  }
}

}  // namespace blue::render
