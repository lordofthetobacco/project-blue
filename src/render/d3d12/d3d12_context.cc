#include "render/d3d12/d3d12_context.h"

#include <D3D12MemAlloc.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/log.h"
#include "render/backend.h"
#include "render/d3d12/d3d12_check.h"
#include "render/d3d12/descriptor_heaps.h"

namespace blue::render::d3d12 {

namespace {

constexpr D3D_FEATURE_LEVEL kMinFeatureLevel = D3D_FEATURE_LEVEL_12_0;

std::string ToUtf8(const wchar_t* text) {
  const int size =
      WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) {
    return {};
  }
  std::string result(static_cast<size_t>(size - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr,
                      nullptr);
  return result;
}

// Asks `device` about `feature`: through CheckFeatureSupport1() with the
// Agility SDK version the backend was built against when the runtime has it
// (capabilities clamped to that version), else CheckFeatureSupport().
template <typename T>
bool CheckFeature(ID3D12Device* device, D3D12_FEATURE feature, T& data) {
  ComPtr<ID3D12Device16> device16;
  if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device16)))) {
    return SUCCEEDED(device16->CheckFeatureSupport1(
        feature, &data, sizeof(data), D3D12_SDK_VERSION));
  }
  return SUCCEEDED(device->CheckFeatureSupport(feature, &data, sizeof(data)));
}

// Returns why `device` does not meet the backend's minimum, or an empty
// string when it does.
std::string MissingRequirement(ID3D12Device* device) {
  D3D12_FEATURE_DATA_SHADER_MODEL shader_model{D3D_SHADER_MODEL_6_0};
  if (!CheckFeature(device, D3D12_FEATURE_SHADER_MODEL, shader_model) ||
      shader_model.HighestShaderModel < D3D_SHADER_MODEL_6_0) {
    return "no Shader Model 6.0";
  }
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  if (!CheckFeature(device, D3D12_FEATURE_D3D12_OPTIONS, options) ||
      options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_2) {
    return "resource binding tier below 2";
  }
  // BC5/BC7 sampling is required at feature level 12_0; checked anyway.
  for (const DXGI_FORMAT format :
       {DXGI_FORMAT_BC7_UNORM_SRGB, DXGI_FORMAT_BC5_UNORM}) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format, {}, {}};
    if (!CheckFeature(device, D3D12_FEATURE_FORMAT_SUPPORT, support) ||
        (support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) == 0) {
      return "no BC5/BC7 sampling";
    }
  }
  return {};
}

// Returns the path of the loaded module `name`, or an empty string.
std::string ModulePath(const wchar_t* name) {
  const HMODULE module = GetModuleHandleW(name);
  if (module == nullptr) {
    return {};
  }
  std::array<wchar_t, MAX_PATH> path{};
  const DWORD length =
      GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
  return length == 0 ? std::string() : ToUtf8(path.data());
}

}  // namespace

D3d12Context::D3d12Context(AdapterPreference adapter, bool gpu_validation) {
#ifndef NDEBUG
  const bool debug_build = true;
#else
  const bool debug_build = false;
#endif
  const bool debug_layer = debug_build || gpu_validation;
  gpu_based_validation_ = gpu_validation;
  if (!CreateFactory(debug_layer)) {
    return;
  }
  if (debug_build) {
    // DRED: breadcrumbs and page fault data for device-removed reports.
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred)))) {
      dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
      dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    }
  }
  if (!CreateDevice(adapter)) {
    return;
  }
  SetDeviceForRemovedReason(device_.Get());
  if (debug_layer) {
    EnableInfoQueue();
  }
  CreateQueueAndAllocator();
  descriptors_ = std::make_unique<DescriptorHeaps>(device_.Get());

  const std::string core_path = ModulePath(L"D3D12Core.dll");
  const std::string warp_path =
      software_adapter_ ? ModulePath(L"d3d10warp.dll") : std::string();
  core::log::Info(
      "Direct3D 12 device: {}{}{} (runtime {}; debug layer {}{}; {} lines)",
      device_name_, software_adapter_ ? " (software) " : "", warp_path,
      core_path.empty() ? "unknown" : core_path, validation() ? "on" : "off",
      validation() && gpu_based_validation_ ? ", GPU-based validation" : "",
      narrow_lines_supported_ ? "narrow quadrilateral" : "aliased");
}

D3d12Context::~D3d12Context() {
  if (device_) {
    WaitIdle();
    PollValidationMessages();
  }
  SetDeviceForRemovedReason(nullptr);
  if (fence_event_ != nullptr) {
    CloseHandle(fence_event_);
  }
}

