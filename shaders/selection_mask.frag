#version 450

// Coverage of the selected draw items (render::OverlayPass), drawn with
// depth.vert: 0.5 where an item is hidden by the scene (pipeline without a
// depth test), 1 where it is visible (depth-tested pipeline, drawn second).

layout(constant_id = 0) const bool kVisible = false;

layout(location = 0) out vec4 outMask;

void main()
{
    outMask = vec4(kVisible ? 1.0 : 0.5);
}
