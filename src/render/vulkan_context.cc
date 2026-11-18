#include "render/vulkan_context.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <vector>

#include "core/log.h"
#include "platform/window.h"
#include "render/vk_check.h"

namespace blue::render {

namespace {

#ifdef NDEBUG
constexpr bool kEnableValidation = false;
#else
constexpr bool kEnableValidation = true;
#endif

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr std::array kDeviceExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

// A physical device that can run the renderer, with its preference score.
struct DeviceCandidate {
  VkPhysicalDevice device = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties properties{};
  uint32_t queue_family = 0;
  int score = 0;
};

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* /*user_data*/) {
  if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    core::log::Error("[vulkan] {}", data->pMessage);
  } else {
    core::log::Warn("[vulkan] {}", data->pMessage);
  }
  return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT DebugMessengerInfo() {
  return {
      .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
      .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
      .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
      .pfnUserCallback = DebugCallback,
  };
}

bool IsLayerAvailable(const char* name) {
  uint32_t count = 0;
  vkEnumerateInstanceLayerProperties(&count, nullptr);
  std::vector<VkLayerProperties> layers(count);
  vkEnumerateInstanceLayerProperties(&count, layers.data());
  return std::ranges::any_of(layers, [name](const VkLayerProperties& layer) {
    return std::strcmp(layer.layerName, name) == 0;
  });
}

bool SupportsDeviceExtensions(VkPhysicalDevice device) {
  uint32_t count = 0;
  vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> available(count);
  vkEnumerateDeviceExtensionProperties(device, nullptr, &count,
                                       available.data());

  return std::ranges::all_of(kDeviceExtensions, [&](const char* required) {
    return std::ranges::any_of(
        available, [required](const VkExtensionProperties& extension) {
          return std::strcmp(extension.extensionName, required) == 0;
        });
  });
}

bool SupportsRequiredFeatures(VkPhysicalDevice device) {
  VkPhysicalDeviceVulkan13Features features13{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceFeatures2 features{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
      .pNext = &features13};
  vkGetPhysicalDeviceFeatures2(device, &features);
  // Depth clamp keeps shadow casters in front of a cascade's near plane.
  // Demote-to-helper (what discard compiles to for alpha-masked materials)
  // is mandatory in Vulkan 1.3, but checked like the rest. BC compression
  // is what cooked prefab textures (BC7, BC5) are stored in; every desktop
  // GPU has it.
  return features13.dynamicRendering && features13.synchronization2 &&
         features13.shaderDemoteToHelperInvocation &&
         features.features.depthClamp && features.features.textureCompressionBC;
}

// Returns a family that can both render and present; true for every desktop
// GPU we target.
std::optional<uint32_t> FindGraphicsPresentFamily(VkPhysicalDevice device,
                                                  VkSurfaceKHR surface) {
  uint32_t count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

  for (uint32_t i = 0; i < count; ++i) {
    VkBool32 can_present = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &can_present);
    if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && can_present) {
      return i;
    }
  }
  return std::nullopt;
}

// Returns how much a device of `type` is preferred: discrete over integrated
// over everything else.
int DeviceTypeScore(VkPhysicalDeviceType type) {
  switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
      return 2;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
      return 1;
    default:
      return 0;
  }
}

// Returns the candidate description of `device`, or nullopt if it cannot run
// the renderer (old API version, no graphics+present queue, missing
// extensions or features).
std::optional<DeviceCandidate> RateDevice(VkPhysicalDevice device,
                                          VkSurfaceKHR surface) {
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(device, &properties);

  const std::optional<uint32_t> family =
      FindGraphicsPresentFamily(device, surface);
  if (properties.apiVersion < VulkanContext::kApiVersion || !family ||
      !SupportsDeviceExtensions(device) || !SupportsRequiredFeatures(device)) {
    return std::nullopt;
  }
  return DeviceCandidate{.device = device,
                         .properties = properties,
                         .queue_family = *family,
                         .score = DeviceTypeScore(properties.deviceType)};
}

