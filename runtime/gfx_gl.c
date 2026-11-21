/* The OpenGL backend: the window picture (the CPU picture or the one the GPU renderer made, the sharp and smooth filter, FSR)
   and the GPU hi-res renderer of gpu_gl.c, behind the interface of gfx.h. This is the code that used to live in host_common.c. */
#ifdef DM_SDL
#include <SDL_opengl.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "hw.h"
#include "host.h"
#include "gfx.h"
#include "gpu_gl.h"
#ifndef APIENTRY
#define APIENTRY
#endif

static int gl_tw, gl_th; static GLuint gl_tex;

/* The picture is drawn as one textured quad so the graphics card does the
   enlarging (and the smooth filter). Without OpenGL the platform stretches it. */

/* The two shaders below are ported from AMD FidelityFX Super Resolution 1.0 (ffx_fsr1.h):

   Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
   associated documentation files (the "Software"), to deal in the Software without restriction, including
   without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the
   following conditions: The above copyright notice and this permission notice shall be included in all
   copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF
   ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
   FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
   LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
   ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/
/* FSR 1 (AMD FidelityFX Super Resolution, MIT licence): EASU upscales the picture to
   the size it will have on screen, then RCAS sharpens it. Both are fragment shaders;
   the GL 2.0 / FBO entry points are looked up at run time, and if anything is missing
   the plain path is used. */
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
typedef GLuint (APIENTRY *fn_createshader)(GLenum);
typedef void (APIENTRY *fn_shadersource)(GLuint, GLsizei, const char *const *, const GLint *);
typedef void (APIENTRY *fn_u_i)(GLuint);
typedef void (APIENTRY *fn_getiv)(GLuint, GLenum, GLint *);
typedef void (APIENTRY *fn_getlog)(GLuint, GLsizei, GLsizei *, char *);
typedef GLuint (APIENTRY *fn_createprogram)(void);
typedef void (APIENTRY *fn_attach)(GLuint, GLuint);
typedef GLint (APIENTRY *fn_uloc)(GLuint, const char *);
typedef void (APIENTRY *fn_uni1i)(GLint, GLint);
typedef void (APIENTRY *fn_uni1f)(GLint, GLfloat);
typedef void (APIENTRY *fn_uni2f)(GLint, GLfloat, GLfloat);
typedef void (APIENTRY *fn_genfb)(GLsizei, GLuint *);
typedef void (APIENTRY *fn_bindfb)(GLenum, GLuint);
typedef void (APIENTRY *fn_fbtex)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (APIENTRY *fn_checkfb)(GLenum);
static fn_createshader p_CreateShader; static fn_shadersource p_ShaderSource; static fn_u_i p_CompileShader, p_LinkProgram, p_UseProgram;
static fn_getiv p_GetShaderiv, p_GetProgramiv; static fn_getlog p_GetShaderInfoLog; static fn_createprogram p_CreateProgram;
static fn_attach p_AttachShader; static fn_uloc p_GetUniformLocation; static fn_uni1i p_Uniform1i; static fn_uni1f p_Uniform1f;
static fn_uni2f p_Uniform2f; static fn_genfb p_GenFramebuffers; static fn_bindfb p_BindFramebuffer; static fn_fbtex p_FramebufferTexture2D;
static fn_checkfb p_CheckFramebufferStatus;
static GLuint fsr_easu, fsr_rcas, fsr_fbo, fsr_tex; static int fsr_state, fsr_tw, fsr_th;   /* state: 0 untried, 1 ready, -1 unavailable */

