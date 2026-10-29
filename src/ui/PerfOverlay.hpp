#pragma once

#include <glm/vec3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

struct ImDrawData;
union SDL_Event;

namespace blue {

class Renderer;
class Window;

// Rolling window of frame times with summary statistics.
class FrameTimeHistory {
public:
    static constexpr size_t kCapacity = 240;

    void push(float milliseconds);

    [[nodiscard]] const float* data() const { return samples_.data(); }
    [[nodiscard]] size_t size() const { return count_; }
    [[nodiscard]] size_t offset() const { return count_ < kCapacity ? 0 : next_; }
    [[nodiscard]] float average() const { return count_ ? sum_ / static_cast<float>(count_) : 0.0f; }
    [[nodiscard]] float min() const;
    [[nodiscard]] float max() const;

private:
    std::array<float, kCapacity> samples_{};
    size_t next_ = 0;
    size_t count_ = 0;
    float sum_ = 0.0f;
};

// Plain data the overlay shows about the camera; the views must outlive the build() call.
struct CameraOverlayInfo {
    std::string_view mode;
    glm::vec3 position{0.0f};
    float yawDegrees = 0.0f;
    float pitchDegrees = 0.0f;
    float flySpeed = 0.0f;
    float sensitivity = 0.0f;
    bool invertY = false;
    std::string_view hint;
};

// Plain data the overlay shows about the loaded scene; the views must outlive the build() call.
struct SceneOverlayInfo {
    CameraOverlayInfo camera;
    size_t entityCount = 0;
    uint32_t drawCalls = 0;
    uint64_t triangles = 0;
    std::string_view scenePath;
    std::string_view status;
};

// Dear ImGui performance overlay: FPS, CPU/GPU frame time, frame-time graph, device info.
class PerfOverlay {
public:
    PerfOverlay(const Window& window, const Renderer& renderer);
    ~PerfOverlay();

    PerfOverlay(const PerfOverlay&) = delete;
    PerfOverlay& operator=(const PerfOverlay&) = delete;

    void processEvent(const SDL_Event& event);
    void toggleVisible() { visible_ = !visible_; }

    // Records this frame's timings and builds the UI. Returns nullptr when hidden.
    [[nodiscard]] ImDrawData* build(float cpuFrameMs, std::optional<double> gpuFrameMs, const SceneOverlayInfo& scene);

private:
    const Renderer& renderer_;
    FrameTimeHistory cpuHistory_;
    std::optional<double> gpuFrameMs_;
    bool visible_ = true;
};

} // namespace blue
