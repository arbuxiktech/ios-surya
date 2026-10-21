#version 450
layout(location = 0) out vec4 color;
layout(push_constant) uniform P { vec4 color; } p;
void main() { color = p.color; }
