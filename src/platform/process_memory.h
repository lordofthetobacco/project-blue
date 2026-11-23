#ifndef BLUE_PLATFORM_PROCESS_MEMORY_H_
#define BLUE_PLATFORM_PROCESS_MEMORY_H_

#include <cstdint>

// Memory counters of the current process, as the OS reports them (Task
// Manager's "Commit size" is private_bytes, "Memory" is roughly
// working_set). On Windows, GPU allocations made through WDDM count towards
// private_bytes.

namespace blue::platform {

struct ProcessMemory {
  bool available = false;           // false where the OS query is unsupported
  uint64_t private_bytes = 0;       // committed private memory
  uint64_t working_set = 0;         // resident pages, including shared ones
  uint64_t peak_private_bytes = 0;  // highest private_bytes so far
  uint64_t peak_working_set = 0;    // highest working_set so far
};

// Returns the counters of the current process; `available` is false (and the
// numbers 0) on platforms without an implementation (everything but
// Windows). Cheap: one system call.
[[nodiscard]] ProcessMemory QueryProcessMemory();

}  // namespace blue::platform

#endif  // BLUE_PLATFORM_PROCESS_MEMORY_H_
