/* See gl_stream.h. */
#ifdef DM_SDL
#include <SDL_opengl.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#endif
#include <stddef.h>
#include <stdio.h>
#include "host.h"
#include "gl_stream.h"
#ifndef APIENTRY
#define APIENTRY
#endif

#define GLS_ARRAY_BUFFER 0x8892
#define GLS_STREAM_DRAW 0x88E0
#define GLS_VERTEX_SHADER 0x8B31
#define GLS_FRAGMENT_SHADER 0x8B30
#define GLS_COMPILE_STATUS 0x8B81
#define GLS_LINK_STATUS 0x8B82

static void (APIENTRY *f_GenVertexArrays)(int, unsigned *), (APIENTRY *f_BindVertexArray)(unsigned);
static void (APIENTRY *f_GenBuffers)(int, unsigned *), (APIENTRY *f_BindBuffer)(unsigned, unsigned);
static void (APIENTRY *f_BufferData)(unsigned, ptrdiff_t, const void *, unsigned);
static void (APIENTRY *f_VertexAttribPointer)(unsigned, int, unsigned, unsigned char, int, const void *);
static void (APIENTRY *f_EnableVertexAttribArray)(unsigned);
static unsigned (APIENTRY *f_CreateShader)(unsigned);
static void (APIENTRY *f_ShaderSource)(unsigned, int, const char *const *, const int *);
static void (APIENTRY *f_CompileShader)(unsigned), (APIENTRY *f_LinkProgram)(unsigned), (APIENTRY *f_UseProgram)(unsigned), (APIENTRY *f_DeleteShader)(unsigned);
static void (APIENTRY *f_GetShaderiv)(unsigned, unsigned, int *), (APIENTRY *f_GetProgramiv)(unsigned, unsigned, int *);
static void (APIENTRY *f_GetShaderInfoLog)(unsigned, int, int *, char *);
static unsigned (APIENTRY *f_CreateProgram)(void);
static void (APIENTRY *f_AttachShader)(unsigned, unsigned);
static int (APIENTRY *f_GetUniformLocation)(unsigned, const char *);
static void (APIENTRY *f_Uniform1i)(int, int);

static int state;   /* 0 untried, 1 ready, -1 unavailable */
static unsigned vao, vbo, plain_prog; static int plain_state;

#define L(var, nm) ((var) = plat_gl_proc(nm))
int gls_init(void) {
    if (state) return state > 0;
    state = -1;
    int ok = L(f_GenVertexArrays, "glGenVertexArrays") && L(f_BindVertexArray, "glBindVertexArray") && L(f_GenBuffers, "glGenBuffers")
          && L(f_BindBuffer, "glBindBuffer") && L(f_BufferData, "glBufferData") && L(f_VertexAttribPointer, "glVertexAttribPointer")
          && L(f_EnableVertexAttribArray, "glEnableVertexAttribArray") && L(f_CreateShader, "glCreateShader")
          && L(f_ShaderSource, "glShaderSource") && L(f_CompileShader, "glCompileShader") && L(f_LinkProgram, "glLinkProgram")
          && L(f_UseProgram, "glUseProgram") && L(f_DeleteShader, "glDeleteShader") && L(f_GetShaderiv, "glGetShaderiv")
          && L(f_GetProgramiv, "glGetProgramiv") && L(f_GetShaderInfoLog, "glGetShaderInfoLog") && L(f_CreateProgram, "glCreateProgram")
          && L(f_AttachShader, "glAttachShader") && L(f_GetUniformLocation, "glGetUniformLocation") && L(f_Uniform1i, "glUniform1i");
    if (!ok) return 0;
    f_GenVertexArrays(1, &vao); f_BindVertexArray(vao);   /* one vertex array and one buffer, bound for the life of the context */
    f_GenBuffers(1, &vbo); f_BindBuffer(GLS_ARRAY_BUFFER, vbo);
    static const int size[5] = { 2, 3, 4, 4, 4 }, off[5] = { 0, 2, 5, 9, 13 };
    for (unsigned i = 0; i < 5; i++) {
        f_VertexAttribPointer(i, size[i], GL_FLOAT, 0, (int)sizeof(GlsVert), (const void *)((ptrdiff_t)off[i] * 4));
        f_EnableVertexAttribArray(i);
    }
    state = 1;
    return 1;
}

void gls_draw(unsigned mode, const GlsVert *v, int n) {
    if (state <= 0 || n <= 0) return;
    f_BindBuffer(GLS_ARRAY_BUFFER, vbo);
    f_BufferData(GLS_ARRAY_BUFFER, (ptrdiff_t)n * (ptrdiff_t)sizeof *v, v, GLS_STREAM_DRAW);
    glDrawArrays(mode, 0, n);
}
void gls_quad(float xl, float yt, float xr, float yb, float ul, float vt, float ur, float vb) {
    GlsVert v[6] = { 0 };
    float px[4] = { xl, xr, xr, xl }, py[4] = { yt, yt, yb, yb }, pu[4] = { ul, ur, ur, ul }, pv[4] = { vt, vt, vb, vb };
    static const int idx[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; i++) { v[i].x = px[idx[i]]; v[i].y = py[idx[i]]; v[i].t0[0] = pu[idx[i]]; v[i].t0[1] = pv[idx[i]]; }
    gls_draw(GL_TRIANGLES, v, 6);
}

static unsigned compile(unsigned type, const char *src, const char *what) {
    unsigned s = f_CreateShader(type);
    f_ShaderSource(s, 1, &src, NULL); f_CompileShader(s);
    int ok = 0; f_GetShaderiv(s, GLS_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = ""; f_GetShaderInfoLog(s, sizeof log - 1, NULL, log);
        fprintf(stderr, "[video] %s shader failed: %s\n", what, log);
        f_DeleteShader(s);
        return 0;
    }
    return s;
}
unsigned gls_program(const char *vs, const char *fs, const char *what) {
    if (!gls_init()) return 0;
    unsigned v = compile(GLS_VERTEX_SHADER, vs, what), f = v ? compile(GLS_FRAGMENT_SHADER, fs, what) : 0;
    if (!v || !f) { if (v) f_DeleteShader(v); return 0; }
    unsigned p = f_CreateProgram(); f_AttachShader(p, v); f_AttachShader(p, f); f_LinkProgram(p);
    f_DeleteShader(v); f_DeleteShader(f);
    int ok = 0; f_GetProgramiv(p, GLS_LINK_STATUS, &ok);
    if (!ok) { fprintf(stderr, "[video] %s shader did not link\n", what); return 0; }
    return p;
}
void gls_use(unsigned prog) { if (state > 0) f_UseProgram(prog); }

const char gls_vs_plain[] =
"#version 330 core\n"
"layout(location = 0) in vec2 pos; layout(location = 2) in vec4 t0;\n"
"out vec2 uv;\n"
"void main() { gl_Position = vec4(pos, 0.0, 1.0); uv = t0.xy; }\n";

static const char plain_fs[] =
"#version 330 core\n"
"in vec2 uv; uniform sampler2D tex; out vec4 fragColor;\n"
"void main() { fragColor = texture(tex, uv); }\n";

unsigned gls_plain_program(void) {
    if (plain_state == 0) {
        plain_state = -1;
        plain_prog = gls_program(gls_vs_plain, plain_fs, "picture");
        if (plain_prog) { f_UseProgram(plain_prog); f_Uniform1i(f_GetUniformLocation(plain_prog, "tex"), 0); plain_state = 1; }
    }
    return plain_state > 0 ? plain_prog : 0;
}
