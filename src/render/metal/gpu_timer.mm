#include "render/metal/gpu_timer.h"

#include <cstdint>
#include <optional>

#include "core/log.h"

namespace blue::render::metal {

namespace {

constexpr double kMsPerSecond = 1000.0;
constexpr NSUInteger kEntriesPerScope = 2;  // Begin and end.
constexpr NSUInteger kEntriesPerFrame =
    GpuTimer::kScopeCount * kEntriesPerScope;

}  // namespace

GpuTimer::GpuTimer(const MetalContext& context) {
  const id<MTLDevice> device = context.device();
  MTL4CounterHeapDescriptor* descriptor =
      [[MTL4CounterHeapDescriptor alloc] init];
  descriptor.type = MTL4CounterHeapTypeTimestamp;
  descriptor.count = kFramesInFlight * kEntriesPerFrame;
  NSError* error = nil;
  heap_ = [device newCounterHeapWithDescriptor:descriptor error:&error];
  ticks_per_second_ = static_cast<double>([device queryTimestampFrequency]);
  if (heap_ == nil || !(ticks_per_second_ > 0.0)) {
    core::log::Warn("GPU timestamp heap unavailable ({}), per-pass GPU times "
                    "unavailable",
                    error != nil ? error.localizedDescription.UTF8String
                                 : "no timestamp frequency");
    heap_ = nil;
    return;
  }
  heap_.label = @"gpu timer";
}

NSUInteger GpuTimer::EntryIndex(size_t frame, GpuScope scope) {
  return frame * kEntriesPerFrame +
         static_cast<size_t>(scope) * kEntriesPerScope;
}

void GpuTimer::Collect(size_t frame, const Submission* submission) {
  frame_ = frame;
  if (submission == nullptr) {
    return;  // Nothing was recorded into this frame slot yet.
  }
  last_ms_.fill(std::nullopt);
  const double total = submission->gpu_seconds();
  if (total > 0.0) {
    last_ms_[static_cast<size_t>(GpuScope::kTotal)] = total * kMsPerSecond;
  }
  ResolveScopes(frame);
  recorded_[frame].fill(false);
}

void GpuTimer::ResolveScopes(size_t frame) {
  if (heap_ == nil) {
    return;
  }
  const NSRange range = NSMakeRange(frame * kEntriesPerFrame, kEntriesPerFrame);
  NSData* data = [heap_ resolveCounterRange:range];
  if (data != nil &&
      data.length >= kEntriesPerFrame * sizeof(MTL4TimestampHeapEntry)) {
    const auto* entries = static_cast<const MTL4TimestampHeapEntry*>(data.bytes);
    for (size_t scope = 0; scope < kScopeCount; ++scope) {
      const uint64_t begin = entries[scope * kEntriesPerScope].timestamp;
      const uint64_t end = entries[scope * kEntriesPerScope + 1].timestamp;
      // An entry the GPU did not write resolves to 0.
      if (recorded_[frame][scope] && begin != 0 && end >= begin) {
        last_ms_[scope] = static_cast<double>(end - begin) /
                          ticks_per_second_ * kMsPerSecond;
      }
    }
  }
  // So that a scope the slot's next frame skips does not read stale values.
  [heap_ invalidateCounterRange:range];
}

void GpuTimer::BeginScope(id<MTL4CommandBuffer> cmd, GpuScope scope) {
  open_scope_ = scope;
  if (heap_ != nil) {
    [cmd writeTimestampIntoHeap:heap_ atIndex:EntryIndex(frame_, scope)];
    recorded_[frame_][static_cast<size_t>(scope)] = true;
  }
}

void GpuTimer::EndScope(id<MTL4CommandBuffer> cmd, GpuScope scope) {
  if (open_scope_ != scope) {
    core::log::Fatal("GpuTimer::EndScope({}) without its BeginScope()",
                     GpuScopeName(scope));
  }
  open_scope_.reset();
  if (heap_ != nil) {
    [cmd writeTimestampIntoHeap:heap_ atIndex:EntryIndex(frame_, scope) + 1];
  }
}

}  // namespace blue::render::metal