static const char *const easu_src =
"#version 120\n"
"uniform sampler2D tex; uniform vec2 inSize; uniform vec2 outSize;\n"
"vec3 fetch(vec2 t) { return texture2D(tex, (t + 0.5) / inSize).rgb; }\n"
"float lum(vec3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }\n"
"void setf(inout vec2 dir, inout float len, vec2 pp, float w, float lA, float lB, float lC, float lD, float lE) {\n"
"  float dc = lD - lC, cb = lC - lB;\n"
"  float lenX = 1.0 / max(max(abs(dc), abs(cb)), 1e-6);\n"
"  float dirX = lD - lB;\n"
"  dir.x += dirX * w;\n"
"  lenX = clamp(abs(dirX) * lenX, 0.0, 1.0); lenX *= lenX; len += lenX * w;\n"
"  float ec = lE - lC, ca = lC - lA;\n"
"  float lenY = 1.0 / max(max(abs(ec), abs(ca)), 1e-6);\n"
"  float dirY = lE - lA;\n"
"  dir.y += dirY * w;\n"
"  lenY = clamp(abs(dirY) * lenY, 0.0, 1.0); lenY *= lenY; len += lenY * w;\n"
"}\n"
"void tap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len, float lob, float clp, vec3 c) {\n"
"  vec2 v = vec2(off.x * dir.x + off.y * dir.y, off.x * -dir.y + off.y * dir.x);\n"
"  v *= len;\n"
"  float d2 = min(v.x * v.x + v.y * v.y, clp);\n"
"  float wB = 2.0 / 5.0 * d2 - 1.0, wA = lob * d2 - 1.0;\n"
"  wB *= wB; wA *= wA;\n"
"  wB = 25.0 / 16.0 * wB - (25.0 / 16.0 - 1.0);\n"
"  float w = wB * wA;\n"
"  aC += c * w; aW += w;\n"
"}\n"
"void main() {\n"
"  vec2 ip = vec2(floor(gl_FragCoord.x), outSize.y - 1.0 - floor(gl_FragCoord.y));\n"
"  vec2 sc = inSize / outSize;\n"
"  vec2 pp = ip * sc + (0.5 * sc - 0.5);\n"
"  vec2 fp = floor(pp); pp -= fp;\n"
"  vec3 b = fetch(fp + vec2(0.,-1.)), c = fetch(fp + vec2(1.,-1.));\n"
"  vec3 e = fetch(fp + vec2(-1.,0.)), f = fetch(fp), g = fetch(fp + vec2(1.,0.)), h = fetch(fp + vec2(2.,0.));\n"
"  vec3 i = fetch(fp + vec2(-1.,1.)), j = fetch(fp + vec2(0.,1.)), k = fetch(fp + vec2(1.,1.)), l = fetch(fp + vec2(2.,1.));\n"
"  vec3 n = fetch(fp + vec2(0.,2.)), o = fetch(fp + vec2(1.,2.));\n"
"  float bL = lum(b), cL = lum(c), eL = lum(e), fL = lum(f), gL = lum(g), hL = lum(h), iL = lum(i), jL = lum(j), kL = lum(k), lL = lum(l), nL = lum(n), oL = lum(o);\n"
"  vec2 dir = vec2(0.0); float len = 0.0;\n"
"  setf(dir, len, pp, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);\n"
"  setf(dir, len, pp, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);\n"
"  setf(dir, len, pp, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);\n"
"  setf(dir, len, pp, pp.x * pp.y, gL, jL, kL, lL, oL);\n"
"  vec2 dir2 = dir * dir;\n"
"  float dirR = dir2.x + dir2.y;\n"
"  bool zro = dirR < 1.0 / 32768.0;\n"
"  dirR = inversesqrt(max(dirR, 1e-9));\n"
"  dirR = zro ? 1.0 : dirR;\n"
"  dir.x = zro ? 1.0 : dir.x;\n"
"  dir *= dirR;\n"
"  len = len * 0.5; len *= len;\n"
"  float stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));\n"
"  vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);\n"
"  float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;\n"
"  float clp = 1.0 / lob;\n"
"  vec3 min4 = min(min(f, g), min(j, k)), max4 = max(max(f, g), max(j, k));\n"
"  vec3 aC = vec3(0.0); float aW = 0.0;\n"
"  tap(aC, aW, vec2( 0.,-1.) - pp, dir, len2, lob, clp, b);\n"
"  tap(aC, aW, vec2( 1.,-1.) - pp, dir, len2, lob, clp, c);\n"
"  tap(aC, aW, vec2(-1., 1.) - pp, dir, len2, lob, clp, i);\n"
"  tap(aC, aW, vec2( 0., 1.) - pp, dir, len2, lob, clp, j);\n"
"  tap(aC, aW, vec2( 0., 0.) - pp, dir, len2, lob, clp, f);\n"
"  tap(aC, aW, vec2(-1., 0.) - pp, dir, len2, lob, clp, e);\n"
"  tap(aC, aW, vec2( 1., 1.) - pp, dir, len2, lob, clp, k);\n"
"  tap(aC, aW, vec2( 2., 1.) - pp, dir, len2, lob, clp, l);\n"
"  tap(aC, aW, vec2( 2., 0.) - pp, dir, len2, lob, clp, h);\n"
"  tap(aC, aW, vec2( 1., 0.) - pp, dir, len2, lob, clp, g);\n"
"  tap(aC, aW, vec2( 1., 2.) - pp, dir, len2, lob, clp, o);\n"
"  tap(aC, aW, vec2( 0., 2.) - pp, dir, len2, lob, clp, n);\n"
"  gl_FragColor = vec4(min(max4, max(min4, aC / aW)), 1.0);\n"
"}\n";

