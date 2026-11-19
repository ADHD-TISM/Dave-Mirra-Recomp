/* The 2x / 4x picture drawn by the graphics card. See gpu_gl.h.
   Needs a compatibility-profile OpenGL context with shaders (GLSL 1.20), framebuffer
   objects and a packed depth-stencil attachment, i.e. OpenGL 3.0 or ARB_framebuffer_object
   (entry points are looked up at run time; anything less falls back to the CPU picture).
   Vram is mirrored in a 1024x512 two-channel texture (low byte, high byte) and the
   fragment shader decodes the 4-bit, 8-bit and 15-bit texels, palettes, modulation,
   dithering and blend modes itself. */
#ifdef DM_SDL
#include <SDL_opengl.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hw.h"
#include "host.h"
#include "gpu_gl.h"
#include "vram_cuts.h"
#ifndef APIENTRY
#define APIENTRY
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_TEXTURE1
#define GL_TEXTURE1 0x84C1
#endif
#ifndef GL_TEXTURE2
#define GL_TEXTURE2 0x84C2
#endif
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#endif
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
#ifndef GL_LUMINANCE8_ALPHA8
#define GL_LUMINANCE8_ALPHA8 0x8045
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_FUNC_ADD
#define GL_FUNC_ADD 0x8006
#endif
#ifndef GL_FUNC_REVERSE_SUBTRACT
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#endif
#ifndef GL_MAX_TEXTURE_SIZE
#define GL_MAX_TEXTURE_SIZE 0x0D33
#endif
#define GLG_DEPTH24_STENCIL8 0x88F0
#define GLG_DEPTH_STENCIL 0x84F9
#define GLG_UNSIGNED_INT_24_8 0x84FA
#define GLG_DEPTH_STENCIL_ATTACHMENT 0x821A

