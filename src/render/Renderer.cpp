#include "render/Renderer.hpp"

#include "platform/Window.hpp"
#include "render/VkCheck.hpp"

#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>
#include <glm/vec4.hpp>
#include <imgui_impl_vulkan.h>

#include <cstring>

namespace blue {

namespace {

// Reversed-Z: the projection maps near to 1 and infinity to 0, so clear to 0 and keep the greater value.
constexpr float kDepthClearValue = 0.0f;
constexpr VkCompareOp kDepthCompareOp = VK_COMPARE_OP_GREATER;

// Must match FrameUniforms in shaders/mesh.*  (std140).
struct FrameUniforms {
    glm::mat4 viewProj;
    glm::vec4 cameraPosition;
    glm::vec4 lightPosition;
    glm::vec4 lightColor; // rgb: color, a: intensity
    glm::vec4 ambientColor;
};

// Must match DrawConstants in shaders/mesh.vert.
struct DrawConstants {
    glm::mat4 model;
    glm::mat4 normalMatrix;
};
static_assert(sizeof(DrawConstants) <= 128, "Push constants must fit the guaranteed minimum of 128 bytes");

constexpr VkClearColorValue kClearColor{{0.015f, 0.018f, 0.022f, 1.0f}};

constexpr auto kVertexBindings = vertexBindings();
constexpr auto kVertexAttributes = vertexAttributes();
constexpr VkPushConstantRange kDrawConstantsRange{
    .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
    .offset = 0,
    .size = sizeof(DrawConstants),
};

VkDescriptorSetLayout createFrameSetLayout(VkDevice device)
{
    const VkDescriptorSetLayoutBinding binding{
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    const VkDescriptorSetLayoutCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &binding,
    };
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &createInfo, nullptr, &layout));
    return layout;
}

VkDescriptorPool createDescriptorPool(VkDevice device)
{
    const VkDescriptorPoolSize poolSize{
        .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = static_cast<uint32_t>(kFramesInFlight),
    };
    const VkDescriptorPoolCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = static_cast<uint32_t>(kFramesInFlight),
        .poolSizeCount = 1,
        .pPoolSizes = &poolSize,
    };
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorPool(device, &createInfo, nullptr, &pool));
    return pool;
}

ImageDesc depthImageDesc(VkExtent2D extent)
{
    return {
        .extent = extent,
        .format = Renderer::kDepthFormat,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
    };
}

GraphicsPipelineDesc meshPipelineDesc(const std::filesystem::path& shaderDir, const VkDescriptorSetLayout& frameSetLayout,
                                      VkFormat colorFormat)
{
    return {
        .vertexShader = shaderDir / "mesh.vert.spv",
        .fragmentShader = shaderDir / "mesh.frag.spv",
        .vertexBindings = kVertexBindings,
        .vertexAttributes = kVertexAttributes,
        .setLayouts = {&frameSetLayout, 1},
        .pushConstants = {&kDrawConstantsRange, 1},
        .colorFormat = colorFormat,
        .depthFormat = Renderer::kDepthFormat,
        .depthCompareOp = kDepthCompareOp,
    };
}

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                  VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                  VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                  VkImageLayout oldLayout, VkImageLayout newLayout)
{
    const VkImageMemoryBarrier2 barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = srcStage,
        .srcAccessMask = srcAccess,
        .dstStageMask = dstStage,
        .dstAccessMask = dstAccess,
        .oldLayout = oldLayout,
        .newLayout = newLayout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {aspect, 0, 1, 0, 1},
    };
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(cmd, &dependency);
}

} // namespace

Renderer::Renderer(const Window& window, const std::filesystem::path& shaderDir)
    : window_(window)
    , context_(window)
    , swapchain_(context_, window.pixelSize(), vsync_)
    , depth_(context_, depthImageDesc(swapchain_.extent()))
    , frameSetLayout_(createFrameSetLayout(context_.device()))
    , descriptorPool_(createDescriptorPool(context_.device()))
    , meshPipeline_(context_.device(), meshPipelineDesc(shaderDir, frameSetLayout_, swapchain_.format()))
    , gpuTimer_(context_)
{
    createFrames();
}