// Returns the number of valid timestamp bits of `queue_family` on `device`.
uint32_t QueryTimestampValidBits(VkPhysicalDevice device,
                                 uint32_t queue_family) {
  uint32_t count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
  return families[queue_family].timestampValidBits;
}

}  // namespace

VulkanContext::VulkanContext(const platform::Window& window) {
  CreateInstance(window);
  surface_ = window.CreateSurface(instance_);
  SelectPhysicalDevice();
  CreateDevice();
  CreateAllocator();

  const VkCommandPoolCreateInfo pool_info{
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
      .queueFamilyIndex = queue_family_,
  };
  CheckVk(vkCreateCommandPool(device_, &pool_info, nullptr, &immediate_pool_));
}

VulkanContext::~VulkanContext() {
  vkDestroyCommandPool(device_, immediate_pool_, nullptr);
  vmaDestroyAllocator(allocator_);
  vkDestroyDevice(device_, nullptr);
  vkDestroySurfaceKHR(instance_, surface_, nullptr);
  if (debug_messenger_ != VK_NULL_HANDLE) {
    // Extension entry points are only reachable as untyped function pointers.
    const auto destroy_messenger =
        reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_,
                                  "vkDestroyDebugUtilsMessengerEXT"));
    destroy_messenger(instance_, debug_messenger_, nullptr);
  }
  vkDestroyInstance(instance_, nullptr);
}

void VulkanContext::CreateInstance(const platform::Window& window) {
  uint32_t loader_version = 0;
  CheckVk(vkEnumerateInstanceVersion(&loader_version));
  if (loader_version < kApiVersion) {
    core::log::Fatal("Vulkan 1.3 is required, loader reports {}.{}",
                     VK_API_VERSION_MAJOR(loader_version),
                     VK_API_VERSION_MINOR(loader_version));
  }

  const std::span<const char* const> window_extensions =
      window.RequiredInstanceExtensions();
  std::vector<const char*> extensions(window_extensions.begin(),
                                      window_extensions.end());
  std::vector<const char*> layers;

  const bool validation =
      kEnableValidation && IsLayerAvailable(kValidationLayer);
  if (kEnableValidation && !validation) {
    core::log::Warn("{} not available, running without validation",
                    kValidationLayer);
  }
  if (validation) {
    layers.push_back(kValidationLayer);
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  }

  const VkApplicationInfo app_info{
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "Project Blue",
      .applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
      .pEngineName = "Blue",
      .engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
      .apiVersion = kApiVersion,
  };

  // Chained so that instance creation/destruction itself is also validated.
  const VkDebugUtilsMessengerCreateInfoEXT messenger_info =
      DebugMessengerInfo();

  const VkInstanceCreateInfo create_info{
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pNext = validation ? &messenger_info : nullptr,
      .pApplicationInfo = &app_info,
      .enabledLayerCount = static_cast<uint32_t>(layers.size()),
      .ppEnabledLayerNames = layers.data(),
      .enabledExtensionCount = static_cast<uint32_t>(extensions.size()),
      .ppEnabledExtensionNames = extensions.data(),
  };
  CheckVk(vkCreateInstance(&create_info, nullptr, &instance_));

  if (validation) {
    // Extension entry points are only reachable as untyped function pointers.
    const auto create_messenger =
        reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
    CheckVk(create_messenger(instance_, &messenger_info, nullptr,
                             &debug_messenger_));
  }
}

