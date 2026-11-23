#ifndef BLUE_RENDER_RENDERER_BACKEND_H_
#define BLUE_RENDER_RENDERER_BACKEND_H_

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "asset/image_decoder.h"
#include "asset/mesh_data.h"
#include "core/extent.h"
#include "material/material.h"
#include "render/backend.h"
#include "render/frame_plan.h"
#include "render/gpu_scope.h"
#include "render/memory_stats.h"
#include "render/renderer.h"
#include "render/scene_view.h"
#include "render/texture_types.h"

// Internal to the renderer: included by render/renderer.cc, the backend
// factory and the backends, never by a public header.

namespace blue::render {

// One GPU API's renderer behind render::Renderer: one pure virtual function
// per public Renderer function, each with the contract documented there
// (Renderer forwards every call unchanged). This is the only dynamic
// dispatch of the renderer: one indirect call per public call, nothing per
// draw or pass. Implementations are final. Neither copyable nor movable.
class RendererBackend {
 public:
  RendererBackend() = default;
  virtual ~RendererBackend() = default;

  RendererBackend(const RendererBackend&) = delete;
  RendererBackend& operator=(const RendererBackend&) = delete;

  [[nodiscard]] virtual Backend backend() const = 0;
  [[nodiscard]] virtual BackendCaps caps() const = 0;

  virtual void DrawFrame(const SceneView& view, ImDrawData* ui) = 0;
  [[nodiscard]] virtual ViewportImage PrepareViewport(core::Extent2D size) = 0;
  [[nodiscard]] virtual const ViewportImage& viewport() const = 0;
  virtual void DrawViewportFrame(const SceneView& view, ImDrawData* ui) = 0;
  [[nodiscard]] virtual uint64_t RequestViewportReadback() = 0;
  [[nodiscard]] virtual uint64_t RequestOutputReadback() = 0;
  [[nodiscard]] virtual ReadbackStatus TakeViewportReadback(
      uint64_t ticket, ViewportPixels& pixels) = 0;

  [[nodiscard]] virtual MeshId UploadMesh(
      std::span<const asset::Vertex> vertices,
      std::span<const asset::Index> indices) = 0;
  virtual void ReleaseMesh(MeshId id) = 0;
  virtual void ReleaseTextures(std::span<const TextureId> ids) = 0;
  virtual void ReleaseMaterial(MaterialId id) = 0;
  virtual void FlushReleases() = 0;
  [[nodiscard]] virtual ImTextureID ui_texture(TextureId id) const = 0;

  [[nodiscard]] virtual MaterialId CreateMaterial(
      const material::MaterialDesc& desc) = 0;
  [[nodiscard]] virtual std::vector<TextureId> UploadTextures(
      std::span<const TextureUpload> textures) = 0;
  [[nodiscard]] virtual MaterialId CreateMaterial(
      const material::MaterialDesc& desc, const MaterialTextures& textures) = 0;
  virtual void UpdateMaterial(MaterialId id,
                              const material::MaterialDesc& desc) = 0;
  [[nodiscard]] virtual const material::MaterialDesc& material(
      MaterialId id) const = 0;
  [[nodiscard]] virtual size_t material_count() const = 0;
  [[nodiscard]] virtual TextureStats texture_stats() const = 0;
  [[nodiscard]] virtual MemoryStats memory_stats() const = 0;

  virtual void SetEnvironment(const asset::ImageDataF& equirect,
                              float intensity, float rotation_radians) = 0;
  virtual void ResetEnvironment() = 0;
  virtual void SetEnvironmentParams(float intensity,
                                    float rotation_radians) = 0;

  virtual void RequestSwapchainRebuild() = 0;
  virtual void SetVsync(bool enabled) = 0;
  virtual void ReloadChangedShaders() = 0;
  [[nodiscard]] virtual std::string_view last_shader_error() const = 0;

  [[nodiscard]] virtual bool vsync() const = 0;
  [[nodiscard]] virtual std::string_view device_name() const = 0;
  [[nodiscard]] virtual core::Extent2D output_extent() const = 0;
  [[nodiscard]] virtual core::Extent2D render_extent() const = 0;
  [[nodiscard]] virtual core::Extent2D scene_output_extent() const = 0;
  [[nodiscard]] virtual Upscaling active_upscaling() const = 0;
  virtual void WaitIdle() const = 0;

  [[nodiscard]] virtual std::optional<double> gpu_frame_ms() const = 0;
  [[nodiscard]] virtual std::optional<double> gpu_scope_ms(
      GpuScope scope) const = 0;
  [[nodiscard]] virtual RenderStats stats() const = 0;
  [[nodiscard]] virtual ValidationStats validation_stats() const = 0;

  virtual void InitImGuiRenderer() const = 0;
  virtual void ShutdownImGuiRenderer() const = 0;
};

// Creates the renderer of options.backend, falling back to the other
// compiled backends (default, Vulkan, D3D12, Metal) that can use
// options.window when it has no suitable device. Fatal when none has one.
// Defined in render/backend_factory.cc.
[[nodiscard]] std::unique_ptr<RendererBackend> CreateBackend(
    const RendererOptions& options);

}  // namespace blue::render

#endif  // BLUE_RENDER_RENDERER_BACKEND_H_
