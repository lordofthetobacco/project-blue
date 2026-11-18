#version 450

// One triangle that covers the whole viewport (render::FullscreenPass). No
// vertex buffer: vertex i is at (-1, -1), (3, -1), (-1, 3). The viewport is
// not flipped, so clip y = -1 is the top row and outUv (0, 0) the top-left
// corner.

layout(location = 0) out vec2 outUv;

void main()
{
    const vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    outUv = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