static GLuint (APIENTRY *q_CreateShader)(GLenum);
static void (APIENTRY *q_ShaderSource)(GLuint, GLsizei, const char *const *, const GLint *);
static void (APIENTRY *q_CompileShader)(GLuint), (APIENTRY *q_LinkProgram)(GLuint), (APIENTRY *q_UseProgram)(GLuint);
static void (APIENTRY *q_GetShaderiv)(GLuint, GLenum, GLint *), (APIENTRY *q_GetProgramiv)(GLuint, GLenum, GLint *);
static void (APIENTRY *q_GetShaderInfoLog)(GLuint, GLsizei, GLsizei *, char *);
static GLuint (APIENTRY *q_CreateProgram)(void);
static void (APIENTRY *q_AttachShader)(GLuint, GLuint);
static GLint (APIENTRY *q_GetUniformLocation)(GLuint, const char *);
static void (APIENTRY *q_Uniform1i)(GLint, GLint), (APIENTRY *q_Uniform1f)(GLint, GLfloat), (APIENTRY *q_Uniform2f)(GLint, GLfloat, GLfloat);
static void (APIENTRY *q_GenFramebuffers)(GLsizei, GLuint *);
static void (APIENTRY *q_BindFramebuffer)(GLenum, GLuint);
static void (APIENTRY *q_FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
static GLenum (APIENTRY *q_CheckFramebufferStatus)(GLenum);
static void (APIENTRY *q_ActiveTexture)(GLenum);
static void (APIENTRY *q_MultiTexCoord4f)(GLenum, GLfloat, GLfloat, GLfloat, GLfloat);
static void (APIENTRY *q_BlendEquation)(GLenum);

static const char *const vs_src =
"#version 120\n"
"uniform vec2 target;\n"
"varying vec4 t0, t1, t2; varying vec3 col;\n"
"void main() {\n"
"  gl_Position = vec4(gl_Vertex.x / target.x * 2.0 - 1.0, gl_Vertex.y / target.y * 2.0 - 1.0, 0.0, 1.0);\n"
"  col = gl_Color.rgb; t0 = gl_MultiTexCoord0; t1 = gl_MultiTexCoord1; t2 = gl_MultiTexCoord2;\n"
"}\n";

/* t0 = u, v, page x, page y; t1 = palette x, palette y, depth, flags (1 textured, 2 raw, 4 dither, 8 direct vram copy);
   t2 = texture window. pass: 0 everything, 1 only texels without the semi-transparency bit, 2 only those with it. */
static const char *const fs_src =
"#version 120\n"
"uniform sampler2D vram; uniform int pass; uniform float alpha;\n"
"varying vec4 t0, t1, t2; varying vec3 col;\n"
"float word(float x, float y) {\n"
"  x = mod(x, 1024.0); y = mod(y, 512.0);\n"
"  vec2 c = texture2D(vram, vec2((x + 0.5) / 1024.0, (y + 0.5) / 512.0)).ra * 255.0;\n"
"  return floor(c.x + 0.5) + floor(c.y + 0.5) * 256.0;\n"
"}\n"
"float win(float u, float m, float o) {\n"
"  float r = u, p = 1.0;\n"
"  for (int k = 0; k < 5; k++) {\n"
"    if (mod(floor(m / p), 2.0) > 0.5) { float b = 8.0 * p; r += (mod(floor(o / p), 2.0) - mod(floor(u / b), 2.0)) * b; }\n"
"    p *= 2.0;\n"
"  }\n"
"  return r;\n"
"}\n"
"float dth(vec2 f) {\n"
"  int xi = int(mod(floor(f.x), 4.0)), yi = int(mod(floor(f.y), 4.0));\n"
"  vec4 c;\n"
"  if (xi == 0) c = vec4(-4.0, 2.0, -3.0, 3.0); else if (xi == 1) c = vec4(0.0, -2.0, 1.0, -1.0);\n"
"  else if (xi == 2) c = vec4(-3.0, 3.0, -4.0, 2.0); else c = vec4(1.0, -1.0, 0.0, -2.0);\n"
"  if (yi == 0) return c.x; if (yi == 1) return c.y; if (yi == 2) return c.z; return c.w;\n"
"}\n"
"void main() {\n"
"  float fl = floor(t1.w + 0.5);\n"
"  bool textured = mod(fl, 2.0) > 0.5, raw = mod(floor(fl / 2.0), 2.0) > 0.5, dith = mod(floor(fl / 4.0), 2.0) > 0.5, direct = mod(floor(fl / 8.0), 2.0) > 0.5;\n"
"  vec3 c8 = floor(col * 255.0 + 0.5);\n"
"  vec3 c5;\n"
"  if (textured) {\n"
"    float depth = floor(t1.z + 0.5), tx = floor(t0.z + 0.5), ty = floor(t0.w + 0.5), cx = floor(t1.x + 0.5), cy = floor(t1.y + 0.5);\n"
"    float u = floor(t0.x + 0.002), v = floor(t0.y + 0.002), w;\n"
"    if (direct) { w = word(u, v); }\n"
"    else {\n"
"      u = mod(u, 256.0); v = mod(v, 256.0);\n"
"      float mx = floor(t2.x + 0.5), my = floor(t2.y + 0.5), ox = floor(t2.z + 0.5), oy = floor(t2.w + 0.5);\n"
"      if (mx > 0.5) u = win(u, mx, ox);\n"
"      if (my > 0.5) v = win(v, my, oy);\n"
"      if (depth < 0.5) { w = word(tx + floor(u / 4.0), ty + v); float ci = mod(floor(w / exp2(mod(u, 4.0) * 4.0)), 16.0); w = word(cx + ci, cy); }\n"
"      else if (depth < 1.5) { w = word(tx + floor(u / 2.0), ty + v); float ci = mod(floor(w / (mod(u, 2.0) > 0.5 ? 256.0 : 1.0)), 256.0); w = word(cx + ci, cy); }\n"
"      else w = word(tx + u, ty + v);\n"
"      if (w < 0.5) discard;\n"
"    }\n"
"    float t15 = step(32768.0, w);\n"
"    if (pass == 1 && t15 > 0.5) discard;\n"
"    if (pass == 2 && t15 < 0.5) discard;\n"
"    vec3 t5 = vec3(mod(w, 32.0), mod(floor(w / 32.0), 32.0), mod(floor(w / 1024.0), 32.0));\n"
"    if (raw || direct) c5 = t5;\n"
"    else {\n"
"      vec3 m = floor(t5 * 8.0 * c8 / 128.0);\n"
"      if (dith) m += dth(gl_FragCoord.xy);\n"
"      c5 = floor(clamp(m, 0.0, 255.0) / 8.0);\n"
"    }\n"
"  } else {\n"
"    vec3 m = c8;\n"
"    if (dith) m += dth(gl_FragCoord.xy);\n"
"    c5 = floor(clamp(m, 0.0, 255.0) / 8.0);\n"
"  }\n"
"  gl_FragColor = vec4(c5 * 8.0 / 255.0, alpha);\n"
"}\n";

static int state;                     /* 0 untried, 1 ready, -1 unavailable */
static int S;                         /* scale of the picture */
static int cur_chk;                   /* check-mask as last told to glg_mask (see there) */
static GLuint prog, fbo, fbo_tex, sten_tex, vtex, tmp_tex; static int tmp_w, tmp_h;
static GLint u_target, u_vram, u_pass, u_alpha;
static int dirty0 = 512, dirty1 = -1;

#define QMAX 8192
static GPrim *q; static int qn;

static GLuint compile(GLenum type, const char *src, const char *what) {
    GLuint s = q_CreateShader(type);
    q_ShaderSource(s, 1, &src, NULL); q_CompileShader(s);
    GLint ok = 0; q_GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = ""; q_GetShaderInfoLog(s, sizeof log - 1, NULL, log);
        fprintf(stderr, "[gpu_gl] %s shader failed: %s\n", what, log);
        return 0;
    }
    return s;
}

