#pragma once

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace blue {

// One frame of already-interpreted input. Produced by the platform layer (SDL), consumed by gameplay/camera.
struct InputFrame {
    glm::vec2 mouseDelta{0.0f}; // summed relative motion in device counts, +x right, +y DOWN (SDL convention)
    float wheel = 0.0f;         // summed vertical notches, + = away from the user
    glm::vec3 move{0.0f};       // held-key axes in [-1,1]: x right/left, y up/down, z forward/back
    bool sprint = false;
    bool look = false;          // Fly: mouse captured. Orbit: LMB held.
    bool pan = false;           // Orbit: RMB or MMB held.
};

} // namespace blue