Renderer::~Renderer()
{
    context_.waitIdle();

    const VkDevice device = context_.device();
    for (Frame& frame : frames_) {
        vkDestroySemaphore(device, frame.imageAcquired, nullptr);
        vkDestroyFence(device, frame.inFlight, nullptr);
        vkDestroyCommandPool(device, frame.commandPool, nullptr);
    }
    vkDestroyDescriptorPool(device, descriptorPool_, nullptr);
    vkDestroyDescriptorSetLayout(device, frameSetLayout_, nullptr);
}

void Renderer::createFrames()
{
    const VkDevice device = context_.device();

    for (Frame& frame : frames_) {
        const VkCommandPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
            .queueFamilyIndex = context_.queueFamily(),
        };
        VK_CHECK(vkCreateCommandPool(device, &poolInfo, nullptr, &frame.commandPool));

        const VkCommandBufferAllocateInfo allocInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = frame.commandPool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        VK_CHECK(vkAllocateCommandBuffers(device, &allocInfo, &frame.commandBuffer));

        const VkFenceCreateInfo fenceInfo{
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };
        VK_CHECK(vkCreateFence(device, &fenceInfo, nullptr, &frame.inFlight));

        const VkSemaphoreCreateInfo semaphoreInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &frame.imageAcquired));

        frame.uniforms = Buffer(context_.allocator(), sizeof(FrameUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                Buffer::Access::CpuSequentialWrite);

        const VkDescriptorSetAllocateInfo setInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = descriptorPool_,
            .descriptorSetCount = 1,
            .pSetLayouts = &frameSetLayout_,
        };
        VK_CHECK(vkAllocateDescriptorSets(device, &setInfo, &frame.descriptorSet));

        const VkDescriptorBufferInfo bufferInfo{
            .buffer = frame.uniforms.handle(),
            .offset = 0,
            .range = sizeof(FrameUniforms),
        };
        const VkWriteDescriptorSet write{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = frame.descriptorSet,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo = &bufferInfo,
        };
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }
}

MeshId Renderer::uploadMesh(const MeshData& data)
{
    meshes_.emplace_back(context_, data);
    return static_cast<MeshId>(meshes_.size() - 1);
}

void Renderer::setVsync(bool enabled)
{
    if (vsync_ != enabled) {
        vsync_ = enabled;
        swapchainDirty_ = true;
    }
}

bool Renderer::rebuildSwapchain()
{
    context_.waitIdle();
    if (!swapchain_.recreate(window_.pixelSize(), vsync_)) {
        return false; // Surface has no area; stay dirty and retry next frame.
    }
    depth_ = Image(context_, depthImageDesc(swapchain_.extent()));
    swapchainDirty_ = false;
    return true;
}

void Renderer::drawFrame(const SceneView& scene, ImDrawData* ui)
{
    const VkExtent2D windowSize = window_.pixelSize();
    if (windowSize.width == 0 || windowSize.height == 0) {
        return; // Minimized: nothing to present to.
    }
    if (swapchainDirty_ && !rebuildSwapchain()) {
        return;
    }

    const VkDevice device = context_.device();
    Frame& frame = frames_[frameIndex_];

    VK_CHECK(vkWaitForFences(device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX));
    gpuTimer_.collect(frameIndex_);

    uint32_t imageIndex = 0;
    const VkResult acquireResult = vkAcquireNextImageKHR(device, swapchain_.handle(), UINT64_MAX,
                                                         frame.imageAcquired, VK_NULL_HANDLE, &imageIndex);
    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
        swapchainDirty_ = true;
        return;
    }
    if (acquireResult == VK_SUBOPTIMAL_KHR) {
        swapchainDirty_ = true; // Still presentable; rebuild next frame.
    } else {
        VK_CHECK(acquireResult);
    }

    // Reset only once we know work will be submitted, otherwise the next wait deadlocks.
    VK_CHECK(vkResetFences(device, 1, &frame.inFlight));

    const FrameUniforms uniforms{
        .viewProj = scene.camera.projection * scene.camera.view,
        .cameraPosition = glm::vec4(scene.camera.position, 1.0f),
        .lightPosition = glm::vec4(scene.light.position, 1.0f),
        .lightColor = glm::vec4(scene.light.color, scene.light.intensity),
        .ambientColor = glm::vec4(scene.ambient, 0.0f),
    };
    std::memcpy(frame.uniforms.mapped(), &uniforms, sizeof(uniforms));

    recordCommands(frame, imageIndex, scene, ui);

    const VkSemaphore renderFinished = swapchain_.renderFinished(imageIndex);
    const VkSemaphoreSubmitInfo waitInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = frame.imageAcquired,
        .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    };
    const VkSemaphoreSubmitInfo signalInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = renderFinished,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    const VkCommandBufferSubmitInfo cmdInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = frame.commandBuffer,
    };
    const VkSubmitInfo2 submitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = 1,
        .pWaitSemaphoreInfos = &waitInfo,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &cmdInfo,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos = &signalInfo,
    };
    VK_CHECK(vkQueueSubmit2(context_.queue(), 1, &submitInfo, frame.inFlight));

    const VkSwapchainKHR swapchain = swapchain_.handle();
    const VkPresentInfoKHR presentInfo{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &renderFinished,
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &imageIndex,
    };
    const VkResult presentResult = vkQueuePresentKHR(context_.queue(), &presentInfo);
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
        swapchainDirty_ = true;
    } else {
        VK_CHECK(presentResult);
    }

    frameIndex_ = (frameIndex_ + 1) % kFramesInFlight;
}

