#version 450
// A copy inside vram at the high resolution: the saved box is drawn back as a textured quad (what the OpenGL renderer does with its plain
// program in copy_box). The vertex shader is gpu_draw.vert; t0.xy is the texture coordinate.
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec4 t0; layout(location = 1) in vec4 t1; layout(location = 2) in vec4 t2; layout(location = 3) in vec3 col;
layout(location = 0) out vec4 fragColor;
void main() { fragColor = texture(tex, t0.xy); }
