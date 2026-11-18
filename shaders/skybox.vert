#version 450

// Full-screen triangle at reversed-Z depth 0 (infinitely far) for the
// skybox (render::SkyboxPass). Passes the NDC position on so the fragment
// shader can rebuild the view direction.

layout(location = 0) out vec2 outNdc;

void main()
{
    const vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    outNdc = uv * 2.0 - 1.0;
    gl_Position = vec4(outNdc, 0.0, 1.0);
}
