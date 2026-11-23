// The Agility SDK exports (design §4.1): an executable that creates a
// Direct3D 12 device exports the D3D12 runtime version it was built against
// and where its D3D12Core.dll is, relative to the executable, so every
// Windows 10 1909+ / 11 machine runs the same runtime (blue_add_d3d12_runtime()
// in src/render/d3d12/CMakeLists.txt compiles this file into the executable
// and copies D3D12Core.dll and d3d12SDKLayers.dll to <exe dir>/D3D12/).
// Without this the system's runtime is used.

#include <d3d12.h>

extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = D3D12_SDK_VERSION;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}
