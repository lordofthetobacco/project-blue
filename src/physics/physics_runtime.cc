#include "physics/physics_runtime.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>

// Jolt.h must be the first Jolt header included.
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/IssueReporting.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/RegisterTypes.h>
// clang-format on

#include "core/log.h"

namespace blue::physics {
namespace {

// True while a PhysicsRuntime exists. Constant-initialized and trivially
// destructible, so it is safe as static storage.
constinit std::atomic<bool> g_runtime_alive{false};

constexpr size_t kTraceBufferSize = 512;

// Jolt's trace hook. Its signature is a C variadic callback, so vsnprintf into
// a fixed buffer is the only way to format the message here.
void TraceHook(const char* format, ...) {
  char buffer[kTraceBufferSize];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  core::log::Info("[jolt] {}", static_cast<const char*>(buffer));
}

#ifdef JPH_ENABLE_ASSERTS
// Jolt's assert hook. A failed Jolt assertion is a programming error.
bool AssertHook(const char* expression, const char* message, const char* file,
                JPH::uint line) {
  core::log::Fatal("Jolt assertion failed: {} ({}) at {}:{}", expression,
                   message != nullptr ? message : "", file, line);
}
#endif

}  // namespace

PhysicsRuntime::PhysicsRuntime() {
  if (g_runtime_alive.exchange(true)) {
    core::log::Fatal("PhysicsRuntime: only one instance may exist at a time");
  }
  JPH::RegisterDefaultAllocator();
  JPH::Trace = TraceHook;
#ifdef JPH_ENABLE_ASSERTS
  JPH::AssertFailed = AssertHook;
#endif
  JPH::Factory::sInstance = new JPH::Factory();
  JPH::RegisterTypes();
}

PhysicsRuntime::~PhysicsRuntime() {
  JPH::UnregisterTypes();
  delete JPH::Factory::sInstance;
  JPH::Factory::sInstance = nullptr;
  g_runtime_alive.store(false);
}

}  // namespace blue::physics
