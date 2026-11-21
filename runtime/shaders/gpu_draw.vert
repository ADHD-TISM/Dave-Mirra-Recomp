#version 450
// The vertex shader of the GPU hi-res renderer, ported from vs_src in runtime/gpu_gl.c: positions are vram pixels times the scale.
layout(push_constant) uniform Push { vec2 target; int pass; float alpha; } pc;
layout(location = 0) in vec2 pos; layout(location = 1) in vec3 color;
layout(location = 2) in vec4 a0; layout(location = 3) in vec4 a1; layout(location = 4) in vec4 a2;
layout(location = 0) out vec4 t0; layout(location = 1) out vec4 t1; layout(location = 2) out vec4 t2; layout(location = 3) out vec3 col;
void main() {
    gl_Position = vec4(pos.x / pc.target.x * 2.0 - 1.0, pos.y / pc.target.y * 2.0 - 1.0, 0.0, 1.0);
    col = color; t0 = a0; t1 = a1; t2 = a2;
}
