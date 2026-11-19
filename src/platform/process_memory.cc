#include "platform/process_memory.h"

#include <cstdint>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// psapi.h needs windows.h first.
#include <psapi.h>
#endif

namespace blue::platform {

#ifdef _WIN32

ProcessMemory QueryProcessMemory() {
  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof(counters);
  if (!GetProcessMemoryInfo(
          GetCurrentProcess(),
          reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
          sizeof(counters))) {
    return {};
  }
  return {.available = true,
          .private_bytes = counters.PrivateUsage,
          .working_set = counters.WorkingSetSize,
          .peak_private_bytes = counters.PeakPagefileUsage,
          .peak_working_set = counters.PeakWorkingSetSize};
}

#else

ProcessMemory QueryProcessMemory() { return {}; }

#endif

}  // namespace blue::platform
