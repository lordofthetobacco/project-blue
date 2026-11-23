#include "render/d3d12/buffer.h"

#include <D3D12MemAlloc.h>
#include <d3d12.h>

#include <cstdint>

#include "render/d3d12/d3d12_check.h"
#include "render/d3d12/d3d12_context.h"

namespace blue::render::d3d12 {

Buffer::Buffer(const D3d12Context& context, uint64_t size, Access access,
               const wchar_t* name)
    : size_(size) {
  D3D12MA::ALLOCATION_DESC allocation_desc{};
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
  switch (access) {
    case Access::kGpuOnly:
      allocation_desc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
      break;
    case Access::kUpload:
      allocation_desc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
      state = D3D12_RESOURCE_STATE_GENERIC_READ;
      break;
    case Access::kReadback:
      allocation_desc.HeapType = D3D12_HEAP_TYPE_READBACK;
      state = D3D12_RESOURCE_STATE_COPY_DEST;
      break;
  }
  const D3D12_RESOURCE_DESC desc{
      .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
      .Alignment = 0,
      .Width = size,
      .Height = 1,
      .DepthOrArraySize = 1,
      .MipLevels = 1,
      .Format = DXGI_FORMAT_UNKNOWN,
      .SampleDesc = {1, 0},
      .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
      .Flags = D3D12_RESOURCE_FLAG_NONE,
  };
  CheckHr(context.allocator()->CreateResource(&allocation_desc, &desc, state,
                                              nullptr, &allocation_,
                                              __uuidof(ID3D12Resource),
                                              nullptr));
  allocation_->GetResource()->SetName(name);
  if (access != Access::kGpuOnly) {
    // An empty read range: the CPU reads nothing it did not ask for.
    const D3D12_RANGE no_read{0, 0};
    CheckHr(allocation_->GetResource()->Map(
        0, access == Access::kReadback ? nullptr : &no_read, &mapped_));
  }
}

}  // namespace blue::render::d3d12