void VulkanContext::SelectPhysicalDevice() {
  uint32_t count = 0;
  CheckVk(vkEnumeratePhysicalDevices(instance_, &count, nullptr));
  std::vector<VkPhysicalDevice> devices(count);
  CheckVk(vkEnumeratePhysicalDevices(instance_, &count, devices.data()));

  // The first device with the highest score wins.
  std::optional<DeviceCandidate> best;
  for (VkPhysicalDevice device : devices) {
    const std::optional<DeviceCandidate> candidate =
        RateDevice(device, surface_);
    if (candidate && (!best || candidate->score > best->score)) {
      best = candidate;
    }
  }

  if (!best) {
    core::log::Fatal(
        "No GPU supports Vulkan 1.3 with dynamic rendering, synchronization2, "
        "depth clamp and BC texture compression (textureCompressionBC)");
  }
  physical_device_ = best->device;
  properties_ = best->properties;
  queue_family_ = best->queue_family;
  timestamp_valid_bits_ =
      QueryTimestampValidBits(physical_device_, queue_family_);
  VkPhysicalDeviceFeatures supported{};
  vkGetPhysicalDeviceFeatures(physical_device_, &supported);
  sampler_anisotropy_ = supported.samplerAnisotropy == VK_TRUE;

  core::log::Info(
      "GPU: {} (Vulkan {}.{}.{}, driver 0x{:x})", properties_.deviceName,
      VK_API_VERSION_MAJOR(properties_.apiVersion),
      VK_API_VERSION_MINOR(properties_.apiVersion),
      VK_API_VERSION_PATCH(properties_.apiVersion), properties_.driverVersion);
}

void VulkanContext::CreateDevice() {
  const float priority = 1.0f;
  const VkDeviceQueueCreateInfo queue_info{
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = queue_family_,
      .queueCount = 1,
      .pQueuePriorities = &priority,
  };

  VkPhysicalDeviceVulkan13Features features13{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .shaderDemoteToHelperInvocation = VK_TRUE,
      .synchronization2 = VK_TRUE,
      .dynamicRendering = VK_TRUE,
  };
  const VkPhysicalDeviceFeatures2 features{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
      .pNext = &features13,
      .features = {.depthClamp = VK_TRUE,
                   .samplerAnisotropy =
                       sampler_anisotropy_ ? VK_TRUE : VK_FALSE,
                   .textureCompressionBC = VK_TRUE},
  };

  const VkDeviceCreateInfo create_info{
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = &features,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
      .enabledExtensionCount = static_cast<uint32_t>(kDeviceExtensions.size()),
      .ppEnabledExtensionNames = kDeviceExtensions.data(),
  };
  CheckVk(vkCreateDevice(physical_device_, &create_info, nullptr, &device_));
  vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
}

void VulkanContext::CreateAllocator() {
  const VmaVulkanFunctions functions{
      .vkGetInstanceProcAddr = vkGetInstanceProcAddr,
      .vkGetDeviceProcAddr = vkGetDeviceProcAddr,
  };
  const VmaAllocatorCreateInfo create_info{
      .physicalDevice = physical_device_,
      .device = device_,
      .pVulkanFunctions = &functions,
      .instance = instance_,
      .vulkanApiVersion = kApiVersion,
  };
  CheckVk(vmaCreateAllocator(&create_info, &allocator_));
}

void VulkanContext::SubmitImmediate(
    const std::function<void(VkCommandBuffer)>& record) const {
  const VkCommandBufferAllocateInfo alloc_info{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = immediate_pool_,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
  };
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  CheckVk(vkAllocateCommandBuffers(device_, &alloc_info, &cmd));

  const VkCommandBufferBeginInfo begin_info{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  CheckVk(vkBeginCommandBuffer(cmd, &begin_info));
  record(cmd);
  CheckVk(vkEndCommandBuffer(cmd));

  const VkFenceCreateInfo fence_info{.sType =
                                         VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence = VK_NULL_HANDLE;
  CheckVk(vkCreateFence(device_, &fence_info, nullptr, &fence));

  const VkCommandBufferSubmitInfo cmd_info{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
      .commandBuffer = cmd,
  };
  const VkSubmitInfo2 submit_info{
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
      .commandBufferInfoCount = 1,
      .pCommandBufferInfos = &cmd_info,
  };
  CheckVk(vkQueueSubmit2(queue_, 1, &submit_info, fence));
  CheckVk(vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX));

  vkDestroyFence(device_, fence, nullptr);
  vkFreeCommandBuffers(device_, immediate_pool_, 1, &cmd);
}

void VulkanContext::WaitIdle() const { CheckVk(vkDeviceWaitIdle(device_)); }

}  // namespace blue::render
