#include "render/gpu_scope.h"

namespace blue::render {

const char* GpuScopeName(GpuScope scope) {
  switch (scope) {
    case GpuScope::kTotal:
      return "frame";
    case GpuScope::kShadow:
      return "shadow";
    case GpuScope::kDepthPrepass:
      return "depth";
    case GpuScope::kSsao:
      return "ssao";
    case GpuScope::kScopeView:
      return "scope";
    case GpuScope::kScene:
      return "scene";
    case GpuScope::kMotionBlur:
      return "motion";
    case GpuScope::kBloom:
      return "bloom";
    case GpuScope::kTonemap:
      return "tonemap";
    case GpuScope::kOverlay:
      return "overlay";
    case GpuScope::kPresent:
      return "present";
    case GpuScope::kUi:
      return "ui";
    case GpuScope::kCount:
      break;
  }
  return "?";
}

}  // namespace blue::render