static int load(void) {
#define L(var, nm) (var = plat_gl_proc(nm))
    int ok = L(q_CreateShader, "glCreateShader") && L(q_ShaderSource, "glShaderSource") && L(q_CompileShader, "glCompileShader")
          && L(q_GetShaderiv, "glGetShaderiv") && L(q_GetShaderInfoLog, "glGetShaderInfoLog") && L(q_CreateProgram, "glCreateProgram")
          && L(q_AttachShader, "glAttachShader") && L(q_LinkProgram, "glLinkProgram") && L(q_GetProgramiv, "glGetProgramiv")
          && L(q_UseProgram, "glUseProgram") && L(q_GetUniformLocation, "glGetUniformLocation") && L(q_Uniform1i, "glUniform1i")
          && L(q_Uniform1f, "glUniform1f") && L(q_Uniform2f, "glUniform2f")
          && (L(q_ActiveTexture, "glActiveTexture") || L(q_ActiveTexture, "glActiveTextureARB"))
          && (L(q_MultiTexCoord4f, "glMultiTexCoord4f") || L(q_MultiTexCoord4f, "glMultiTexCoord4fARB"))
          && (L(q_BlendEquation, "glBlendEquation") || L(q_BlendEquation, "glBlendEquationEXT"))
          && (L(q_GenFramebuffers, "glGenFramebuffers") || L(q_GenFramebuffers, "glGenFramebuffersEXT"))
          && (L(q_BindFramebuffer, "glBindFramebuffer") || L(q_BindFramebuffer, "glBindFramebufferEXT"))
          && (L(q_FramebufferTexture2D, "glFramebufferTexture2D") || L(q_FramebufferTexture2D, "glFramebufferTexture2DEXT"))
          && (L(q_CheckFramebufferStatus, "glCheckFramebufferStatus") || L(q_CheckFramebufferStatus, "glCheckFramebufferStatusEXT"));
    return ok;
}

static int setup_once(void) {
    if (!load()) { fprintf(stderr, "[gpu_gl] this graphics driver lacks OpenGL shaders or framebuffers (it needs a compatibility-profile OpenGL 3.0 context); using the CPU picture\n"); return -1; }
    GLuint vs = compile(GL_VERTEX_SHADER, vs_src, "vertex"), fs = compile(GL_FRAGMENT_SHADER, fs_src, "fragment");
    if (!vs || !fs) return -1;
    prog = q_CreateProgram(); q_AttachShader(prog, vs); q_AttachShader(prog, fs); q_LinkProgram(prog);
    GLint ok = 0; q_GetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { fprintf(stderr, "[gpu_gl] shaders did not link; using the CPU picture\n"); return -1; }
    u_target = q_GetUniformLocation(prog, "target"); u_vram = q_GetUniformLocation(prog, "vram");
    u_pass = q_GetUniformLocation(prog, "pass"); u_alpha = q_GetUniformLocation(prog, "alpha");
    q_ActiveTexture(GL_TEXTURE1);
    glGenTextures(1, &vtex); glBindTexture(GL_TEXTURE_2D, vtex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE8_ALPHA8, 1024, 512, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, vram);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glGenTextures(1, &fbo_tex); glGenTextures(1, &tmp_tex); glGenTextures(1, &sten_tex);
    q_GenFramebuffers(1, &fbo);
    q_ActiveTexture(GL_TEXTURE0);
    q = malloc(sizeof *q * QMAX);
    return q ? 1 : -1;
}

