#include "render/d3d12/d3d12_check.h"

#include <d3d12.h>
#include <dxgi.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <format>
#include <source_location>
#include <string>

#include "core/log.h"

namespace blue::render::d3d12 {

namespace {

std::atomic<ID3D12Device*> removed_reason_device{nullptr};

bool IsDeviceLost(HRESULT result) {
  return result == DXGI_ERROR_DEVICE_REMOVED ||
         result == DXGI_ERROR_DEVICE_RESET ||
         result == DXGI_ERROR_DEVICE_HUNG;
}

}  // namespace

std::string HrToString(HRESULT result) {
  char* text = nullptr;
  const DWORD length = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, static_cast<DWORD>(result), 0, reinterpret_cast<char*>(&text),
      0, nullptr);
  std::string message;
  if (length > 0 && text != nullptr) {
    message.assign(text, length);
    while (!message.empty() &&
           (message.back() == '\n' || message.back() == '\r' ||
            message.back() == ' ' || message.back() == '.')) {
      message.pop_back();
    }
  }
  if (text != nullptr) {
    LocalFree(text);
  }
  const auto code = static_cast<uint32_t>(result);
  return message.empty() ? std::format("0x{:08X}", code)
                         : std::format("0x{:08X} ({})", code, message);
}

void SetDeviceForRemovedReason(ID3D12Device* device) {
  removed_reason_device.store(device);
}

void CheckHr(HRESULT result, std::source_location location) {
  if (SUCCEEDED(result)) {
    return;
  }
  std::string reason;
  ID3D12Device* device = removed_reason_device.load();
  if (IsDeviceLost(result) && device != nullptr) {
    reason = ", device removed: " + HrToString(device->GetDeviceRemovedReason());
  }
  core::log::Fatal("Direct3D 12 call failed: {}{} ({}:{} in {})",
                   HrToString(result), reason, location.file_name(),
                   location.line(), location.function_name());
}

}  // namespace blue::render::d3d12
