# The Vulkan renderer backend (namespace blue::render::vulkan): device, swapchain, render
# targets, the frame's passes, meshes, materials and textures. Built when
# BLUE_RENDERER_VULKAN is ON (every platform; macOS through MoltenVK). Only blue_render
# links it (CreateVulkanRenderer()); Vulkan-only tests link it directly.
add_library(blue_render_vulkan STATIC
    barriers.cc
    buffer.cc
    descriptor_layout.cc
    draw_record.cc
    environment.cc
    frame_globals.cc
    fullscreen_pass.cc
    gpu_timer.cc
    image.cc
    material_system.cc
    mesh.cc
    pipeline.cc
    render_targets.cc
    samplers.cc
    shader_programs.cc
    staging_upload.cc
    swapchain.cc
    texture_cache.cc
    vk_check.cc
    vma.cc
    vulkan_context.cc
    vulkan_renderer.cc
    passes/bloom_pass.cc
    passes/depth_prepass.cc
    passes/mesh_pass.cc
    passes/motion_blur_pass.cc
    passes/overlay_pass.cc
    passes/present_pass.cc
    passes/reticle_pass.cc
    passes/scene_pass.cc
    passes/scope_lens_pass.cc
    passes/shadow_pass.cc
    passes/skybox_pass.cc
    passes/ssao_pass.cc
    passes/tonemap_pass.cc
    passes/ui_pass.cc
)

target_include_directories(blue_render_vulkan PUBLIC ${PROJECT_SOURCE_DIR}/src)

blue_target_defaults(blue_render_vulkan)

# The backend's headers name Vulkan and VMA types, so whoever includes them (the backend's
# own tests) gets both; consumers of render::Renderer never do (they link blue_render).
target_link_libraries(blue_render_vulkan
    PUBLIC
        blue_render_common
        blue_window
        blue_image
        blue_shader
        imgui
        Vulkan::Vulkan
        GPUOpen::VulkanMemoryAllocator
    PRIVATE
        SDL3::SDL3-static
)