int glg_init(int scale) {
    if (state < 0) return 0;
    if (state == 0) { state = setup_once(); if (state < 0) return 0; }
    GLint maxt = 0; glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxt);
    if (maxt < 1024 * scale) { fprintf(stderr, "[gpu_gl] the graphics driver cannot make a %d pixel wide picture; using the CPU picture\n", 1024 * scale); return 0; }
    GLint save0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &save0);
    q_ActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, fbo_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1024 * scale, 512 * scale, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, sten_tex);   /* the stencil bit is the console's mask bit (bit 15 of a vram pixel) */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GLG_DEPTH24_STENCIL8, 1024 * scale, 512 * scale, 0, GLG_DEPTH_STENCIL, GLG_UNSIGNED_INT_24_8, NULL);
    q_BindFramebuffer(GL_FRAMEBUFFER, fbo);
    q_FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo_tex, 0);
    q_FramebufferTexture2D(GL_FRAMEBUFFER, GLG_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, sten_tex, 0);
    GLenum st = q_CheckFramebufferStatus(GL_FRAMEBUFFER);
    q_BindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, vtex);
    q_ActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, (GLuint)save0);
    if (st != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "[gpu_gl] the picture target (a colour texture with a depth-stencil buffer) is not usable on this driver; using the CPU picture\n"); state = -1; return 0; }
    S = scale; qn = 0; tmp_w = tmp_h = 0;
    cur_chk = 0;   /* the stencil is new and undefined: gpu_set_scale's mask sync rebuilds it */
    dirty0 = 0; dirty1 = 511;
    fprintf(stderr, "[gpu_gl] drawing the %dx picture on the graphics card\n", scale);
    return 1;
}

void glg_dirty(int y0, int y1) {
    if (y0 < 0 || y1 > 511 || y1 - y0 >= 511) { dirty0 = 0; dirty1 = 511; return; }
    if (y0 < dirty0) dirty0 = y0;
    if (y1 > dirty1) dirty1 = y1;
}

