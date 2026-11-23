#include "render/renderer.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "render/renderer_backend.h"

namespace blue::render {

Renderer::Renderer(const RendererOptions& options)
    : backend_(CreateBackend(options)) {}

Renderer::Renderer(const platform::Window& window,
                   const std::string& shader_dir,
                   shader::CompileMode shader_mode)
    : Renderer(RendererOptions{.window = &window,
                               .shader_dir = shader_dir,
                               .shader_mode = shader_mode}) {}

Renderer::~Renderer() = default;

Backend Renderer::backend() const { return backend_->backend(); }
BackendCaps Renderer::caps() const { return backend_->caps(); }

void Renderer::DrawFrame(const SceneView& view, ImDrawData* ui) {
  backend_->DrawFrame(view, ui);
}
ViewportImage Renderer::PrepareViewport(core::Extent2D size) {
  return backend_->PrepareViewport(size);
}
const ViewportImage& Renderer::viewport() const { return backend_->viewport(); }
void Renderer::DrawViewportFrame(const SceneView& view, ImDrawData* ui) {
  backend_->DrawViewportFrame(view, ui);
}
uint64_t Renderer::RequestViewportReadback() {
  return backend_->RequestViewportReadback();
}
uint64_t Renderer::RequestOutputReadback() {
  return backend_->RequestOutputReadback();
}
ReadbackStatus Renderer::TakeViewportReadback(uint64_t ticket,
                                              ViewportPixels& pixels) {
  return backend_->TakeViewportReadback(ticket, pixels);
}

MeshId Renderer::UploadMesh(std::span<const asset::Vertex> vertices,
                            std::span<const asset::Index> indices) {
  return backend_->UploadMesh(vertices, indices);
}
void Renderer::ReleaseMesh(MeshId id) { backend_->ReleaseMesh(id); }
void Renderer::ReleaseTextures(std::span<const TextureId> ids) {
  backend_->ReleaseTextures(ids);
}
void Renderer::ReleaseMaterial(MaterialId id) { backend_->ReleaseMaterial(id); }
void Renderer::FlushReleases() { backend_->FlushReleases(); }
ImTextureID Renderer::ui_texture(TextureId id) const {
  return backend_->ui_texture(id);
}

MaterialId Renderer::CreateMaterial(const material::MaterialDesc& desc) {
  return backend_->CreateMaterial(desc);
}
std::vector<TextureId> Renderer::UploadTextures(
    std::span<const TextureUpload> textures) {
  return backend_->UploadTextures(textures);
}
MaterialId Renderer::CreateMaterial(const material::MaterialDesc& desc,
                                    const MaterialTextures& textures) {
  return backend_->CreateMaterial(desc, textures);
}
void Renderer::UpdateMaterial(MaterialId id,
                              const material::MaterialDesc& desc) {
  backend_->UpdateMaterial(id, desc);
}
const material::MaterialDesc& Renderer::material(MaterialId id) const {
  return backend_->material(id);
}
size_t Renderer::material_count() const { return backend_->material_count(); }
TextureStats Renderer::texture_stats() const {
  return backend_->texture_stats();
}
MemoryStats Renderer::memory_stats() const { return backend_->memory_stats(); }

void Renderer::SetEnvironment(const asset::ImageDataF& equirect,
                              float intensity, float rotation_radians) {
  backend_->SetEnvironment(equirect, intensity, rotation_radians);
}
void Renderer::ResetEnvironment() { backend_->ResetEnvironment(); }
void Renderer::SetEnvironmentParams(float intensity, float rotation_radians) {
  backend_->SetEnvironmentParams(intensity, rotation_radians);
}

void Renderer::RequestSwapchainRebuild() {
  backend_->RequestSwapchainRebuild();
}
void Renderer::SetVsync(bool enabled) { backend_->SetVsync(enabled); }
void Renderer::ReloadChangedShaders() { backend_->ReloadChangedShaders(); }
std::string_view Renderer::last_shader_error() const {
  return backend_->last_shader_error();
}

bool Renderer::vsync() const { return backend_->vsync(); }
std::string_view Renderer::device_name() const {
  return backend_->device_name();
}
core::Extent2D Renderer::output_extent() const {
  return backend_->output_extent();
}
core::Extent2D Renderer::render_extent() const {
  return backend_->render_extent();
}
core::Extent2D Renderer::scene_output_extent() const {
  return backend_->scene_output_extent();
}
Upscaling Renderer::active_upscaling() const {
  return backend_->active_upscaling();
}
void Renderer::WaitIdle() const { backend_->WaitIdle(); }

std::optional<double> Renderer::gpu_frame_ms() const {
  return backend_->gpu_frame_ms();
}
std::optional<double> Renderer::gpu_scope_ms(GpuScope scope) const {
  return backend_->gpu_scope_ms(scope);
}
RenderStats Renderer::stats() const { return backend_->stats(); }
ValidationStats Renderer::validation_stats() const {
  return backend_->validation_stats();
}

void Renderer::InitImGuiRenderer() const { backend_->InitImGuiRenderer(); }
void Renderer::ShutdownImGuiRenderer() const {
  backend_->ShutdownImGuiRenderer();
}

}  // namespace blue::render
