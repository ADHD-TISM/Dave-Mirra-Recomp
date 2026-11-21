#version 450
// The shaders in this directory are ported from AMD FidelityFX Super Resolution 1.0 (ffx_fsr1.h):
// 
//    Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
//    Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
//    associated documentation files (the "Software"), to deal in the Software without restriction, including
//    without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//    copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the
//    following conditions: The above copyright notice and this permission notice shall be included in all
//    copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF
//    ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
//    FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
//    LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
//    ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// Vulkan port of the OpenGL shader in runtime/gfx_gl.c: push constants and a descriptor set instead of uniforms, top-left origin.
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(push_constant) uniform Push { vec2 inSize; vec2 outSize; vec2 origin; float con; } pc;
layout(location = 0) out vec4 fragColor;
vec3 ld(vec2 p) { return texture(tex, (p + 0.5) / pc.outSize).rgb; }
void main() {
  vec2 sp = floor(gl_FragCoord.xy - pc.origin);
  vec3 b = ld(sp + vec2(0., -1.)), d = ld(sp + vec2(-1., 0.)), e = ld(sp), f = ld(sp + vec2(1., 0.)), h = ld(sp + vec2(0., 1.));
  vec3 mn4 = min(min(b, d), min(f, h)), mx4 = max(max(b, d), max(f, h));
  vec3 hitMin = min(mn4, e) / (4.0 * max(mx4, 1e-5));
  vec3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, -1e-5);
  vec3 lobe3 = max(-hitMin, hitMax);
  float lobe = max(-0.1875, min(max(max(lobe3.r, lobe3.g), lobe3.b), 0.0)) * pc.con;
  float rcpL = 1.0 / (4.0 * lobe + 1.0);
  fragColor = vec4(clamp((lobe * (b + d + h + f) + e) * rcpL, 0.0, 1.0), 1.0);
}
