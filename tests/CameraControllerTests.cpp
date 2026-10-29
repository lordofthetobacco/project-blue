#include "camera/CameraController.hpp"

#include <doctest/doctest.h>

#include <glm/gtc/constants.hpp>

#include <cmath>

using namespace blue;

namespace {

constexpr float kEpsilon = 1e-5f;
constexpr float kMoveEpsilon = 1e-4f;

bool nearlyEqual(float a, float b, float epsilon = kEpsilon)
{
    return std::abs(a - b) <= epsilon;
}

bool nearlyEqual(const glm::vec3& a, const glm::vec3& b, float epsilon = kEpsilon)
{
    return glm::length(a - b) <= epsilon;
}

InputFrame lookFrame(glm::vec2 delta)
{
    InputFrame frame;
    frame.mouseDelta = delta;
    frame.look = true;
    return frame;
}

InputFrame moveFrame(glm::vec3 move)
{
    InputFrame frame;
    frame.move = move;
    return frame;
}

// Camera at the origin looking down -Z, in the requested mode.
CameraController makeCamera(CameraMode mode = CameraMode::Fly)
{
    CameraController camera;
    camera.setFromSceneCamera({.position = {0.0f, 0.0f, 0.0f}, .target = {0.0f, 0.0f, -1.0f}});
    camera.setMode(mode);
    return camera;
}

} // namespace

TEST_CASE("mouse right turns the view right, mouse up looks up")
{
    const InputSettings settings;
    const float angle = 100.0f * settings.mouseSensitivity;

    CameraController right = makeCamera();
    right.update(lookFrame({100.0f, 0.0f}), settings, 0.016f);
    CHECK(right.yaw() < 0.0f);
    CHECK(nearlyEqual(right.yaw(), -angle));
    CHECK(right.forward().x > 0.0f);
    CHECK(right.forward().z < 0.0f);
    CHECK(nearlyEqual(right.forward(), {std::sin(angle), 0.0f, -std::cos(angle)}));

    CameraController up = makeCamera();
    up.update(lookFrame({0.0f, -100.0f}), settings, 0.016f); // SDL reports mouse-up as negative y.
    CHECK(up.pitch() > 0.0f);
    CHECK(up.forward().y > 0.0f);
    CHECK(nearlyEqual(up.forward(), {0.0f, std::sin(angle), -std::cos(angle)}));
}

TEST_CASE("invert Y flips only the vertical mapping")
{
    const InputSettings normal;
    InputSettings inverted;
    inverted.invertY = true;
    const float angle = 100.0f * normal.mouseSensitivity;

    CameraController up = makeCamera();
    up.update(lookFrame({0.0f, -100.0f}), inverted, 0.016f);
    CHECK(up.pitch() < 0.0f);
    CHECK(nearlyEqual(up.forward(), {0.0f, -std::sin(angle), -std::cos(angle)}));

    CameraController down = makeCamera();
    down.update(lookFrame({0.0f, 100.0f}), inverted, 0.016f);
    CHECK(down.pitch() > 0.0f);

    CameraController a = makeCamera();
    CameraController b = makeCamera();
    a.update(lookFrame({250.0f, 0.0f}), normal, 0.016f);
    b.update(lookFrame({250.0f, 0.0f}), inverted, 0.016f);
    CHECK(a.yaw() == b.yaw());
    CHECK(nearlyEqual(a.forward(), b.forward()));
}

TEST_CASE("mouse look is independent of how motion is split across frames")
{
    const InputSettings settings;

    CameraController single = makeCamera();
    single.update(lookFrame({300.0f, -120.0f}), settings, 0.016f);

    CameraController split = makeCamera();
    for (int i = 0; i < 100; ++i) {
        split.update(lookFrame({3.0f, -1.2f}), settings, 0.001f * static_cast<float>(i + 1));
    }

    CHECK(nearlyEqual(single.yaw(), split.yaw()));
    CHECK(nearlyEqual(single.pitch(), split.pitch()));
}

TEST_CASE("mouse delta is not scaled by frame time")
{
    const InputSettings settings;

    CameraController fast = makeCamera();
    CameraController slow = makeCamera();
    fast.update(lookFrame({120.0f, -45.0f}), settings, 0.001f);
    slow.update(lookFrame({120.0f, -45.0f}), settings, 0.5f);

    CHECK(fast.yaw() == slow.yaw());
    CHECK(fast.pitch() == slow.pitch());
}