static const char *const rcas_src =
"#version 120\n"
"uniform sampler2D tex; uniform vec2 texSize; uniform vec2 origin; uniform float con;\n"
"vec3 ld(vec2 p) { return texture2D(tex, (p + 0.5) / texSize).rgb; }\n"
"void main() {\n"
"  vec2 sp = floor(gl_FragCoord.xy - origin);\n"
"  vec3 b = ld(sp + vec2(0., -1.)), d = ld(sp + vec2(-1., 0.)), e = ld(sp), f = ld(sp + vec2(1., 0.)), h = ld(sp + vec2(0., 1.));\n"
"  vec3 mn4 = min(min(b, d), min(f, h)), mx4 = max(max(b, d), max(f, h));\n"
"  vec3 hitMin = min(mn4, e) / (4.0 * max(mx4, 1e-5));\n"
"  vec3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, -1e-5);\n"
"  vec3 lobe3 = max(-hitMin, hitMax);\n"
"  float lobe = max(-0.1875, min(max(max(lobe3.r, lobe3.g), lobe3.b), 0.0)) * con;\n"
"  float rcpL = 1.0 / (4.0 * lobe + 1.0);\n"
"  gl_FragColor = vec4(clamp((lobe * (b + d + h + f) + e) * rcpL, 0.0, 1.0), 1.0);\n"
"}\n";

