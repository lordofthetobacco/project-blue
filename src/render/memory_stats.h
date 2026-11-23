#ifndef BLUE_RENDER_MEMORY_STATS_H_
#define BLUE_RENDER_MEMORY_STATS_H_

#include <array>
#include <cstdint>

namespace blue::render {

// The most memory heaps MemoryStats can describe. A Metal device has one
// pool of GPU memory, so the renderer reports exactly one heap.
inline constexpr uint32_t kMaxMemoryHeaps = 4;

// GPU memory of one heap of the device. Metal has no memory heaps or
// allocator blocks to inspect, only two numbers per device: what the process
// has allocated on it and how much the system recommends it stays below.
struct HeapMemoryStats {
  // MTLDevice.recommendedMaxWorkingSetSize: there is no fixed heap size (on
  // Apple silicon the GPU shares the system's memory).
  uint64_t heap_size = 0;
  bool device_local = false;  // GPU memory; always true for Metal's one heap.
  // What Metal has allocated for the process on the device
  // (MTLDevice.currentAllocatedSize: every texture, buffer and heap, the
  // drawables and ImGui's objects included), and how much of it the objects
  // the renderer accounts for by owner (below) use; the difference is what
  // nobody accounts for.
  uint64_t block_bytes = 0;
  uint64_t allocation_bytes = 0;
  // Always 0: Metal reports neither blocks nor a number of allocations.
  uint32_t block_count = 0;
  uint32_t allocation_count = 0;
  // The process's usage of the device (block_bytes again) and the size the
  // system recommends it stays below (heap_size again); both come from the
  // driver (MemoryStats::driver_budget).
  uint64_t usage = 0;
  uint64_t budget = 0;
};

// What the renderer holds in memory (Renderer::memory_stats()). Cheap: no
// allocation, no walk over allocations; fine to call every frame.
struct MemoryStats {
  std::array<HeapMemoryStats, kMaxMemoryHeaps> heaps{};
  uint32_t heap_count = 0;
  // Whether usage/budget come from the driver; always true with Metal.
  bool driver_budget = false;

  // GPU memory by owner, in bytes (MTLTexture.allocatedSize, MTLBuffer
  // length).
  uint64_t texture_bytes = 0;  // Uploaded textures (defaults excluded).
  uint32_t texture_count = 0;
  uint64_t mesh_bytes = 0;  // Resident vertex and index buffers.
  uint32_t mesh_count = 0;
  // Window- or viewport-sized targets (HDR, depth, SSAO, bloom, LDR, the
  // motion blur images, the editor viewport image), the shadow map and a
  // scope's picture and depth.
  uint64_t render_target_bytes = 0;
  // Environment cubes (sky, specular source, prefiltered) and the BRDF LUT.
  uint64_t environment_bytes = 0;
  // Released meshes and textures still waiting for the frames in flight.
  uint64_t retired_bytes = 0;

  // Upload staging: the fixed per-upload budget, the most staging memory
  // allocated at once so far, and what is allocated right now (0 between
  // uploads).
  uint64_t staging_budget_bytes = 0;
  uint64_t staging_peak_bytes = 0;
  uint64_t staging_current_bytes = 0;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_MEMORY_STATS_H_
