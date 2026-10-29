#include "App.hpp"

#include "camera/Projection.hpp"
#include "core/Log.hpp"
#include "core/Path.hpp"
#include "scene/Placement.hpp"
#include "scene/SceneSerializer.hpp"

#include <SDL3/SDL.h>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <system_error>

namespace blue {

namespace {

constexpr int kInitialWidth = 1600;
constexpr int kInitialHeight = 900;
constexpr float kNearPlane = 0.1f;
constexpr float kMaxSimulationStep = 0.1f; // Avoids jumps after stalls (window drag, breakpoints).
constexpr float kSensitivityStep = 1.15f;

constexpr float kImportTargetSize = 2.0f;
constexpr float kImportSpacing = 2.5f;

constexpr const char* kDefaultScenePath = "scenes/untitled.bscn";
constexpr const char* kSettingsFileName = "input.cfg";

std::filesystem::path shaderDirectory()
{
    // Shaders are deployed next to the executable, so lookups do not depend on the working directory.
    const char* basePath = SDL_GetBasePath();
    return std::filesystem::path(basePath != nullptr ? basePath : "") / "shaders";
}

[[nodiscard]] std::filesystem::path pathFromUtf8(const char* text)
{
    const std::string_view view(text);
    return std::filesystem::path(std::u8string(view.begin(), view.end()));
}

[[nodiscard]] std::filesystem::path inputSettingsPath()
{
    char* prefPath = SDL_GetPrefPath("ProjectBlue", "ProjectBlue");
    if (prefPath == nullptr) {
        log::warn("SDL_GetPrefPath failed, keeping input settings in the working directory: {}", SDL_GetError());
        return kSettingsFileName;
    }
    std::filesystem::path path = pathFromUtf8(prefPath) / kSettingsFileName;
    SDL_free(prefPath);
    return path;
}

[[nodiscard]] std::string_view modeLabel(CameraMode mode)
{
    return mode == CameraMode::Fly ? "Fly" : "Orbit";
}

[[nodiscard]] std::string_view cameraHint(CameraMode mode, bool mouseCaptured)
{
    if (mode == CameraMode::Orbit) {
        return "LMB rotate, RMB/MMB pan, wheel zoom";
    }
    return mouseCaptured ? "Esc to release" : "Click to look, Esc releases";
}

[[nodiscard]] std::string windowTitle(const std::filesystem::path& scenePath)
{
    return "Project Blue \xE2\x80\x94 " + toUtf8(scenePath.filename());
}

[[nodiscard]] std::string timedStatus(std::string_view verb, const std::string& path,
                                      std::chrono::steady_clock::time_point start)
{
    const double milliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return std::format("{} {} ({:.1f} ms)", verb, path, milliseconds);
}

} // namespace

std::optional<CliOptions> parseCommandLine(int argc, const char* const argv[])
{
    CliOptions options;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            options.showHelp = true;
            return options;
        }
        const bool isScene = std::strcmp(argv[i], "--scene") == 0;
        const bool isImport = std::strcmp(argv[i], "--import") == 0;
        if ((!isScene && !isImport) || i + 1 >= argc) {
            return std::nullopt;
        }
        const std::filesystem::path value = pathFromUtf8(argv[++i]);
        if (isScene) {
            options.scenePath = value;
        } else {
            options.imports.push_back(value);
        }
    }
    return options;
}

App::App(const CliOptions& options)
    : window_("Project Blue", kInitialWidth, kInitialHeight)
    , renderer_(window_, shaderDirectory())
    , meshCache_(renderer_)
    , overlay_(window_, renderer_)
    , scenePath_(options.scenePath.value_or(kDefaultScenePath))
    , scenePathText_(toUtf8(scenePath_))
    , settingsPath_(inputSettingsPath())
{
    settings_ = loadInputSettings(settingsPath_);
    SDL_SetWindowTitle(window_.handle(), windowTitle(scenePath_).c_str());
    initializeScene(options);
    camera_.setFromSceneCamera(scene_.camera);
}

void App::initializeScene(const CliOptions& options)
{
    std::error_code ec;
    if (options.scenePath && std::filesystem::exists(scenePath_, ec)) {
        auto loaded = loadScene(scenePath_);
        if (loaded) {
            scene_ = std::move(*loaded);
            status_ = "Loaded " + scenePathText_;
            return;
        }
        log::error("Failed to load '{}': {}", scenePathText_, toString(loaded.error()));
        status_ = std::string("Load failed: ") + toString(loaded.error());
    }

    scene_.entities.push_back({.name = "ground", .meshPath = "builtin:plane", .transform = {}});
    if (options.imports.empty()) {
        Transform cube;
        cube.position.y = 0.5f; // Unit cube centred at the origin; rest it on the ground.
        scene_.entities.push_back({.name = "cube", .meshPath = "builtin:cube", .transform = cube});
    } else {
        addImports(options.imports);
    }
}

void App::addImports(const std::vector<std::filesystem::path>& imports)
{
    const float centeredOffset = (static_cast<float>(imports.size()) - 1.0f) * 0.5f;
    for (size_t i = 0; i < imports.size(); ++i) {
        const std::string meshPath = toUtf8(imports[i]);
        const std::optional<CachedMesh> mesh = meshCache_.get(meshPath);
        if (!mesh) {
            continue; // The cache already logged why.
        }
        const glm::vec3 ground{(static_cast<float>(i) - centeredOffset) * kImportSpacing, 0.0f, 0.0f};
        scene_.entities.push_back({
            .name = toUtf8(imports[i].stem()),
            .meshPath = meshPath,
            .transform = fitOnGround(mesh->bounds.min, mesh->bounds.max, kImportTargetSize, ground),
        });
    }
}