static GLuint make_program(const char *src, const char *what) {
    GLuint s = p_CreateShader(GL_FRAGMENT_SHADER);
    p_ShaderSource(s, 1, &src, NULL); p_CompileShader(s);
    GLint ok = 0; p_GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = ""; p_GetShaderInfoLog(s, sizeof log - 1, NULL, log);
        fprintf(stderr, "[video] FSR %s shader failed: %s\n", what, log);
        return 0;
    }
    GLuint p = p_CreateProgram(); p_AttachShader(p, s); p_LinkProgram(p);
    p_GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { fprintf(stderr, "[video] FSR %s shader did not link\n", what); return 0; }
    return p;
}
static int fsr_init(void) {
#define LOADGL(var, nm) (var = plat_gl_proc(nm))
    int ok = LOADGL(p_CreateShader, "glCreateShader") && LOADGL(p_ShaderSource, "glShaderSource") && LOADGL(p_CompileShader, "glCompileShader")
          && LOADGL(p_GetShaderiv, "glGetShaderiv") && LOADGL(p_GetShaderInfoLog, "glGetShaderInfoLog") && LOADGL(p_CreateProgram, "glCreateProgram")
          && LOADGL(p_AttachShader, "glAttachShader") && LOADGL(p_LinkProgram, "glLinkProgram") && LOADGL(p_GetProgramiv, "glGetProgramiv")
          && LOADGL(p_UseProgram, "glUseProgram") && LOADGL(p_GetUniformLocation, "glGetUniformLocation") && LOADGL(p_Uniform1i, "glUniform1i")
          && LOADGL(p_Uniform1f, "glUniform1f") && LOADGL(p_Uniform2f, "glUniform2f");
    if (ok) ok = (LOADGL(p_GenFramebuffers, "glGenFramebuffers") || LOADGL(p_GenFramebuffers, "glGenFramebuffersEXT"))
              && (LOADGL(p_BindFramebuffer, "glBindFramebuffer") || LOADGL(p_BindFramebuffer, "glBindFramebufferEXT"))
              && (LOADGL(p_FramebufferTexture2D, "glFramebufferTexture2D") || LOADGL(p_FramebufferTexture2D, "glFramebufferTexture2DEXT"))
              && (LOADGL(p_CheckFramebufferStatus, "glCheckFramebufferStatus") || LOADGL(p_CheckFramebufferStatus, "glCheckFramebufferStatusEXT"));
    if (ok) {
        fsr_easu = make_program(easu_src, "EASU"); fsr_rcas = make_program(rcas_src, "RCAS");
        ok = fsr_easu && fsr_rcas;
    }
    if (!ok) { fprintf(stderr, "[video] FSR is not available on this graphics driver; using the plain picture\n"); return -1; }
    p_GenFramebuffers(1, &fsr_fbo);
    glGenTextures(1, &fsr_tex);
    return 1;
}
/* Both passes; returns 0 if the picture should be drawn the plain way. */
static int fsr_present(const GfxFrame *f) {
    int cw = f->cw, ch = f->ch, w = f->w, h = f->h, fb_w = f->fb_w, fb_h = f->fb_h;
    if (fsr_state == 0) fsr_state = fsr_init();
    if (fsr_state < 0 || w <= fb_w || h <= fb_h) return 0;
    if (w != fsr_tw || h != fsr_th) {
        glBindTexture(GL_TEXTURE_2D, fsr_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        p_BindFramebuffer(GL_FRAMEBUFFER, fsr_fbo);
        p_FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fsr_tex, 0);
        GLenum st = p_CheckFramebufferStatus(GL_FRAMEBUFFER);
        p_BindFramebuffer(GL_FRAMEBUFFER, 0);
        if (st != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "[video] FSR render target is not usable; using the plain picture\n"); fsr_state = -1; glBindTexture(GL_TEXTURE_2D, gl_tex); return 0; }
        fsr_tw = w; fsr_th = h;
    }
    /* pass 1: EASU, input picture -> w x h target */
    glBindTexture(GL_TEXTURE_2D, gl_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    p_BindFramebuffer(GL_FRAMEBUFFER, fsr_fbo);
    glViewport(0, 0, w, h);
    p_UseProgram(fsr_easu);
    p_Uniform1i(p_GetUniformLocation(fsr_easu, "tex"), 0);
    p_Uniform2f(p_GetUniformLocation(fsr_easu, "inSize"), (float)fb_w, (float)fb_h);
    p_Uniform2f(p_GetUniformLocation(fsr_easu, "outSize"), (float)w, (float)h);
    glBegin(GL_QUADS); glVertex2f(-1, 1); glVertex2f(1, 1); glVertex2f(1, -1); glVertex2f(-1, -1); glEnd();
    p_BindFramebuffer(GL_FRAMEBUFFER, 0);
    /* pass 2: RCAS, target -> window */
    int ox = (cw - w) / 2, oy = (ch - h) / 2;
    glBindTexture(GL_TEXTURE_2D, fsr_tex);
    glViewport(ox, oy, w, h);
    p_UseProgram(fsr_rcas);
    p_Uniform1i(p_GetUniformLocation(fsr_rcas, "tex"), 0);
    p_Uniform2f(p_GetUniformLocation(fsr_rcas, "texSize"), (float)w, (float)h);
    p_Uniform2f(p_GetUniformLocation(fsr_rcas, "origin"), (float)ox, (float)oy);
    p_Uniform1f(p_GetUniformLocation(fsr_rcas, "con"), (float)exp2(-(1.0 - f->fsr_sharp / 100.0) * 2.0));
    glBegin(GL_QUADS); glVertex2f(-1, 1); glVertex2f(1, 1); glVertex2f(1, -1); glVertex2f(-1, -1); glEnd();
    p_UseProgram(0);
    glBindTexture(GL_TEXTURE_2D, gl_tex);
    return 1;
}
static void gl_setup(void) {
    glGenTextures(1, &gl_tex); glBindTexture(GL_TEXTURE_2D, gl_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);   /* clamp to edge */
    glEnable(GL_TEXTURE_2D); glDisable(GL_DEPTH_TEST); glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}
