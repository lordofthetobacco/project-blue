#ifndef BLUE_RENDER_MEMORY_STATS_H_
#define BLUE_RENDER_MEMORY_STATS_H_

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>

namespace blue::render {

// Device memory of one Vulkan memory heap as the renderer's allocator sees
// it (VMA's heap budget).
struct HeapMemoryStats {
  VkDeviceSize heap_size = 0;  // VkMemoryHeap::size.
  bool device_local = false;   // VRAM (or the shared pool of an iGPU).
  // VkDeviceMemory blocks the allocator holds in this heap, dedicated
  // allocations included, and how much of them live allocations use; the
  // difference is free space kept inside blocks.
  VkDeviceSize block_bytes = 0;
  VkDeviceSize allocation_bytes = 0;
  uint32_t block_count = 0;
  uint32_t allocation_count = 0;
  // The process's usage of the heap and the budget the OS grants it, from
  // VK_EXT_memory_budget when MemoryStats::driver_budget (all allocations of
  // the process, not only the renderer's); otherwise estimated by VMA
  // (usage = block_bytes, budget = 80% of the heap).
  VkDeviceSize usage = 0;
  VkDeviceSize budget = 0;
};

// What the renderer holds in memory (Renderer::memory_stats()). Cheap: no
// allocation, no walk over allocations; fine to call every frame.
struct MemoryStats {
  std::array<HeapMemoryStats, VK_MAX_MEMORY_HEAPS> heaps{};
  uint32_t heap_count = 0;
  // Whether usage/budget come from the driver (VK_EXT_memory_budget).
  bool driver_budget = false;

  // Device memory by owner, in bytes.
  VkDeviceSize texture_bytes = 0;  // Uploaded textures (defaults excluded).
  uint32_t texture_count = 0;
  VkDeviceSize mesh_bytes = 0;  // Resident vertex and index buffers.
  uint32_t mesh_count = 0;
  // Window- or viewport-sized targets (HDR, depth, SSAO, bloom, LDR, the
  // editor viewport image) and the shadow map.
  VkDeviceSize render_target_bytes = 0;
  // Environment cubes (sky, irradiance, prefiltered) and the BRDF LUT.
  VkDeviceSize environment_bytes = 0;
  // Released meshes and textures still waiting for the frames in flight.
  VkDeviceSize retired_bytes = 0;

  // Upload staging: the fixed per-upload budget, the largest staging buffer
  // created so far, and what is allocated right now (0 between uploads).
  VkDeviceSize staging_budget_bytes = 0;
  VkDeviceSize staging_peak_bytes = 0;
  VkDeviceSize staging_current_bytes = 0;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_MEMORY_STATS_H_
