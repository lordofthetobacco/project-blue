#include "render/d3d12/gpu_timer.h"

#include <d3d12.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "render/d3d12/buffer.h"
#include "render/d3d12/d3d12_check.h"
#include "render/d3d12/d3d12_context.h"
#include "render/frame_config.h"
#include "render/gpu_scope.h"

namespace blue::render::d3d12 {

namespace {

constexpr uint32_t kQueriesPerScope = 2;
constexpr uint32_t kQueriesPerFrame =
    static_cast<uint32_t>(GpuTimer::kScopeCount) * kQueriesPerScope;
constexpr uint32_t kQueryCount =
    static_cast<uint32_t>(kFramesInFlight) * kQueriesPerFrame;

uint32_t FirstFrameQuery(size_t frame) {
  return static_cast<uint32_t>(frame) * kQueriesPerFrame;
}

uint32_t FirstScopeQuery(size_t frame, GpuScope scope) {
  return FirstFrameQuery(frame) +
         static_cast<uint32_t>(scope) * kQueriesPerScope;
}

}  // namespace

GpuTimer::GpuTimer(const D3d12Context& context)
    : readback_(context, uint64_t{kQueryCount} * sizeof(uint64_t),
                Buffer::Access::kReadback, L"blue gpu timer readback"),
      ms_per_tick_(1000.0 / static_cast<double>(context.timestamp_frequency())) {
  const D3D12_QUERY_HEAP_DESC desc{
      .Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP,
      .Count = kQueryCount,
      .NodeMask = 0,
  };
  CheckHr(context.device()->CreateQueryHeap(&desc, IID_PPV_ARGS(&heap_)));
  heap_->SetName(L"blue gpu timer queries");
}

void GpuTimer::Collect(size_t frame) {
  std::array<bool, kScopeCount>& pending = pending_[frame];
  if (!pending[static_cast<size_t>(GpuScope::kTotal)]) {
    return;  // The frame was never recorded, or already collected.
  }
  const auto* ticks = static_cast<const uint64_t*>(readback_.mapped());
  for (size_t scope = 0; scope < kScopeCount; ++scope) {
    if (!pending[scope]) {
      last_ms_[scope] = std::nullopt;  // The scope did not run in this frame.
      continue;
    }
    const uint32_t first =
        FirstScopeQuery(frame, static_cast<GpuScope>(scope));
    const uint64_t begin = ticks[first];
    const uint64_t end = ticks[first + 1];
    last_ms_[scope] =
        static_cast<double>(end >= begin ? end - begin : 0) * ms_per_tick_;
    pending[scope] = false;
  }
}

void GpuTimer::Begin(ID3D12GraphicsCommandList* cmd, size_t frame,
                     GpuScope scope) {
  cmd->EndQuery(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                FirstScopeQuery(frame, scope));
}

void GpuTimer::End(ID3D12GraphicsCommandList* cmd, size_t frame,
                   GpuScope scope) {
  cmd->EndQuery(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                FirstScopeQuery(frame, scope) + 1);
  pending_[frame][static_cast<size_t>(scope)] = true;
}

void GpuTimer::EndFrame(ID3D12GraphicsCommandList* cmd, size_t frame) {
  // Only the scopes that ran (resolving a query never written is an error),
  // in runs of consecutive scopes.
  const std::array<bool, kScopeCount>& pending = pending_[frame];
  size_t scope = 0;
  while (scope < kScopeCount) {
    if (!pending[scope]) {
      ++scope;
      continue;
    }
    const size_t first = scope;
    while (scope < kScopeCount && pending[scope]) {
      ++scope;
    }
    const uint32_t query = FirstScopeQuery(frame, static_cast<GpuScope>(first));
    cmd->ResolveQueryData(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query,
                          static_cast<uint32_t>(scope - first) *
                              kQueriesPerScope,
                          readback_.resource(),
                          uint64_t{query} * sizeof(uint64_t));
  }
}

}  // namespace blue::render::d3d12
