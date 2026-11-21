/* Shared helpers for the core-profile OpenGL code (gfx_gl.c, gpu_gl.c): vertices sent through one streamed buffer instead of
   glBegin, the shader compile and link, and the plain textured-quad program. Needs an OpenGL 3.3 context (any profile). */
#ifndef GL_STREAM_H
#define GL_STREAM_H

/* One vertex layout for everything: position, colour and three 4-vectors of attributes (locations 0 to 4 in the shaders). The
   window picture's quads use only x, y and t0[0..1] (the texture coordinate). */
typedef struct { float x, y, c[3], t0[4], t1[4], t2[4]; } GlsVert;

int      gls_init(void);                                    /* load the entry points, make the vertex array and buffer; 1 when usable */
void     gls_draw(unsigned mode, const GlsVert *v, int n);  /* draw n vertices (GL_TRIANGLES, say) */
void     gls_quad(float xl, float yt, float xr, float yb, float ul, float vt, float ur, float vb);   /* two triangles; positions in clip space */
unsigned gls_program(const char *vs, const char *fs, const char *what);   /* compile, link; 0 (and a message on stderr) when it fails */
void     gls_use(unsigned prog);
unsigned gls_plain_program(void);                           /* draws texture unit 0 at t0.xy over a quad; 0 when it cannot be made */
extern const char gls_vs_plain[];                           /* vertex shader: position as given (clip space), t0.xy out as "uv" */
#endif