/* ------------------------------------------------------------- drawing --- */
static void vert(const GPrim *p, int flags, float px, float py, float r, float g, float b, float u, float v) {
    glColor3f(r / 255.0f, g / 255.0f, b / 255.0f);
    q_MultiTexCoord4f(GL_TEXTURE0, u, v, (float)p->tx, (float)p->ty);
    q_MultiTexCoord4f(GL_TEXTURE1, (float)p->cx, (float)p->cy, (float)p->depth, (float)flags);
    q_MultiTexCoord4f(GL_TEXTURE2, (float)p->tw[0], (float)p->tw[1], (float)p->tw[2], (float)p->tw[3]);
    glVertex2f(px, py);
}
static void quad(const GPrim *p, int flags, float x0, float y0, float x1, float y1, float r, float g, float b,
                 float ul, float vt, float ur, float vb) {
    vert(p, flags, x0, y0, r, g, b, ul, vt); vert(p, flags, x1, y0, r, g, b, ur, vt); vert(p, flags, x1, y1, r, g, b, ur, vb);
    vert(p, flags, x0, y0, r, g, b, ul, vt); vert(p, flags, x1, y1, r, g, b, ur, vb); vert(p, flags, x0, y1, r, g, b, ul, vb);
}
static int pflags(const GPrim *p) {
    return (p->textured || p->kind == GK_BLIT ? 1 : 0) | (p->raw ? 2 : 0) | (p->dither ? 4 : 0) | (p->kind == GK_BLIT ? 8 : 0);
}
/* a box in vram wraps around the edges: up to four pieces */
static void pieces(int x, int y, int w, int h, int out[4][4], int *n) {
    int xs[2][2], ys[2][2], nx = 0, ny = 0;
    x &= 1023; y &= 511;
    xs[nx][0] = x; xs[nx][1] = w > 1024 - x ? 1024 - x : w; nx++;
    if (w > 1024 - x) { xs[nx][0] = 0; xs[nx][1] = w - (1024 - x); nx++; }
    ys[ny][0] = y; ys[ny][1] = h > 512 - y ? 512 - y : h; ny++;
    if (h > 512 - y) { ys[ny][0] = 0; ys[ny][1] = h - (512 - y); ny++; }
    *n = 0;
    for (int j = 0; j < ny; j++) for (int i = 0; i < nx; i++) {
        out[*n][0] = xs[i][0]; out[*n][1] = ys[j][0]; out[*n][2] = xs[i][1]; out[*n][3] = ys[j][1]; (*n)++;
    }
}
static void emit(const GPrim *p) {
    float s = (float)S;
    int fl = pflags(p);
    switch (p->kind) {
    case GK_TRI:
        for (int i = 0; i < 3; i++)   /* the picture is sampled at pixel corners, so corners sit half a pixel in */
            vert(p, fl, p->vx[i] * s + 0.5f, p->vy[i] * s + 0.5f, (float)p->vr[i], (float)p->vg[i], (float)p->vb[i], (float)p->vu[i], (float)p->vv[i]);
        break;
    case GK_RECT: {
        float ul = (float)p->u0, vt = (float)p->v0, ur, vb;
        if (p->flipx) ul += 1.0f - 0.004f;
        if (p->flipy) vt += 1.0f - 0.004f;
        ur = p->flipx ? ul - (float)p->w : ul + (float)p->w;
        vb = p->flipy ? vt - (float)p->h : vt + (float)p->h;
        quad(p, fl, p->x * s, p->y * s, (p->x + p->w) * s, (p->y + p->h) * s, (float)p->r, (float)p->g, (float)p->b, ul, vt, ur, vb);
        break;
    }
    case GK_PIX: case GK_FILL: {
        int pc[4][4], n;
        pieces(p->x, p->y, p->w, p->h, pc, &n);
        float r = (float)((p->col & 31) * 8), g = (float)(((p->col >> 5) & 31) * 8), b = (float)(((p->col >> 10) & 31) * 8);
        for (int i = 0; i < n; i++) quad(p, fl, pc[i][0] * s, pc[i][1] * s, (pc[i][0] + pc[i][2]) * s, (pc[i][1] + pc[i][3]) * s, r, g, b, 0, 0, 0, 0);
        break;
    }
    case GK_BLIT: {
        int pc[4][4], n;
        pieces(p->x, p->y, p->w, p->h, pc, &n);
        for (int i = 0; i < n; i++)
            quad(p, fl, pc[i][0] * s, pc[i][1] * s, (pc[i][0] + pc[i][2]) * s, (pc[i][1] + pc[i][3]) * s, 255, 255, 255,
                 (float)pc[i][0], (float)pc[i][1], (float)(pc[i][0] + pc[i][2]), (float)(pc[i][1] + pc[i][3]));
        break;
    }
    }
}

static void cliprect(const GPrim *p, int c[4]) {
    if (p->kind == GK_TRI || p->kind == GK_RECT) memcpy(c, p->clip, 4 * sizeof(int));
    else { c[0] = 0; c[1] = 0; c[2] = 1023; c[3] = 511; }
}
static int same_state(const GPrim *a, const GPrim *b) {
    if (a->chkm || b->chkm) return 0;   /* with the mask checked every prim is on its own: the stencil depends on drawing order */
    if (a->kind == GK_COPY || b->kind == GK_COPY || (a->semi && a->textured) || (b->semi && b->textured)) return 0;   /* translucent textured ones are drawn one at a time, in two passes */
    int ca[4], cb[4]; cliprect(a, ca); cliprect(b, cb);
    return memcmp(ca, cb, sizeof ca) == 0 && a->semi == b->semi && a->semimode == b->semimode && a->setm == b->setm && a->chkm == b->chkm
        && (a->kind == GK_FILL) == (b->kind == GK_FILL) && (a->kind == GK_BLIT) == (b->kind == GK_BLIT);
}
/* the mask bit, kept only while the mask is checked (see glg_mask): a fill ignores it and clears it, an upload mirrors bit 15 of vram
   afterwards (see glg_flush), the rest honour the mode */