bool D3d12Context::CreateFactory(bool debug) {
  UINT flags = 0;
  if (debug) {
    ComPtr<ID3D12Debug1> debug_interface;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_interface)))) {
      debug_interface->EnableDebugLayer();
      if (gpu_based_validation_) {
        debug_interface->SetEnableGPUBasedValidation(TRUE);
      }
      flags |= DXGI_CREATE_FACTORY_DEBUG;
    } else {
      core::log::Warn(
          "Direct3D 12 debug layer not available "
          "(d3d12SDKLayers.dll missing); validation is off");
      gpu_based_validation_ = false;
    }
  }
  if (FAILED(CreateDXGIFactory2(flags, IID_PPV_ARGS(&factory_)))) {
    core::log::Warn("Direct3D 12: no DXGI factory");
    return false;
  }
  ComPtr<IDXGIFactory5> factory5;
  BOOL tearing = FALSE;
  if (SUCCEEDED(factory_.As(&factory5)) &&
      SUCCEEDED(factory5->CheckFeatureSupport(
          DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing)))) {
    tearing_supported_ = tearing == TRUE;
  }
  return true;
}

bool D3d12Context::CreateDevice(AdapterPreference preference) {
  std::vector<ComPtr<IDXGIAdapter1>> candidates;
  if (preference == AdapterPreference::kSoftware) {
    ComPtr<IDXGIAdapter1> warp;
    if (SUCCEEDED(factory_->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) {
      candidates.push_back(warp);
    }
  } else {
    ComPtr<IDXGIFactory6> factory6;
    CheckHr(factory_.As(&factory6));
    ComPtr<IDXGIAdapter1> candidate;
    for (UINT i = 0; factory6->EnumAdapterByGpuPreference(
                         i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                         IID_PPV_ARGS(&candidate)) != DXGI_ERROR_NOT_FOUND;
         ++i) {
      DXGI_ADAPTER_DESC1 desc{};
      candidate->GetDesc1(&desc);
      if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
        candidates.push_back(candidate);
      }
      candidate.Reset();
    }
  }

  for (const ComPtr<IDXGIAdapter1>& candidate : candidates) {
    DXGI_ADAPTER_DESC1 desc{};
    candidate->GetDesc1(&desc);
    const std::string name = ToUtf8(desc.Description);
    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(candidate.Get(), kMinFeatureLevel,
                                 IID_PPV_ARGS(&device)))) {
      core::log::Info("Direct3D 12: {} has no feature level 12_0", name);
      continue;
    }
    const std::string missing = MissingRequirement(device.Get());
    if (!missing.empty()) {
      core::log::Info("Direct3D 12: {} skipped: {}", name, missing);
      continue;
    }
    adapter_ = candidate;
    device_ = device;
    device_name_ = name;
    software_adapter_ = preference == AdapterPreference::kSoftware ||
                        (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    device_->SetName(L"blue device");
    D3D12_FEATURE_DATA_D3D12_OPTIONS19 options19{};
    narrow_lines_supported_ =
        CheckFeature(device_.Get(), D3D12_FEATURE_D3D12_OPTIONS19, options19) &&
        options19.RasterizerDesc2Supported &&
        options19.NarrowQuadrilateralLinesSupported;
    return true;
  }
  core::log::Warn(
      "Direct3D 12: no {} adapter with feature level 12_0, "
      "Shader Model 6.0 and resource binding tier 2",
      preference == AdapterPreference::kSoftware ? "software" : "hardware");
  return false;
}

void D3d12Context::EnableInfoQueue() {
  if (FAILED(device_.As(&info_queue_))) {
    info_queue_.Reset();
    return;
  }
  // Nothing below warnings is stored.
  std::array<D3D12_MESSAGE_SEVERITY, 1> denied_severities = {
      D3D12_MESSAGE_SEVERITY_INFO};
  D3D12_INFO_QUEUE_FILTER filter{};
  filter.DenyList.NumSeverities = static_cast<UINT>(denied_severities.size());
  filter.DenyList.pSeverityList = denied_severities.data();
  CheckHr(info_queue_->PushStorageFilter(&filter));
  info_queue_->SetMessageCountLimit(4096);
  if (IsDebuggerPresent()) {
    info_queue_->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
    info_queue_->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
  }
}

