#pragma once

#include "camera/CameraController.hpp"
#include "input/InputSettings.hpp"
#include "platform/InputMapper.hpp"
#include "platform/Window.hpp"
#include "render/MeshCache.hpp"
#include "render/Renderer.hpp"
#include "scene/Scene.hpp"
#include "ui/PerfOverlay.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

union SDL_Event;

namespace blue {

struct CliOptions {
    std::optional<std::filesystem::path> scenePath;
    std::vector<std::filesystem::path> imports;
    bool showHelp = false;
};

inline constexpr const char* kUsage = "usage: project-blue [--scene <file.bscn>] [--import <model>]...";

// Returns nullopt for unknown arguments or options missing their value. --help / -h sets showHelp.
[[nodiscard]] std::optional<CliOptions> parseCommandLine(int argc, const char* const argv[]);

class App {
public:
    explicit App(const CliOptions& options);

    int run();

private:
    void handleEvent(const SDL_Event& event);
    void applyMouseCapture();
    void handleAction(InputAction action);
    void changeSensitivity(float factor);
    void saveSettings();
    [[nodiscard]] CameraOverlayInfo cameraOverlayInfo() const;
    void initializeScene(const CliOptions& options);
    void addImports(const std::vector<std::filesystem::path>& imports);
    void saveCurrentScene();
    void reloadCurrentScene();
    [[nodiscard]] SceneView buildSceneView();

    // Declaration order is destruction order in reverse: the overlay and renderer
    // must go before the window they render into.
    Window window_;
    Renderer renderer_;
    MeshCache meshCache_;
    PerfOverlay overlay_;

    Scene scene_;
    std::filesystem::path scenePath_;
    std::string scenePathText_;
    std::string status_;
    std::vector<DrawItem> draws_;

    InputMapper input_;
    CameraController camera_;
    InputSettings settings_;
    std::filesystem::path settingsPath_;

    bool mouseCaptured_ = false;
};

} // namespace blue