static void set_stencil(const GPrim *f) {
    if (!f->chkm) { glDisable(GL_STENCIL_TEST); return; }   /* the stencil is only read under check-mask, and is rebuilt from vram when that starts */
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xff);
    if (f->kind == GK_FILL || f->kind == GK_BLIT) { glStencilFunc(GL_ALWAYS, 0, 0xff); glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE); }
    else { glStencilFunc(GL_NOTEQUAL, 1, 1); glStencilOp(GL_KEEP, GL_KEEP, f->setm ? GL_REPLACE : GL_KEEP); }
}
static void set_blend(int on, int mode) {
    if (!on) { glDisable(GL_BLEND); return; }
    glEnable(GL_BLEND);
    q_BlendEquation(mode == 2 ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD);
    switch (mode) {
    case 0: glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;   /* B/2 + F/2 */
    case 3: glBlendFunc(GL_SRC_ALPHA, GL_ONE); break;                   /* B + F/4 */
    default: glBlendFunc(GL_ONE, GL_ONE); break;                        /* B + F, B - F */
    }
}
static void draw_run(int a, int b, int pass, int blend) {
    const GPrim *f = &q[a];
    q_Uniform1i(u_pass, pass);
    q_Uniform1f(u_alpha, !blend ? 1.0f : f->semimode == 0 ? 0.5f : f->semimode == 3 ? 0.25f : 1.0f);
    set_blend(blend, f->semimode);
    glBegin(GL_TRIANGLES);
    for (int i = a; i < b; i++) emit(&q[i]);
    glEnd();
}

/* one box that wraps neither at the source nor at the destination */
static void copy_box(const GPrim *p, int sx, int sy, int dx, int dy, int w, int h) {   /* vram to vram, at the high resolution */
    GLint save; q_ActiveTexture(GL_TEXTURE0); glGetIntegerv(GL_TEXTURE_BINDING_2D, &save);
    glBindTexture(GL_TEXTURE_2D, tmp_tex);
    if (w * S > tmp_w || h * S > tmp_h) {
        tmp_w = w * S > tmp_w ? w * S : tmp_w; tmp_h = h * S > tmp_h ? h * S : tmp_h;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tmp_w, tmp_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, sx * S, sy * S, w * S, h * S);
    q_UseProgram(0); glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND); glEnable(GL_TEXTURE_2D);
    set_stencil(p);   /* the copied pixels' own bit 15 is put back by the stencil-only pass below */
    glColor4f(1, 1, 1, 1);
    float tx = (float)(w * S) / (float)tmp_w, ty = (float)(h * S) / (float)tmp_h;
    float W = 1024.0f * S, H = 512.0f * S;
    float x0 = (float)(dx * S) / W * 2 - 1, x1 = (float)((dx + w) * S) / W * 2 - 1;
    float y0 = (float)(dy * S) / H * 2 - 1, y1 = (float)((dy + h) * S) / H * 2 - 1;
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(x0, y0); glTexCoord2f(tx, 0); glVertex2f(x1, y0);
    glTexCoord2f(tx, ty); glVertex2f(x1, y1); glTexCoord2f(0, ty); glVertex2f(x0, y1);
    glEnd();
    glBindTexture(GL_TEXTURE_2D, (GLuint)save);
    q_UseProgram(prog);
    if (!p->chkm) return;
    /* the console copies bit 15 with the pixel, so mirror it from vram (already copied on the CPU), as an upload does */
    GPrim b; memset(&b, 0, sizeof b);
    b.kind = GK_BLIT; b.x = dx; b.y = dy; b.w = w; b.h = h;
    q_Uniform1i(u_pass, 2); q_Uniform1f(u_alpha, 1.0f);
    glStencilFunc(GL_NOTEQUAL, 1, 1);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glBegin(GL_TRIANGLES); emit(&b); glEnd();
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}
/* the console wraps a copy around the edges of vram, at the source and at the destination independently: cut it into boxes
   that wrap at neither, along each axis (see vram_cuts.h) */
static void do_copy(const GPrim *p) {
    int w = p->w < 1024 ? p->w : 1024, h = p->h < 512 ? p->h : 512;
    if (w <= 0 || h <= 0) return;
    int cx[3], cy[3], nx = cuts(p->sx, p->x, w, 1024, cx), ny = cuts(p->sy, p->y, h, 512, cy);
    for (int j = 0; j < ny; j++) for (int i = 0; i < nx; i++) {
        int x0 = cx[i], x1 = i + 1 < nx ? cx[i + 1] : w, y0 = cy[j], y1 = j + 1 < ny ? cy[j + 1] : h;
        copy_box(p, (p->sx + x0) & 1023, (p->sy + y0) & 511, (p->x + x0) & 1023, (p->y + y0) & 511, x1 - x0, y1 - y0);
    }
}

