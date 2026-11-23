#ifndef BLUE_RENDER_D3D12_ROOT_SIGNATURE_H_
#define BLUE_RENDER_D3D12_ROOT_SIGNATURE_H_

#include <d3d12.h>
#include <wrl/client.h>

#include "render/d3d12/root_layout.h"

namespace blue::render::d3d12 {

// A root signature (version 1.1) made from a RootLayout: serialized and
// created once at startup. Root parameter i is layout().params[i]. Frame
// textures and pass tables are DATA_STATIC_WHILE_SET_AT_EXECUTE (written
// before recording, unchanged until the frame finished); descriptors are
// static (always initialized: unused full-screen inputs hold null views).
// Neither copyable nor movable.
class RootSignature {
 public:
  RootSignature(ID3D12Device* device, const RootLayout& layout,
                const wchar_t* name);

  RootSignature(const RootSignature&) = delete;
  RootSignature& operator=(const RootSignature&) = delete;

  [[nodiscard]] ID3D12RootSignature* get() const { return signature_.Get(); }
  [[nodiscard]] const RootLayout& layout() const { return layout_; }

 private:
  RootLayout layout_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> signature_;
};

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_ROOT_SIGNATURE_H_