TEST_CASE("pitch is clamped to 89 degrees and yaw wraps into (-pi, pi]")
{
    const InputSettings settings;
    const float maxPitch = glm::radians(89.0f);

    CameraController camera = makeCamera();
    camera.update(lookFrame({0.0f, -1.0e6f}), settings, 0.016f);
    CHECK(nearlyEqual(camera.pitch(), maxPitch));
    CHECK(std::abs(camera.forward().y) < 1.0f);

    camera.update(lookFrame({0.0f, 1.0e6f}), settings, 0.016f);
    CHECK(nearlyEqual(camera.pitch(), -maxPitch));
    CHECK(std::abs(camera.forward().y) < 1.0f);

    for (const float x : {1.0e5f, -1.0e5f, 2856.0f, -2856.0f, 12345.0f}) {
        camera.update(lookFrame({x, 0.0f}), settings, 0.016f);
        CHECK(camera.yaw() > -glm::pi<float>());
        CHECK(camera.yaw() <= glm::pi<float>());
    }

    // A turn of exactly -pi (yaw += pi) must land on +pi, not -pi.
    InputSettings coarse;
    coarse.mouseSensitivity = glm::pi<float>() / 4.0f;
    CameraController half = makeCamera();
    half.update(lookFrame({-4.0f, 0.0f}), coarse, 0.016f);
    CHECK(half.yaw() > 0.0f);
}

TEST_CASE("forward key moves along -Z at yaw 0 and follows pitch")
{
    const InputSettings settings;

    CameraController camera = makeCamera();
    camera.update(moveFrame({0.0f, 0.0f, 1.0f}), settings, 1.0f);
    CHECK(nearlyEqual(camera.position(), {0.0f, 0.0f, -camera.flySpeed()}, kMoveEpsilon));

    CameraController pitched = makeCamera();
    pitched.update(lookFrame({0.0f, -300.0f}), settings, 0.016f);
    const glm::vec3 before = pitched.position();
    pitched.update(moveFrame({0.0f, 0.0f, 1.0f}), settings, 1.0f);
    CHECK(pitched.position().y > before.y);
}

TEST_CASE("strafing moves along the view's right vector")
{
    const InputSettings settings;
    CameraController camera = makeCamera();
    camera.update(moveFrame({1.0f, 0.0f, 0.0f}), settings, 1.0f);
    CHECK(nearlyEqual(camera.position(), {camera.flySpeed(), 0.0f, 0.0f}, kMoveEpsilon));
}

TEST_CASE("diagonal movement is not faster than straight movement")
{
    const InputSettings settings;

    CameraController straight = makeCamera();
    CameraController diagonal = makeCamera();
    straight.update(moveFrame({0.0f, 0.0f, 1.0f}), settings, 0.5f);
    diagonal.update(moveFrame({1.0f, 0.0f, 1.0f}), settings, 0.5f);

    CHECK(nearlyEqual(glm::length(diagonal.position()), glm::length(straight.position()), kMoveEpsilon));
}

TEST_CASE("movement distance does not depend on frame rate")
{
    const InputSettings settings;

    CameraController coarse = makeCamera();
    coarse.update(moveFrame({0.3f, 0.0f, 1.0f}), settings, 1.0f);

    CameraController fine = makeCamera();
    for (int i = 0; i < 100; ++i) {
        fine.update(moveFrame({0.3f, 0.0f, 1.0f}), settings, 0.01f);
    }

    CHECK(nearlyEqual(coarse.position(), fine.position(), kMoveEpsilon));
}

TEST_CASE("sprint multiplies speed by four")
{
    const InputSettings settings;

    CameraController walk = makeCamera();
    CameraController sprint = makeCamera();
    InputFrame frame = moveFrame({0.0f, 0.0f, 1.0f});
    walk.update(frame, settings, 0.25f);
    frame.sprint = true;
    sprint.update(frame, settings, 0.25f);

    CHECK(nearlyEqual(glm::length(sprint.position()), 4.0f * glm::length(walk.position()), kMoveEpsilon));
}

TEST_CASE("up and down move along world Y regardless of pitch")
{
    const InputSettings settings;
    CameraController camera = makeCamera();
    camera.update(lookFrame({200.0f, -400.0f}), settings, 0.016f);
    const glm::vec3 before = camera.position();

    camera.update(moveFrame({0.0f, 1.0f, 0.0f}), settings, 1.0f);
    CHECK(nearlyEqual(camera.position() - before, {0.0f, camera.flySpeed(), 0.0f}, kMoveEpsilon));

    camera.update(moveFrame({0.0f, -1.0f, 0.0f}), settings, 1.0f);
    CHECK(nearlyEqual(camera.position(), before, kMoveEpsilon));
}