/* --shot-window: the next frame drawn is also copied out, from the back buffer before it is shown
   (reading the front buffer afterwards is not reliable on every system) */
static u32 *grab; static int grab_w, grab_h, grab_ok;
static void grab_and_swap(int cw, int ch, int want) {
    grab_ok = 0;
    if (want) {
        free(grab); grab = malloc((size_t)cw * ch * 4); grab_w = cw; grab_h = ch;
        if (grab) {
            grab_ok = 1;
            glReadBuffer(GL_BACK); glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glReadPixels(0, 0, cw, ch, 0x80E1, GL_UNSIGNED_BYTE, grab);
            for (int y = 0; y < ch / 2; y++) for (int x = 0; x < cw; x++) { u32 t = grab[y * cw + x]; grab[y * cw + x] = grab[(ch - 1 - y) * cw + x]; grab[(ch - 1 - y) * cw + x] = t; }
        }
    }
    plat_swap();
}
static void gl_present(const GfxFrame *f) {
    int cw = f->cw, ch = f->ch, w = f->w, h = f->h, fb_w = f->fb_w, fb_h = f->fb_h;
    glViewport(0, 0, cw, ch);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    if (f->on_gpu) {   /* the picture is already on the graphics card: copy the visible part into the texture */
        glg_present(f->box[0], f->box[1], f->box[2], f->box[3]);
        gl_tw = fb_w; gl_th = fb_h;
    } else if (fb_w != gl_tw || fb_h != gl_th) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, fb_w, fb_h, 0, 0x80E1 /* BGRA */, GL_UNSIGNED_BYTE, f->fb);
        gl_tw = fb_w; gl_th = fb_h;
    } else glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fb_w, fb_h, 0x80E1, GL_UNSIGNED_BYTE, f->fb);
    if (f->upscale && fsr_present(f)) { grab_and_swap(cw, ch, f->grab); return; }
    int flt = f->filter ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, flt); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, flt);
    float qx = (float)w / (float)cw, qy = (float)h / (float)ch;
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(-qx, qy);
    glTexCoord2f(1, 0); glVertex2f(qx, qy);
    glTexCoord2f(1, 1); glVertex2f(qx, -qy);
    glTexCoord2f(0, 1); glVertex2f(-qx, -qy);
    glEnd();
    grab_and_swap(cw, ch, f->grab);
}

static int gfx_gl_init(int w, int h, const char *title) {
    if (!plat_video_init(w, h, title)) return 0;   /* no OpenGL context: the platform has made a plain window instead */
    gl_setup();
    return 1;
}
static void gfx_gl_present(const GfxFrame *f) { gl_present(f); }
static const u32 *gfx_gl_grab(int *w, int *h) { if (!grab_ok) return NULL; grab_ok = 0; *w = grab_w; *h = grab_h; return grab; }   /* one-shot */

const GfxBackend gfx_gl_backend = {
    .name = "OpenGL", .hw = 1,
    .init = gfx_gl_init, .present = gfx_gl_present, .grab = gfx_gl_grab,
    .r_init = glg_init, .r_submit = glg_submit, .r_mask = glg_mask,
    .r_dirty = glg_dirty, .r_flush = glg_flush, .r_read = glg_read
};
