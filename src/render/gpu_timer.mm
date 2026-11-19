#include "render/gpu_timer.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>

#include "core/log.h"

namespace blue::render {

namespace {

constexpr double kMsPerSecond = 1000.0;
constexpr double kMsPerNanosecond = 1.0e-6;
constexpr NSUInteger kSamplesPerEncoder = 2;  // Start and end.
constexpr NSUInteger kSamplesPerFrame =
    GpuTimer::kMaxEncodersPerFrame * kSamplesPerEncoder;

// Returns the timestamp counter set if `device` can sample it at the
// boundaries of its encoders, else nil.
id<MTLCounterSet> FindTimestampCounterSet(id<MTLDevice> device) {
  if (![device
          supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) {
    return nil;
  }
  for (id<MTLCounterSet> set in device.counterSets) {
    if (![set.name isEqualToString:MTLCommonCounterSetTimestamp]) {
      continue;
    }
    for (id<MTLCounter> counter in set.counters) {
      if ([counter.name isEqualToString:MTLCommonCounterTimestamp]) {
        return set;
      }
    }
  }
  return nil;
}

// A resolved sample the GPU did not write (or wrote as an error) reads as
// MTLCounterErrorValue or 0.
bool IsValidSample(uint64_t timestamp) {
  return timestamp != MTLCounterErrorValue && timestamp != 0;
}

}  // namespace

GpuTimer::GpuTimer(const MetalContext& context) {
  id<MTLDevice> device = context.device();
  id<MTLCounterSet> counter_set = FindTimestampCounterSet(device);
  if (counter_set == nil) {
    core::log::Warn(
        "GPU timestamp counters not supported, per-pass GPU times "
        "unavailable");
    return;
  }
  MTLCounterSampleBufferDescriptor* descriptor =
      [[MTLCounterSampleBufferDescriptor alloc] init];
  descriptor.counterSet = counter_set;
  descriptor.storageMode = MTLStorageModeShared;
  descriptor.sampleCount = kFramesInFlight * kSamplesPerFrame;
  descriptor.label = @"gpu timer";
  NSError* error = nil;
  samples_ = [device newCounterSampleBufferWithDescriptor:descriptor
                                                    error:&error];
  if (samples_ == nil) {
    core::log::Warn("GPU timestamp sample buffer unavailable ({}), per-pass "
                    "GPU times unavailable",
                    error != nil ? error.localizedDescription.UTF8String
                                 : "unknown error");
  }
}

void GpuTimer::Collect(size_t frame, id<MTLCommandBuffer> command_buffer) {
  frame_ = frame;
  if (command_buffer == nil) {
    return;  // Nothing was recorded into this frame slot yet.
  }
  last_ms_.fill(std::nullopt);
  // Both times are 0 for a command buffer that failed or never ran.
  const CFTimeInterval start = command_buffer.GPUStartTime;
  const CFTimeInterval end = command_buffer.GPUEndTime;
  const bool completed =
      command_buffer.status == MTLCommandBufferStatusCompleted;
  if (completed && end > start) {
    last_ms_[static_cast<size_t>(GpuScope::kTotal)] =
        (end - start) * kMsPerSecond;
  }
  if (completed) {
    ResolveScopes(frame);
  }
  frames_[frame].encoder_count = 0;
}

void GpuTimer::ResolveScopes(size_t frame) {
  const FrameSamples& recorded = frames_[frame];
  if (samples_ == nil || recorded.encoder_count == 0) {
    return;
  }
  NSData* data = [samples_
      resolveCounterRange:NSMakeRange(frame * kSamplesPerFrame,
                                      recorded.encoder_count *
                                          kSamplesPerEncoder)];
  if (data == nil ||
      data.length < recorded.encoder_count * kSamplesPerEncoder *
                        sizeof(MTLCounterResultTimestamp)) {
    return;
  }
  const auto* timestamps =
      static_cast<const MTLCounterResultTimestamp*>(data.bytes);

  // Earliest start and latest end over each scope's encoders; a scope with
  // one encoder that has no valid sample pair cannot be measured.
  constexpr uint64_t kNone = std::numeric_limits<uint64_t>::max();
  std::array<uint64_t, kScopeCount> first_start;
  std::array<uint64_t, kScopeCount> last_end{};
  std::array<bool, kScopeCount> ran{};
  std::array<bool, kScopeCount> broken{};
  first_start.fill(kNone);
  for (size_t i = 0; i < recorded.encoder_count; ++i) {
    const auto scope = static_cast<size_t>(recorded.scopes[i]);
    const uint64_t begin = timestamps[i * kSamplesPerEncoder].timestamp;
    const uint64_t end = timestamps[i * kSamplesPerEncoder + 1].timestamp;
    ran[scope] = true;
    if (!IsValidSample(begin) || !IsValidSample(end) || end < begin) {
      broken[scope] = true;
      continue;
    }
    first_start[scope] = std::min(first_start[scope], begin);
    last_end[scope] = std::max(last_end[scope], end);
  }
  for (size_t scope = 0; scope < kScopeCount; ++scope) {
    if (ran[scope] && !broken[scope]) {
      // Apple GPUs count MTLCommonCounterTimestamp in nanoseconds (it
      // advanced by the CPU nanoseconds elapsed when checked against
      // sampleTimestamps:gpuTimestamp:), so no conversion is needed.
      last_ms_[scope] =
          static_cast<double>(last_end[scope] - first_start[scope]) *
          kMsPerNanosecond;
    }
  }
}

void GpuTimer::BeginScope(GpuScope scope) { open_scope_ = scope; }

void GpuTimer::EndScope(GpuScope scope) {
  if (open_scope_ != scope) {
    core::log::Fatal("GpuTimer::EndScope({}) without its BeginScope()",
                     GpuScopeName(scope));
  }
  open_scope_.reset();
}

std::optional<NSUInteger> GpuTimer::ReserveSamples() {
  if (samples_ == nil || !open_scope_) {
    return std::nullopt;
  }
  FrameSamples& frame = frames_[frame_];
  if (frame.encoder_count == kMaxEncodersPerFrame) {
    if (!overflow_logged_) {
      core::log::Warn("More than {} encoders in a frame; the rest are not "
                      "timed",
                      kMaxEncodersPerFrame);
      overflow_logged_ = true;
    }
    return std::nullopt;
  }
  const size_t encoder = frame.encoder_count++;
  frame.scopes[encoder] = *open_scope_;
  return frame_ * kSamplesPerFrame + encoder * kSamplesPerEncoder;
}

void GpuTimer::AttachSamples(MTLRenderPassDescriptor* descriptor) {
  const std::optional<NSUInteger> first = ReserveSamples();
  if (!first) {
    return;
  }
  MTLRenderPassSampleBufferAttachmentDescriptor* attachment =
      descriptor.sampleBufferAttachments[0];
  attachment.sampleBuffer = samples_;
  // From the start of the fragment stage, not of the vertex stage: the GPU
  // starts an encoder's vertex stage while the previous encoder still runs,
  // and a pass that reads the previous pass's output waits for it only at
  // its fragment stage. Sampled at the vertex start, every cheap full-screen
  // pass reported the time it spent waiting for its predecessor (ui at
  // 3 ms of a 4.4 ms frame). The cost is that an encoder's vertex (tiling)
  // work is not counted.
  attachment.startOfVertexSampleIndex = MTLCounterDontSample;
  attachment.endOfVertexSampleIndex = MTLCounterDontSample;
  attachment.startOfFragmentSampleIndex = *first;
  attachment.endOfFragmentSampleIndex = *first + 1;
}

void GpuTimer::AttachSamples(MTLBlitPassDescriptor* descriptor) {
  const std::optional<NSUInteger> first = ReserveSamples();
  if (!first) {
    return;
  }
  MTLBlitPassSampleBufferAttachmentDescriptor* attachment =
      descriptor.sampleBufferAttachments[0];
  attachment.sampleBuffer = samples_;
  attachment.startOfEncoderSampleIndex = *first;
  attachment.endOfEncoderSampleIndex = *first + 1;
}

}  // namespace blue::render
