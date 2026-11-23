#ifndef BLUE_RENDER_D3D12_D3D12_CHECK_H_
#define BLUE_RENDER_D3D12_D3D12_CHECK_H_

#include <d3d12.h>
#include <windows.h>

#include <source_location>
#include <string>

namespace blue::render::d3d12 {

// Returns normally when `result` succeeded (SUCCEEDED()). Otherwise logs the
// HRESULT (hex and the system's text for it) with the file, line and
// function of the call site, then terminates the process through
// core::log::Fatal. A device-removed result also logs the removal reason
// (see SetDeviceForRemovedReason()). Wrap every Direct3D / DXGI call that
// returns an HRESULT and cannot be recovered from.
//
// `location` defaults to std::source_location::current(), which is evaluated
// at the call site, so callers never pass it.
void CheckHr(HRESULT result,
             std::source_location location = std::source_location::current());

// Returns "0x887A0005 (The GPU device instance has been suspended...)".
[[nodiscard]] std::string HrToString(HRESULT result);

// The device whose GetDeviceRemovedReason() CheckHr() logs when a call
// reports DXGI_ERROR_DEVICE_REMOVED / _RESET / _HUNG; null: none. Set by
// D3d12Context for the lifetime of its device.
void SetDeviceForRemovedReason(ID3D12Device* device);

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_D3D12_CHECK_H_