TEST_CASE("keyboard moves the fly camera whether or not the mouse is captured")
{
    const InputSettings settings;

    CameraController released = makeCamera();
    InputFrame frame = moveFrame({0.0f, 0.0f, 1.0f});
    frame.mouseDelta = {500.0f, 500.0f};
    frame.look = false;
    released.update(frame, settings, 1.0f);
    CHECK(nearlyEqual(released.position(), {0.0f, 0.0f, -released.flySpeed()}, kMoveEpsilon));
    CHECK(released.yaw() == 0.0f); // look gates the mouse only
    CHECK(released.pitch() == 0.0f);

    CameraController captured = makeCamera();
    frame.look = true;
    frame.mouseDelta = {0.0f, 0.0f};
    captured.update(frame, settings, 1.0f);
    CHECK(nearlyEqual(captured.position(), released.position(), kMoveEpsilon));
}

TEST_CASE("orbit mode ignores keyboard movement")
{
    const InputSettings settings;
    CameraController camera = makeCamera(CameraMode::Orbit);
    InputFrame frame = moveFrame({1.0f, 1.0f, 1.0f});
    frame.sprint = true;
    camera.update(frame, settings, 1.0f);
    CHECK(nearlyEqual(camera.position(), {0.0f, 0.0f, 0.0f}));
}

TEST_CASE("fly wheel scales speed by 1.2 per notch within clamps")
{
    const InputSettings settings;
    CameraController camera = makeCamera();
    const float start = camera.flySpeed();

    InputFrame frame;
    frame.wheel = 1.0f;
    camera.update(frame, settings, 0.016f);
    CHECK(nearlyEqual(camera.flySpeed(), start * 1.2f));

    frame.wheel = -2.0f;
    camera.update(frame, settings, 0.016f);
    CHECK(nearlyEqual(camera.flySpeed(), start * 1.2f / 1.44f));

    frame.wheel = 1000.0f;
    camera.update(frame, settings, 0.016f);
    CHECK(camera.flySpeed() == 100.0f);

    frame.wheel = -1000.0f;
    camera.update(frame, settings, 0.016f);
    CHECK(camera.flySpeed() == 0.25f);
}

TEST_CASE("orbit wheel scales distance by 0.9 per notch and keeps the target")
{
    const InputSettings settings;
    CameraController camera;
    camera.setFromSceneCamera({.position = {0.0f, 0.0f, 10.0f}, .target = {0.0f, 0.0f, 0.0f}});
    camera.setMode(CameraMode::Orbit);

    InputFrame frame;
    frame.wheel = 1.0f;
    camera.update(frame, settings, 0.016f);
    CHECK(nearlyEqual(camera.toSceneCamera().target, {0.0f, 0.0f, 0.0f}, kMoveEpsilon));
    CHECK(nearlyEqual(glm::length(camera.position()), 9.0f, kMoveEpsilon));

    frame.wheel = -1000.0f;
    camera.update(frame, settings, 0.016f);
    CHECK(nearlyEqual(glm::length(camera.position()), 500.0f, 1e-2f));
    CHECK(nearlyEqual(camera.toSceneCamera().target, {0.0f, 0.0f, 0.0f}, 1e-3f));

    frame.wheel = 1000.0f;
    camera.update(frame, settings, 0.016f);
    CHECK(nearlyEqual(glm::length(camera.position()), 0.25f, kMoveEpsilon));
}

TEST_CASE("orbit rotate keeps target fixed and distance constant")
{
    const InputSettings settings;
    CameraController camera;
    camera.setFromSceneCamera({.position = {3.0f, 2.0f, 4.0f}, .target = {1.0f, 0.5f, -1.0f}});
    camera.setMode(CameraMode::Orbit);
    const SceneCamera before = camera.toSceneCamera();
    const float distance = glm::length(before.target - before.position);

    for (const glm::vec2 delta : {glm::vec2{80.0f, -30.0f}, glm::vec2{-400.0f, 90.0f}, glm::vec2{20.0f, 5000.0f}}) {
        camera.update(lookFrame(delta), settings, 0.016f);
        const SceneCamera after = camera.toSceneCamera();
        CHECK(nearlyEqual(after.target, before.target, kMoveEpsilon));
        CHECK(nearlyEqual(glm::length(after.target - after.position), distance, kMoveEpsilon));
    }
}

TEST_CASE("orbit drag right swings the view right around the target")
{
    const InputSettings settings;
    CameraController camera;
    camera.setFromSceneCamera({.position = {0.0f, 0.0f, 10.0f}, .target = {0.0f, 0.0f, 0.0f}});
    camera.setMode(CameraMode::Orbit);

    camera.update(lookFrame({100.0f, 0.0f}), settings, 0.016f);
    CHECK(camera.yaw() < 0.0f);
    CHECK(camera.forward().x > 0.0f);  // view turned right...
    CHECK(camera.position().x < 0.0f); // ...so the camera swung to the left of the target
}

