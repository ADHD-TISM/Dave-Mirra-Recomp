#version 450
// The picture shader of the GPU hi-res renderer, ported from fs_src in runtime/gpu_gl.c (GLSL 330 to Vulkan GLSL 450: a descriptor set for
// the vram texture, push constants for pass and alpha, explicit locations). Keep the two in step.
layout(set = 0, binding = 0) uniform sampler2D vram;
layout(push_constant) uniform Push { vec2 target; int pass; float alpha; } pc;
layout(location = 0) in vec4 t0; layout(location = 1) in vec4 t1; layout(location = 2) in vec4 t2; layout(location = 3) in vec3 col;
layout(location = 0) out vec4 fragColor;
float word(float x, float y) {
  x = mod(x, 1024.0); y = mod(y, 512.0);
  vec2 c = texture(vram, vec2((x + 0.5) / 1024.0, (y + 0.5) / 512.0)).rg * 255.0;
  return floor(c.x + 0.5) + floor(c.y + 0.5) * 256.0;
}
float win(float u, float m, float o) {
  float r = u, p = 1.0;
  for (int k = 0; k < 5; k++) {
    if (mod(floor(m / p), 2.0) > 0.5) { float b = 8.0 * p; r += (mod(floor(o / p), 2.0) - mod(floor(u / b), 2.0)) * b; }
    p *= 2.0;
  }
  return r;
}
float dth(vec2 f) {
  int xi = int(mod(floor(f.x), 4.0)), yi = int(mod(floor(f.y), 4.0));
  vec4 c;
  if (xi == 0) c = vec4(-4.0, 2.0, -3.0, 3.0); else if (xi == 1) c = vec4(0.0, -2.0, 1.0, -1.0);
  else if (xi == 2) c = vec4(-3.0, 3.0, -4.0, 2.0); else c = vec4(1.0, -1.0, 0.0, -2.0);
  if (yi == 0) return c.x; if (yi == 1) return c.y; if (yi == 2) return c.z; return c.w;
}
void main() {
  float fl = floor(t1.w + 0.5);
  bool textured = mod(fl, 2.0) > 0.5, raw = mod(floor(fl / 2.0), 2.0) > 0.5, dith = mod(floor(fl / 4.0), 2.0) > 0.5, direct = mod(floor(fl / 8.0), 2.0) > 0.5;
  vec3 c8 = floor(col * 255.0 + 0.5);
  vec3 c5;
  if (textured) {
    float depth = floor(t1.z + 0.5), tx = floor(t0.z + 0.5), ty = floor(t0.w + 0.5), cx = floor(t1.x + 0.5), cy = floor(t1.y + 0.5);
    float u = floor(t0.x + 0.002), v = floor(t0.y + 0.002), w;
    if (direct) { w = word(u, v); }
    else {
      u = mod(u, 256.0); v = mod(v, 256.0);
      float mx = floor(t2.x + 0.5), my = floor(t2.y + 0.5), ox = floor(t2.z + 0.5), oy = floor(t2.w + 0.5);
      if (mx > 0.5) u = win(u, mx, ox);
      if (my > 0.5) v = win(v, my, oy);
      if (depth < 0.5) { w = word(tx + floor(u / 4.0), ty + v); float ci = mod(floor(w / exp2(mod(u, 4.0) * 4.0)), 16.0); w = word(cx + ci, cy); }
      else if (depth < 1.5) { w = word(tx + floor(u / 2.0), ty + v); float ci = mod(floor(w / (mod(u, 2.0) > 0.5 ? 256.0 : 1.0)), 256.0); w = word(cx + ci, cy); }
      else w = word(tx + u, ty + v);
      if (w < 0.5) discard;
    }
    float t15 = step(32768.0, w);
    if (pc.pass == 1 && t15 > 0.5) discard;
    if (pc.pass == 2 && t15 < 0.5) discard;
    vec3 t5 = vec3(mod(w, 32.0), mod(floor(w / 32.0), 32.0), mod(floor(w / 1024.0), 32.0));
    if (raw || direct) c5 = t5;
    else {
      vec3 m = floor(t5 * 8.0 * c8 / 128.0);
      if (dith) m += dth(gl_FragCoord.xy);
      c5 = floor(clamp(m, 0.0, 255.0) / 8.0);
    }
  } else {
    vec3 m = c8;
    if (dith) m += dth(gl_FragCoord.xy);
    c5 = floor(clamp(m, 0.0, 255.0) / 8.0);
  }
  fragColor = vec4(c5 * 8.0 / 255.0, pc.alpha);
}