void Renderer::recordCommands(const Frame& frame, uint32_t imageIndex, const SceneView& scene, ImDrawData* ui)
{
    const VkCommandBuffer cmd = frame.commandBuffer;
    const VkImage colorImage = swapchain_.image(imageIndex);
    const VkExtent2D extent = swapchain_.extent();

    VK_CHECK(vkResetCommandPool(context_.device(), frame.commandPool, 0));
    const VkCommandBufferBeginInfo beginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(cmd, &beginInfo));
    gpuTimer_.begin(cmd, frameIndex_);

    // Previous contents are irrelevant for both attachments: they are cleared on load.
    imageBarrier(cmd, colorImage, VK_IMAGE_ASPECT_COLOR_BIT,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_NONE,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    imageBarrier(cmd, depth_.handle(), VK_IMAGE_ASPECT_DEPTH_BIT,
                 VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

    const VkRenderingAttachmentInfo colorAttachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = swapchain_.imageView(imageIndex),
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.color = kClearColor},
    };
    const VkRenderingAttachmentInfo depthAttachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = depth_.view(),
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .clearValue = {.depthStencil = {kDepthClearValue, 0}},
    };
    const VkRenderingInfo renderingInfo{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colorAttachment,
        .pDepthAttachment = &depthAttachment,
    };
    vkCmdBeginRendering(cmd, &renderingInfo);

    // Negative height flips Y so clip space is Y-up, matching GLM and keeping CCW front faces.
    const VkViewport viewport{
        .x = 0.0f,
        .y = static_cast<float>(extent.height),
        .width = static_cast<float>(extent.width),
        .height = -static_cast<float>(extent.height),
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshPipeline_.handle());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshPipeline_.layout(), 0, 1,
                            &frame.descriptorSet, 0, nullptr);

    stats_ = {};
    for (const DrawItem& item : scene.draws) {
        const Mesh& mesh = meshes_[static_cast<size_t>(item.mesh)];
        const DrawConstants draw{
            .model = item.transform,
            .normalMatrix = glm::transpose(glm::inverse(item.transform)),
        };
        vkCmdPushConstants(cmd, meshPipeline_.layout(), kDrawConstantsRange.stageFlags, 0, sizeof(draw), &draw);
        mesh.draw(cmd);
        ++stats_.drawCalls;
        stats_.triangles += mesh.indexCount() / 3;
    }

    if (ui != nullptr) {
        ImGui_ImplVulkan_RenderDrawData(ui, cmd);
    }

    vkCmdEndRendering(cmd);

    imageBarrier(cmd, colorImage, VK_IMAGE_ASPECT_COLOR_BIT,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    gpuTimer_.end(cmd, frameIndex_);
    VK_CHECK(vkEndCommandBuffer(cmd));
}

} // namespace blue