TEST_CASE("orbit pan moves target and position by the same vector")
{
    const InputSettings settings;
    CameraController camera;
    camera.setFromSceneCamera({.position = {0.0f, 0.0f, 10.0f}, .target = {0.0f, 0.0f, 0.0f}});
    camera.setMode(CameraMode::Orbit);
    const SceneCamera before = camera.toSceneCamera();

    InputFrame frame;
    frame.pan = true;
    frame.mouseDelta = {40.0f, 20.0f};
    camera.update(frame, settings, 0.016f);
    const SceneCamera after = camera.toSceneCamera();

    const glm::vec3 positionShift = after.position - before.position;
    const glm::vec3 targetShift = after.target - before.target;
    CHECK(nearlyEqual(positionShift, targetShift, kMoveEpsilon));
    CHECK(glm::length(positionShift) > 0.0f);
    CHECK(positionShift.x < 0.0f); // drag right: scene follows the cursor, camera goes left
    CHECK(positionShift.y > 0.0f); // drag down: camera goes up
    CHECK(nearlyEqual(camera.yaw(), 0.0f));
    CHECK(nearlyEqual(camera.pitch(), 0.0f));
}

TEST_CASE("orbit pan speed scales with distance")
{
    const InputSettings settings;
    InputFrame frame;
    frame.pan = true;
    frame.mouseDelta = {50.0f, 0.0f};

    CameraController nearCamera;
    nearCamera.setFromSceneCamera({.position = {0.0f, 0.0f, 2.0f}, .target = {0.0f, 0.0f, 0.0f}});
    nearCamera.setMode(CameraMode::Orbit);
    nearCamera.update(frame, settings, 0.016f);

    CameraController farCamera;
    farCamera.setFromSceneCamera({.position = {0.0f, 0.0f, 20.0f}, .target = {0.0f, 0.0f, 0.0f}});
    farCamera.setMode(CameraMode::Orbit);
    farCamera.update(frame, settings, 0.016f);

    CHECK(nearlyEqual(std::abs(farCamera.position().x), 10.0f * std::abs(nearCamera.position().x), kMoveEpsilon));
}

TEST_CASE("scene camera round trip preserves position, direction and distance")
{
    const SceneCamera input{.position = {2.2f, 1.6f, 2.8f}, .target = {0.3f, -0.4f, 0.1f}, .verticalFovDegrees = 47.0f};

    CameraController camera;
    camera.setFromSceneCamera(input);
    const SceneCamera output = camera.toSceneCamera();

    CHECK(nearlyEqual(output.position, input.position, kMoveEpsilon));
    CHECK(nearlyEqual(output.target, input.target, kMoveEpsilon));
    CHECK(output.verticalFovDegrees == 47.0f);
    CHECK(camera.verticalFovDegrees() == 47.0f);
}

TEST_CASE("degenerate scene camera falls back to looking down -Z")
{
    CameraController camera;
    camera.setFromSceneCamera({.position = {1.0f, 2.0f, 3.0f}, .target = {1.0f, 2.0f, 3.0f}});

    CHECK(nearlyEqual(camera.forward(), {0.0f, 0.0f, -1.0f}));
    CHECK(nearlyEqual(camera.toSceneCamera().target, {1.0f, 2.0f, 2.0f}));
}

TEST_CASE("switching modes keeps the view identical")
{
    const InputSettings settings;
    CameraController camera;
    camera.setFromSceneCamera({.position = {2.0f, 1.0f, 3.0f}, .target = {0.0f, 0.5f, 0.0f}});
    camera.update(lookFrame({60.0f, -25.0f}), settings, 0.016f);

    const glm::mat4 flyView = camera.view();
    const glm::vec3 flyPosition = camera.position();
    camera.setMode(CameraMode::Orbit);
    CHECK(camera.mode() == CameraMode::Orbit);

    const glm::mat4 orbitView = camera.view();
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            CHECK(nearlyEqual(flyView[column][row], orbitView[column][row]));
        }
    }

    // An idle orbit update must not drift the camera either.
    camera.update(InputFrame{}, settings, 0.016f);
    CHECK(nearlyEqual(camera.position(), flyPosition, kMoveEpsilon));
    camera.setMode(CameraMode::Fly);
    CHECK(nearlyEqual(camera.position(), flyPosition, kMoveEpsilon));
}