static void upload_dirty(void);
static void setup_target(void);
static int cur_set;
/* The stencil only matters while the mask is checked, so it is not kept up to date otherwise: when checking starts it is
   rebuilt from vram (whose bit 15 is the console's mask bit, kept exactly by the CPU). */
void glg_mask(int set, int check) {
    int c = check != 0;
    if (c && !cur_chk && state > 0) {
        glg_flush();   /* what is queued so far was drawn without checking */
        upload_dirty(); setup_target();
        glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND); glStencilMask(0xff); glClearStencil(0);
        glClear(GL_STENCIL_BUFFER_BIT);
        GPrim b; memset(&b, 0, sizeof b);
        b.kind = GK_BLIT; b.w = 1024; b.h = 512;
        glEnable(GL_STENCIL_TEST); glStencilFunc(GL_ALWAYS, 1, 0xff); glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        q_Uniform1i(u_pass, 2); q_Uniform1f(u_alpha, 1.0f);
        glBegin(GL_TRIANGLES); emit(&b); glEnd();
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDisable(GL_STENCIL_TEST);
        q_UseProgram(0); glColor4f(1, 1, 1, 1);   /* the window picture is drawn with the fixed pipeline */
        q_BindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    cur_set = set != 0; cur_chk = c;
}

/* do n pixels from s and from d (both wrapping at size) share any pixel? */
static int spans_meet(int s, int d, int n, int size) { return ((d - s) & (size - 1)) < n || ((s - d) & (size - 1)) < n; }

void glg_submit(const GPrim *p) {
    if (state <= 0) return;
    if (qn == QMAX) glg_flush();
    q[qn] = *p; q[qn].setm = cur_set; q[qn].chkm = cur_chk;
    /* The console copies a pixel at a time, so a copy whose source and destination overlap smears: the CPU copy in vram is exact
       about that, a copy of the picture is not. Such a copy is drawn from vram instead (the picture loses its extra detail there). */
    int smear = 0;
    if (p->kind == GK_COPY && spans_meet(p->sx, p->x, p->w, 1024) && spans_meet(p->sy, p->y, p->h, 512)) {
        /* Without wrapping, the pixel-at-a-time loop reads a pixel it has already written only when the destination is below the
           source, or level with it and to the right; up and left (a scroll) equals read-everything-then-write, which do_copy does. */
        int wraps = p->sx + p->w > 1024 || p->x + p->w > 1024 || p->sy + p->h > 512 || p->y + p->h > 512;
        smear = wraps || p->sy < p->y || (p->sy == p->y && p->sx < p->x);
    }
    if (smear) q[qn].kind = GK_BLIT;
    qn++;
    /* the copy's bit-15 pass reads vram as it is now; a drawn-from-vram copy must not see later drawing */
    if (smear || (cur_chk && p->kind == GK_COPY)) glg_flush();
}

