#pragma once

#include "render/Buffer.hpp"
#include "render/FrameConfig.hpp"
#include "render/GpuTimer.hpp"
#include "render/Image.hpp"
#include "render/Mesh.hpp"
#include "render/Pipeline.hpp"
#include "render/Swapchain.hpp"
#include "render/VulkanContext.hpp"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

struct ImDrawData;

namespace blue {

class Window;

struct Camera {
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    glm::vec3 position{0.0f};
};

struct PointLight {
    glm::vec3 position{0.0f};
    glm::vec3 color{1.0f};
    float intensity = 1.0f;
};

// Handle to a mesh owned by the renderer.
enum class MeshId : uint32_t {};

struct DrawItem {
    MeshId mesh;
    glm::mat4 transform;
};

struct RenderStats {
    uint32_t drawCalls = 0;
    uint64_t triangles = 0;
};

struct SceneView {
    Camera camera;
    PointLight light;
    glm::vec3 ambient{0.03f};
    std::span<const DrawItem> draws;
};

class Renderer {
public:
    static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

    Renderer(const Window& window, const std::filesystem::path& shaderDir);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Renders and presents one frame. `ui` is drawn on top when non-null.
    void drawFrame(const SceneView& scene, ImDrawData* ui);

    // Meshes stay resident for the whole session; unloading is out of scope.
    [[nodiscard]] MeshId uploadMesh(const MeshData& data);

    void requestSwapchainRebuild() { swapchainDirty_ = true; }
    void setVsync(bool enabled);

    [[nodiscard]] bool vsync() const { return vsync_; }
    [[nodiscard]] const VulkanContext& context() const { return context_; }
    [[nodiscard]] const Swapchain& swapchain() const { return swapchain_; }
    [[nodiscard]] std::optional<double> gpuFrameMs() const { return gpuTimer_.lastFrameMs(); }
    [[nodiscard]] RenderStats stats() const { return stats_; }

private:
    struct Frame {
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkFence inFlight = VK_NULL_HANDLE;
        VkSemaphore imageAcquired = VK_NULL_HANDLE;
        Buffer uniforms;
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    };

    void createFrames();
    [[nodiscard]] bool rebuildSwapchain();
    void recordCommands(const Frame& frame, uint32_t imageIndex, const SceneView& scene, ImDrawData* ui);

    const Window& window_;
    VulkanContext context_;
    bool vsync_ = true;
    bool swapchainDirty_ = false;
    Swapchain swapchain_;
    Image depth_;

    VkDescriptorSetLayout frameSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    std::array<Frame, kFramesInFlight> frames_;
    size_t frameIndex_ = 0;

    Pipeline meshPipeline_;
    std::vector<Mesh> meshes_;
    RenderStats stats_;
    GpuTimer gpuTimer_;
};

} // namespace blue
