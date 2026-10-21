#version 450
// Rasterisation check: one triangle from the vertex index, no vertex buffers.
void main() {
    const vec2 p[3] = vec2[](vec2(-0.5, -0.5), vec2(0.5, -0.5), vec2(0.0, 0.5));
    gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
}