/* bring the texture copy of vram up to date: the rows changed since last time */
static void upload_dirty(void) {
    q_ActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, vtex);
    if (dirty1 >= dirty0) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, dirty0, 1024, dirty1 - dirty0 + 1, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, &vram[dirty0 * 1024]);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        dirty0 = 512; dirty1 = -1;
    }
    q_ActiveTexture(GL_TEXTURE0);
}
static void setup_target(void) {
    q_BindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, 1024 * S, 512 * S);
    glDisable(GL_DEPTH_TEST);
    q_UseProgram(prog);
    q_Uniform1i(u_vram, 1);
    q_Uniform2f(u_target, 1024.0f * S, 512.0f * S);
}
void glg_flush(void) {
    if (state <= 0 || !qn) return;
    upload_dirty();
    setup_target();
    for (int i = 0; i < qn;) {
        const GPrim *f = &q[i];
        if (f->kind == GK_COPY) { do_copy(f); i++; continue; }
        int j = i + 1;
        while (j < qn && same_state(f, &q[j])) j++;
        int c[4]; cliprect(f, c);
        if (c[0] == 0 && c[1] == 0 && c[2] == 1023 && c[3] == 511) glDisable(GL_SCISSOR_TEST);
        else if (c[2] < c[0] || c[3] < c[1]) { i = j; continue; }   /* empty drawing area */
        else { glEnable(GL_SCISSOR_TEST); glScissor(c[0] * S, c[1] * S, (c[2] - c[0] + 1) * S, (c[3] - c[1] + 1) * S); }
        set_stencil(f);
        if (f->semi && f->textured) { draw_run(i, j, 1, 0); draw_run(i, j, 2, 1); }   /* opaque texels, then the translucent ones */
        else draw_run(i, j, 0, f->semi);
        if (f->chkm && (f->kind == GK_BLIT || (f->textured && !f->setm && (f->kind == GK_TRI || f->kind == GK_RECT)))) {   /* only under check-mask */
            /* texels with bit 15 keep it in vram (it is the mask bit): the same prims again, those texels only, stencil only.
               An upload does the same for its pixels. */
            if (f->kind != GK_BLIT) glStencilFunc(GL_NOTEQUAL, 1, 1);
            else glStencilFunc(GL_ALWAYS, 1, 0xff);
            glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            q_Uniform1i(u_pass, 2); q_Uniform1f(u_alpha, 1.0f); set_blend(0, 0);
            glBegin(GL_TRIANGLES);
            for (int k = i; k < j; k++) if (f->kind == GK_BLIT || q[k].textured) emit(&q[k]);   /* a run can mix in flat prims */
            glEnd();
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        }
        i = j;
    }
    qn = 0;
    q_UseProgram(0);
    glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST); q_BlendEquation(GL_FUNC_ADD);
    glColor4f(1, 1, 1, 1);   /* the window picture is drawn with the fixed pipeline: it would be tinted by the last vertex colour */
    q_BindFramebuffer(GL_FRAMEBUFFER, 0);
}

/* ------------------------------------------------------------- display --- */
/* The display box wraps around the edges of vram like the CPU picture does (a 640 wide box starting at x 512, say): cut one axis
   of it into the one or two runs that do not wrap. out[i] = { first vram coordinate, offset inside the box, length }. */
static int runs(int s, int n, int size, int out[2][3]) {
    s &= size - 1;
    if (n > size) n = size;
    out[0][0] = s; out[0][1] = 0; out[0][2] = n < size - s ? n : size - s;
    if (n <= size - s) return 1;
    out[1][0] = 0; out[1][1] = size - s; out[1][2] = n - (size - s);
    return 2;
}
void glg_present(int x, int y, int w, int h) {
    if (state <= 0 || w <= 0 || h <= 0) return;
    if (w > 1024) w = 1024;
    if (h > 512) h = 512;
    int rx[2][3], ry[2][3], nx = runs(x, w, 1024, rx), ny = runs(y, h, 512, ry);
    q_ActiveTexture(GL_TEXTURE0);
    q_BindFramebuffer(GL_FRAMEBUFFER, fbo);
    if (nx == 1 && ny == 1) glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, rx[0][0] * S, ry[0][0] * S, w * S, h * S, 0);
    else {   /* the texture is made at the full size, then filled a piece at a time */
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w * S, h * S, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
        for (int j = 0; j < ny; j++) for (int i = 0; i < nx; i++)
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, rx[i][1] * S, ry[j][1] * S, rx[i][0] * S, ry[j][0] * S, rx[i][2] * S, ry[j][2] * S);
    }
    q_BindFramebuffer(GL_FRAMEBUFFER, 0);
}
void glg_read(int x, int y, int w, int h, unsigned *dst) {
    if (state <= 0 || w <= 0 || h <= 0) return;
    if (w > 1024) w = 1024;
    if (h > 512) h = 512;
    int rx[2][3], ry[2][3], nx = runs(x, w, 1024, rx), ny = runs(y, h, 512, ry);
    q_BindFramebuffer(GL_FRAMEBUFFER, fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, w * S);   /* the pieces land in one picture w * S wide */
    for (int j = 0; j < ny; j++) for (int i = 0; i < nx; i++)
        glReadPixels(rx[i][0] * S, ry[j][0] * S, rx[i][2] * S, ry[j][2] * S, GL_BGRA, GL_UNSIGNED_BYTE,
                     dst + (size_t)(ry[j][1] * S) * (size_t)(w * S) + (size_t)(rx[i][1] * S));
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    q_BindFramebuffer(GL_FRAMEBUFFER, 0);
}