void App::saveCurrentScene()
{
    scene_.camera = camera_.toSceneCamera();
    const auto start = std::chrono::steady_clock::now();
    const auto result = saveScene(scene_, scenePath_);
    if (result) {
        status_ = timedStatus("Saved", scenePathText_, start);
        log::info("{}", status_);
    } else {
        status_ = std::string("Save failed: ") + toString(result.error());
        log::error("{}", status_);
    }
}

void App::reloadCurrentScene()
{
    const auto start = std::chrono::steady_clock::now();
    auto loaded = loadScene(scenePath_);
    if (loaded) {
        scene_ = std::move(*loaded);
        camera_.setFromSceneCamera(scene_.camera);
        status_ = timedStatus("Loaded", scenePathText_, start);
        log::info("{}", status_);
    } else {
        status_ = std::string("Load failed: ") + toString(loaded.error());
        log::error("{}", status_);
    }
}

int App::run()
{
    using Clock = std::chrono::steady_clock;
    auto previous = Clock::now();

    for (;;) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            handleEvent(event);
        }

        applyMouseCapture();
        if (input_.quitRequested()) {
            break;
        }

        if (window_.isMinimized()) {
            SDL_WaitEvent(nullptr); // Sleep until something happens instead of spinning.
            previous = Clock::now();
            continue;
        }

        const auto now = Clock::now();
        const float frameSeconds = std::chrono::duration<float>(now - previous).count();
        previous = now;

        const InputFrame input = input_.endFrame();
        for (const InputAction action : input_.takeActions()) {
            handleAction(action);
        }
        camera_.setMode(input_.mode());
        camera_.update(input, settings_, std::min(frameSeconds, kMaxSimulationStep));

        const RenderStats stats = renderer_.stats();
        const SceneOverlayInfo sceneInfo{
            .camera = cameraOverlayInfo(),
            .entityCount = scene_.entities.size(),
            .drawCalls = stats.drawCalls,
            .triangles = stats.triangles,
            .scenePath = scenePathText_,
            .status = status_,
        };
        ImDrawData* ui = overlay_.build(frameSeconds * 1000.0f, renderer_.gpuFrameMs(), sceneInfo);
        renderer_.drawFrame(buildSceneView(), ui);
    }
    return 0;
}

void App::handleEvent(const SDL_Event& event)
{
    overlay_.processEvent(event);
    input_.handleEvent(event);

    if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
        renderer_.requestSwapchainRebuild();
    }
}

void App::applyMouseCapture()
{
    if (input_.wantsMouseCaptured() != mouseCaptured_) {
        mouseCaptured_ = input_.wantsMouseCaptured();
        window_.setRelativeMouseMode(mouseCaptured_);
    }
}

void App::handleAction(InputAction action)
{
    switch (action) {
    case InputAction::ToggleInvertY:
        settings_.invertY = !settings_.invertY;
        saveSettings();
        break;
    case InputAction::SensitivityDown: changeSensitivity(1.0f / kSensitivityStep); break;
    case InputAction::SensitivityUp: changeSensitivity(kSensitivityStep); break;
    case InputAction::ToggleOverlay: overlay_.toggleVisible(); break;
    case InputAction::SaveScene: saveCurrentScene(); break;
    case InputAction::LoadScene: reloadCurrentScene(); break;
    case InputAction::ToggleVsync: renderer_.setVsync(!renderer_.vsync()); break;
    }
}

void App::changeSensitivity(float factor)
{
    settings_.mouseSensitivity =
        std::clamp(settings_.mouseSensitivity * factor, kMinMouseSensitivity, kMaxMouseSensitivity);
    saveSettings();
}

void App::saveSettings()
{
    if (!saveInputSettings(settings_, settingsPath_)) {
        status_ = "Saving input settings failed";
    }
}

CameraOverlayInfo App::cameraOverlayInfo() const
{
    return {
        .mode = modeLabel(camera_.mode()),
        .position = camera_.position(),
        .yawDegrees = glm::degrees(camera_.yaw()),
        .pitchDegrees = glm::degrees(camera_.pitch()),
        .flySpeed = camera_.flySpeed(),
        .sensitivity = settings_.mouseSensitivity,
        .invertY = settings_.invertY,
        .hint = cameraHint(camera_.mode(), mouseCaptured_),
    };
}

SceneView App::buildSceneView()
{
    const VkExtent2D extent = renderer_.swapchain().extent();
    const float aspect = static_cast<float>(extent.width) / static_cast<float>(std::max(extent.height, 1u));

    draws_.clear();
    for (const Entity& entity : scene_.entities) {
        if (const std::optional<CachedMesh> mesh = meshCache_.get(entity.meshPath)) {
            draws_.push_back({.mesh = mesh->id, .transform = entity.transform.matrix()});
        }
    }

    SceneView view;
    view.camera.position = camera_.position();
    view.camera.view = camera_.view();
    view.camera.projection =
        reversedInfinitePerspective(glm::radians(camera_.verticalFovDegrees()), aspect, kNearPlane);
    view.light = {.position = scene_.light.position, .color = scene_.light.color, .intensity = scene_.light.intensity};
    view.ambient = scene_.ambient;
    view.draws = draws_;
    return view;
}

} // namespace blue
