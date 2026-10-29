#pragma once

#include <vulkan/vulkan.h>

#include <filesystem>
#include <span>

namespace blue {

struct GraphicsPipelineDesc {
    std::filesystem::path vertexShader;
    std::filesystem::path fragmentShader;

    std::span<const VkVertexInputBindingDescription> vertexBindings;
    std::span<const VkVertexInputAttributeDescription> vertexAttributes;
    std::span<const VkDescriptorSetLayout> setLayouts;
    std::span<const VkPushConstantRange> pushConstants;

    VkFormat colorFormat = VK_FORMAT_UNDEFINED;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
    VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkCompareOp depthCompareOp = VK_COMPARE_OP_GREATER;
};

// Graphics pipeline for dynamic rendering, with its layout. Viewport and scissor are dynamic.
class Pipeline {
public:
    Pipeline(VkDevice device, const GraphicsPipelineDesc& desc);
    ~Pipeline();

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    [[nodiscard]] VkPipeline handle() const { return pipeline_; }
    [[nodiscard]] VkPipelineLayout layout() const { return layout_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

} // namespace blue