void D3d12Context::CreateQueueAndAllocator() {
  const D3D12_COMMAND_QUEUE_DESC queue_desc{
      .Type = D3D12_COMMAND_LIST_TYPE_DIRECT,
      .Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
      .Flags = D3D12_COMMAND_QUEUE_FLAG_NONE,
      .NodeMask = 0,
  };
  CheckHr(device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue_)));
  queue_->SetName(L"blue direct queue");
  CheckHr(
      device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)));
  fence_->SetName(L"blue queue fence");
  fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (fence_event_ == nullptr) {
    core::log::Fatal("CreateEvent failed");
  }
  CheckHr(queue_->GetTimestampFrequency(&timestamp_frequency_));

  D3D12MA::ALLOCATOR_DESC allocator_desc{};
  allocator_desc.Flags = D3D12MA::ALLOCATOR_FLAG_NONE;
  allocator_desc.pDevice = device_.Get();
  allocator_desc.pAdapter = adapter_.Get();
  CheckHr(D3D12MA::CreateAllocator(&allocator_desc, &allocator_));

  CheckHr(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          IID_PPV_ARGS(&immediate_allocator_)));
  CheckHr(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                     immediate_allocator_.Get(), nullptr,
                                     IID_PPV_ARGS(&immediate_list_)));
  immediate_list_->SetName(L"blue immediate commands");
  CheckHr(immediate_list_->Close());
}

D3d12Context::ValidationCounts D3d12Context::validation_counts() const {
  PollValidationMessages();
  return validation_counts_;
}

void D3d12Context::PollValidationMessages() const {
  if (!info_queue_) {
    return;
  }
  const UINT64 count = info_queue_->GetNumStoredMessages();
  std::vector<std::byte> storage;
  for (UINT64 i = 0; i < count; ++i) {
    SIZE_T size = 0;
    if (FAILED(info_queue_->GetMessage(i, nullptr, &size)) || size == 0) {
      continue;
    }
    storage.resize(size);
    auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
    if (FAILED(info_queue_->GetMessage(i, message, &size))) {
      continue;
    }
    const std::string_view text(message->pDescription,
                                message->DescriptionByteLength > 0
                                    ? message->DescriptionByteLength - 1
                                    : 0);
    switch (message->Severity) {
      case D3D12_MESSAGE_SEVERITY_CORRUPTION:
      case D3D12_MESSAGE_SEVERITY_ERROR:
        ++validation_counts_.errors;
        core::log::Error("D3D12 debug layer: {}", text);
        break;
      case D3D12_MESSAGE_SEVERITY_WARNING:
        ++validation_counts_.warnings;
        core::log::Warn("D3D12 debug layer: {}", text);
        break;
      default:
        break;
    }
  }
  info_queue_->ClearStoredMessages();
}

void D3d12Context::AddStagingBytes(uint64_t bytes) const {
  staging_usage_.current += bytes;
  staging_usage_.peak = std::max(staging_usage_.peak, staging_usage_.current);
}

void D3d12Context::RemoveStagingBytes(uint64_t bytes) const {
  staging_usage_.current -= std::min(bytes, staging_usage_.current);
}

uint64_t D3d12Context::Execute(ID3D12CommandList* list) const {
  queue_->ExecuteCommandLists(1, &list);
  const uint64_t value = next_fence_value_++;
  CheckHr(queue_->Signal(fence_.Get(), value));
  return value;
}

bool D3d12Context::Reached(uint64_t value) const {
  return fence_->GetCompletedValue() >= value;
}

void D3d12Context::WaitFor(uint64_t value) const {
  if (Reached(value)) {
    return;
  }
  CheckHr(fence_->SetEventOnCompletion(value, fence_event_));
  WaitForSingleObject(fence_event_, INFINITE);
  // A removed device signals the fence with UINT64_MAX: report why.
  if (fence_->GetCompletedValue() == UINT64_MAX) {
    CheckHr(device_->GetDeviceRemovedReason());
  }
}

void D3d12Context::WaitIdle() const {
  const uint64_t value = next_fence_value_++;
  CheckHr(queue_->Signal(fence_.Get(), value));
  WaitFor(value);
}

void D3d12Context::SubmitImmediate(
    const std::function<void(ID3D12GraphicsCommandList*)>& record) const {
  CheckHr(immediate_allocator_->Reset());
  CheckHr(immediate_list_->Reset(immediate_allocator_.Get(), nullptr));
  descriptors_->Bind(immediate_list_.Get());
  record(immediate_list_.Get());
  CheckHr(immediate_list_->Close());
  WaitFor(Execute(immediate_list_.Get()));
  PollValidationMessages();
}

}  // namespace blue::render::d3d12
