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
vec3 fetch(vec2 t) { return texture(tex, (t + 0.5) / pc.inSize).rgb; }
float lum(vec3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }
void setf(inout vec2 dir, inout float len, vec2 pp, float w, float lA, float lB, float lC, float lD, float lE) {
  float dc = lD - lC, cb = lC - lB;
  float lenX = 1.0 / max(max(abs(dc), abs(cb)), 1e-6);
  float dirX = lD - lB;
  dir.x += dirX * w;
  lenX = clamp(abs(dirX) * lenX, 0.0, 1.0); lenX *= lenX; len += lenX * w;
  float ec = lE - lC, ca = lC - lA;
  float lenY = 1.0 / max(max(abs(ec), abs(ca)), 1e-6);
  float dirY = lE - lA;
  dir.y += dirY * w;
  lenY = clamp(abs(dirY) * lenY, 0.0, 1.0); lenY *= lenY; len += lenY * w;
}
void tap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len, float lob, float clp, vec3 c) {
  vec2 v = vec2(off.x * dir.x + off.y * dir.y, off.x * -dir.y + off.y * dir.x);
  v *= len;
  float d2 = min(v.x * v.x + v.y * v.y, clp);
  float wB = 2.0 / 5.0 * d2 - 1.0, wA = lob * d2 - 1.0;
  wB *= wB; wA *= wA;
  wB = 25.0 / 16.0 * wB - (25.0 / 16.0 - 1.0);
  float w = wB * wA;
  aC += c * w; aW += w;
}
void main() {
  vec2 ip = floor(gl_FragCoord.xy);   // Vulkan: the origin is the top left, the rows already run top down
  vec2 sc = pc.inSize / pc.outSize;
  vec2 pp = ip * sc + (0.5 * sc - 0.5);
  vec2 fp = floor(pp); pp -= fp;
  vec3 b = fetch(fp + vec2(0.,-1.)), c = fetch(fp + vec2(1.,-1.));
  vec3 e = fetch(fp + vec2(-1.,0.)), f = fetch(fp), g = fetch(fp + vec2(1.,0.)), h = fetch(fp + vec2(2.,0.));
  vec3 i = fetch(fp + vec2(-1.,1.)), j = fetch(fp + vec2(0.,1.)), k = fetch(fp + vec2(1.,1.)), l = fetch(fp + vec2(2.,1.));
  vec3 n = fetch(fp + vec2(0.,2.)), o = fetch(fp + vec2(1.,2.));
  float bL = lum(b), cL = lum(c), eL = lum(e), fL = lum(f), gL = lum(g), hL = lum(h), iL = lum(i), jL = lum(j), kL = lum(k), lL = lum(l), nL = lum(n), oL = lum(o);
  vec2 dir = vec2(0.0); float len = 0.0;
  setf(dir, len, pp, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
  setf(dir, len, pp, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
  setf(dir, len, pp, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
  setf(dir, len, pp, pp.x * pp.y, gL, jL, kL, lL, oL);
  vec2 dir2 = dir * dir;
  float dirR = dir2.x + dir2.y;
  bool zro = dirR < 1.0 / 32768.0;
  dirR = inversesqrt(max(dirR, 1e-9));
  dirR = zro ? 1.0 : dirR;
  dir.x = zro ? 1.0 : dir.x;
  dir *= dirR;
  len = len * 0.5; len *= len;
  float stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));
  vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
  float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
  float clp = 1.0 / lob;
  vec3 min4 = min(min(f, g), min(j, k)), max4 = max(max(f, g), max(j, k));
  vec3 aC = vec3(0.0); float aW = 0.0;
  tap(aC, aW, vec2( 0.,-1.) - pp, dir, len2, lob, clp, b);
  tap(aC, aW, vec2( 1.,-1.) - pp, dir, len2, lob, clp, c);
  tap(aC, aW, vec2(-1., 1.) - pp, dir, len2, lob, clp, i);
  tap(aC, aW, vec2( 0., 1.) - pp, dir, len2, lob, clp, j);
  tap(aC, aW, vec2( 0., 0.) - pp, dir, len2, lob, clp, f);
  tap(aC, aW, vec2(-1., 0.) - pp, dir, len2, lob, clp, e);
  tap(aC, aW, vec2( 1., 1.) - pp, dir, len2, lob, clp, k);
  tap(aC, aW, vec2( 2., 1.) - pp, dir, len2, lob, clp, l);
  tap(aC, aW, vec2( 2., 0.) - pp, dir, len2, lob, clp, h);
  tap(aC, aW, vec2( 1., 0.) - pp, dir, len2, lob, clp, g);
  tap(aC, aW, vec2( 1., 2.) - pp, dir, len2, lob, clp, o);
  tap(aC, aW, vec2( 0., 2.) - pp, dir, len2, lob, clp, n);
  fragColor = vec4(min(max4, max(min4, aC / aW)), 1.0);
}
