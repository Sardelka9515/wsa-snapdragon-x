/*
 * Stand-in for WSA's /vendor/lib64/egl/libGLESv2_emulation.so.
 *
 * Every exported function forwards to the real driver (gles_forwarders.c,
 * loaded from libGLESv2_emulation_real.so), except the ones defined here.
 *
 * Fix (always built): VAO emulation for the packages in debug.gles.vaoemu (see
 * "VAO emulation" below). WSA mishandles vertex array objects that an app keeps
 * deleting and re-creating: after a while some meshes read their attributes
 * wrongly and the world tears into long spikes (Bully: Anniversary Edition).
 *
 * Diagnostics (Build-Module.ps1 -Diagnostics, -DGLES_DIAG), log tag "gles-seq":
 *   adb shell setprop debug.gles.trace 1        record the next MAX_EVENTS framebuffer
 *                                               calls, draws collapsed with their signature
 *   adb shell setprop debug.gles.skip "m5 s2"   drop draws whose signature has these tokens
 *   adb shell setprop debug.gles.reupload 1     re-send every buffer's recorded contents
 * (any new value of trace/reupload triggers again; skip "none" turns the filter off)
 */

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLsizei, GLint;
typedef unsigned char GLboolean;
typedef long GLintptr, GLsizeiptr;

#define RTLD_NOW                    2
#define REAL_DRIVER                 "/vendor/lib64/egl/libGLESv2_emulation_real.so"
#define GL_DRAW_FRAMEBUFFER         0x8CA9
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_CURRENT_PROGRAM          0x8B8D
#define GL_COLOR_ATTACHMENT0        0x8CE0
#define GL_ATT_OBJECT_TYPE          0x8CD0
#define GL_ATT_OBJECT_NAME          0x8CD1
#define GL_TEXTURE                  0x1702
#define GL_ACTIVE_TEXTURE           0x84E0
#define GL_TEXTURE0                 0x84C0
#define MAX_EVENTS                  600

extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int __system_property_get(const char *, char *);
extern int gettid(void);
extern int pthread_mutex_lock(void *);
extern int pthread_mutex_unlock(void *);
extern int snprintf(char *, unsigned long, const char *, ...);

#include "gles_forwarders.c"

static void *(*r_getproc)(const char *);
static void (*r_getintegerv)(GLenum, GLint *);
static void (*r_glGetVertexAttribiv)(GLuint, GLenum, GLint *);
static void (*r_glGetVertexAttribPointerv)(GLuint, GLenum, void **);
static void (*r_getattach)(GLenum, GLenum, GLenum, GLint *);
static int (*r_log)(int, const char *, const char *, ...);

#define LOG(...) do { if (r_log) r_log(5 /*WARN*/, "gles-seq", __VA_ARGS__); } while (0)

/* ---- recording state (one recording at a time; races only cost log lines) ---- */

static volatile int g_remaining;           /* events left to log in this recording */
static unsigned g_calls;                   /* calls since the last property check */
static char g_last_value[92];
static unsigned g_draws;                   /* identical draws collapsed since the last logged line */
static GLint g_draw_fbo;
static char g_draw_sig[320];

/* debug.gles.skip: space-separated tokens; draws whose signature contains any of them are dropped */
static char g_skip_value[92];
static char g_skip[8][24];
static int g_nskip;

static void parse_skip(const char *v) {
    g_nskip = 0;
    for (const char *p = v; *p && g_nskip < 8;) {
        while (*p == ' ' || *p == ',') p++;
        int n = 0;
        while (p[n] && p[n] != ' ' && p[n] != ',' && n < 23) n++;
        if (n && !(n == 4 && !__builtin_memcmp(p, "none", 4))) {
            __builtin_memcpy(g_skip[g_nskip], p, n);
            g_skip[g_nskip][n] = 0;
            g_nskip++;
        }
        p += n;
    }
}

/* debug.gles.reupload: on change, re-send every shadowed buffer (see "buffer shadows" below) */
static char g_reupload_value[92];
static volatile int g_reupload_pending;

static int recording(void) {
#ifndef GLES_DIAG
    return 0;                              /* everyday build: no recording, skipping or re-uploads */
#endif
    if ((++g_calls & 1023) == 0) {         /* poll the properties every 1024 calls */
        char r[92] = { 0 };
        __system_property_get("debug.gles.reupload", r);
        if (__builtin_strcmp(r, g_reupload_value)) {
            __builtin_memcpy(g_reupload_value, r, sizeof(r));
            if (r[0]) g_reupload_pending = 1;
        }
        char v[92] = { 0 };
        int len = __system_property_get("debug.gles.trace", v);
        static int polls;
        if (polls++ == 0) LOG("first poll: debug.gles.trace='%s' (len %d)", v, len);
        if (v[0] && __builtin_strcmp(v, g_last_value)) {
            __builtin_memcpy(g_last_value, v, sizeof(v));
            g_remaining = MAX_EVENTS;
            g_draws = 0;
            LOG("---- recording %d events (debug.gles.trace=%s) ----", MAX_EVENTS, v);
        }
        char s[92] = { 0 };
        __system_property_get("debug.gles.skip", s);
        if (__builtin_strcmp(s, g_skip_value)) {
            __builtin_memcpy(g_skip_value, s, sizeof(s));
            parse_skip(s);
            LOG("skip filter now '%s' (%d tokens)", s, g_nskip);
        }
    }
    return g_remaining > 0;
}

static GLint get_int(GLenum e) { GLint v = 0; r_getintegerv(e, &v); return v; }

static void flush_draws(void) {
    if (g_draws) {
        LOG("[%d]   draw x%u fbo %d: %s", gettid(), g_draws, g_draw_fbo, g_draw_sig);
        g_draws = 0;
        g_remaining--;
    }
}

#define EVENT(...) do { flush_draws(); LOG(__VA_ARGS__); g_remaining--; } while (0)

static GLint attached_tex(GLenum att) {
    GLint type = 0, name = 0;
    r_getattach(GL_DRAW_FRAMEBUFFER, att, GL_ATT_OBJECT_TYPE, &type);
    if (!type) return 0;
    r_getattach(GL_DRAW_FRAMEBUFFER, att, GL_ATT_OBJECT_NAME, &name);
    return type == GL_TEXTURE ? name : -name;   /* negative = renderbuffer */
}

#define REAL(name, ret, ...) static ret (*r_##name)(__VA_ARGS__);
#define EXPORT __attribute__((visibility("default")))

/* ---- buffer shadows: a CPU copy of every buffer's contents, so they can be re-sent ---- */

extern void *realloc(void *, unsigned long);
extern void free(void *);

#define MAX_SHADOW 65536
typedef struct { unsigned char *p; long size; GLenum usage; int valid; } shadow_t;
static shadow_t g_shadow[MAX_SHADOW];
static long g_shadow_bytes;
static int g_shadow_overflow;

REAL(glBufferData, void, GLenum, GLsizeiptr, const void *, GLenum)
REAL(glBufferSubData, void, GLenum, GLintptr, GLsizeiptr, const void *)
REAL(glDeleteBuffers, void, GLsizei, const GLuint *)
REAL(glMapBufferRange, void *, GLenum, GLintptr, GLsizeiptr, GLbitfield)
static void (*r_glBindBuffer)(GLenum, GLuint);

static GLenum binding_of(GLenum target) {
    switch (target) {
    case 0x8892: return 0x8894;            /* ARRAY_BUFFER */
    case 0x8893: return 0x8895;            /* ELEMENT_ARRAY_BUFFER */
    case 0x8A11: return 0x8A28;            /* UNIFORM_BUFFER */
    case 0x88EB: return 0x88ED;            /* PIXEL_PACK */
    case 0x88EC: return 0x88EF;            /* PIXEL_UNPACK */
    case 0x8C8E: return 0x8C8F;            /* TRANSFORM_FEEDBACK */
    default: return target;                /* COPY_READ/COPY_WRITE: binding enum == target */
    }
}

static shadow_t *shadow_for(GLenum target) {
    GLint id = get_int(binding_of(target));
    if (id <= 0) return 0;
    if (id >= MAX_SHADOW) { g_shadow_overflow++; return 0; }
    return &g_shadow[id];
}

EXPORT void glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage) {
    r_glBufferData(target, size, data, usage);
#ifdef GLES_DIAG
    shadow_t *s = shadow_for(target);
    if (!s) return;
    unsigned char *p = realloc(s->p, size > 0 ? (unsigned long)size : 1);
    if (!p) { s->valid = 0; return; }
    g_shadow_bytes += size - s->size;
    s->p = p; s->size = size; s->usage = usage; s->valid = 1;
    if (data) __builtin_memcpy(p, data, size);
    else __builtin_memset(p, 0, size);
#endif
}

EXPORT void glBufferSubData(GLenum target, GLintptr off, GLsizeiptr size, const void *data) {
    r_glBufferSubData(target, off, size, data);
#ifdef GLES_DIAG
    shadow_t *s = shadow_for(target);
    if (s && s->valid && data && off >= 0 && off + size <= s->size) __builtin_memcpy(s->p + off, data, size);
#endif
}

EXPORT void *glMapBufferRange(GLenum target, GLintptr off, GLsizeiptr len, GLbitfield access) {
#ifdef GLES_DIAG
    shadow_t *s = shadow_for(target);
    if (s && (access & 0x0002 /*MAP_WRITE_BIT*/)) s->valid = 0;   /* contents now unknown */
#endif
    return r_glMapBufferRange(target, off, len, access);
}

static void vao_forget_buffers(GLsizei n, const GLuint *ids);

EXPORT void glDeleteBuffers(GLsizei n, const GLuint *ids) {
    vao_forget_buffers(n, ids);
#ifdef GLES_DIAG
    for (GLsizei k = 0; ids && k < n; k++) {
        if (ids[k] && ids[k] < MAX_SHADOW) {
            shadow_t *s = &g_shadow[ids[k]];
            g_shadow_bytes -= s->size;
            free(s->p);
            s->p = 0; s->size = 0; s->valid = 0;
        }
    }
#endif
    r_glDeleteBuffers(n, ids);
}

/* Re-specify every shadowed buffer with its recorded contents (on the GL thread) */
static void reupload_all(void) {
    GLint saved = get_int(0x8F37 /*COPY_WRITE_BUFFER_BINDING*/);
    int count = 0, skipped = 0;
    long bytes = 0;
    for (int id = 1; id < MAX_SHADOW; id++) {
        shadow_t *s = &g_shadow[id];
        if (!s->p) continue;
        if (!s->valid) { skipped++; continue; }
        r_glBindBuffer(0x8F37, (GLuint)id);
        r_glBufferData(0x8F37, s->size, s->p, s->usage);
        count++;
        bytes += s->size;
    }
    r_glBindBuffer(0x8F37, (GLuint)saved);
    LOG("[%d] re-uploaded %d buffers (%ld KB), skipped %d unknown; shadows total %ld KB, %d ids too large",
        gettid(), count, bytes / 1024, skipped, g_shadow_bytes / 1024, g_shadow_overflow);
}

REAL(glBindFramebuffer, void, GLenum, GLuint)
REAL(glDrawBuffers, void, GLsizei, const GLenum *)
REAL(glReadBuffer, void, GLenum)
REAL(glFramebufferTexture2D, void, GLenum, GLenum, GLenum, GLuint, GLint)
REAL(glFramebufferRenderbuffer, void, GLenum, GLenum, GLenum, GLuint)
REAL(glClear, void, GLbitfield)
REAL(glColorMask, void, GLboolean, GLboolean, GLboolean, GLboolean)
REAL(glBlitFramebuffer, void, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)
REAL(glCopyTexSubImage2D, void, GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei)
REAL(glInvalidateFramebuffer, void, GLenum, GLsizei, const GLenum *)
REAL(glBindTexture, void, GLenum, GLuint)
REAL(glGenerateMipmap, void, GLenum)
REAL(glDrawElements, void, GLenum, GLsizei, GLenum, const void *)
REAL(glDrawRangeElements, void, GLenum, GLuint, GLuint, GLsizei, GLenum, const void *)
REAL(glDrawElementsInstanced, void, GLenum, GLsizei, GLenum, const void *, GLsizei)
REAL(glDrawArrays, void, GLenum, GLint, GLsizei)
REAL(glDrawArraysInstanced, void, GLenum, GLint, GLsizei, GLsizei)

EXPORT void glBindFramebuffer(GLenum target, GLuint fb) {
    if (g_reupload_pending) { g_reupload_pending = 0; reupload_all(); }
    r_glBindFramebuffer(target, fb);
    if (recording()) {
        if (fb && (target == 0x8D40 || target == GL_DRAW_FRAMEBUFFER))
            EVENT("[%d] bind fbo %u (target %#x)  att0=%d att1=%d", gettid(), fb, target,
                  attached_tex(GL_COLOR_ATTACHMENT0), attached_tex(GL_COLOR_ATTACHMENT0 + 1));
        else
            EVENT("[%d] bind fbo %u (target %#x)", gettid(), fb, target);
    }
}

EXPORT void glDrawBuffers(GLsizei n, const GLenum *b) {
    if (recording())
        EVENT("[%d] drawbuffers n=%d {%#x %#x %#x %#x} on fbo %d", gettid(), n, n > 0 ? b[0] : 0, n > 1 ? b[1] : 0,
              n > 2 ? b[2] : 0, n > 3 ? b[3] : 0, get_int(GL_DRAW_FRAMEBUFFER_BINDING));
    r_glDrawBuffers(n, b);
}

EXPORT void glReadBuffer(GLenum src) {
    if (recording()) EVENT("[%d] readbuffer %#x (read fbo %d)", gettid(), src, get_int(GL_READ_FRAMEBUFFER_BINDING));
    r_glReadBuffer(src);
}

EXPORT void glFramebufferTexture2D(GLenum target, GLenum att, GLenum textarget, GLuint tex, GLint level) {
    if (recording()) EVENT("[%d] attach %#x = tex %u (level %d) target %#x", gettid(), att, tex, level, target);
    r_glFramebufferTexture2D(target, att, textarget, tex, level);
}

EXPORT void glFramebufferRenderbuffer(GLenum target, GLenum att, GLenum rbt, GLuint rb) {
    if (recording()) EVENT("[%d] attach %#x = renderbuffer %u target %#x", gettid(), att, rb, target);
    r_glFramebufferRenderbuffer(target, att, rbt, rb);
}

EXPORT void glClear(GLbitfield mask) {
    if (recording()) EVENT("[%d] clear %#x (fbo %d)", gettid(), mask, get_int(GL_DRAW_FRAMEBUFFER_BINDING));
    r_glClear(mask);
}

EXPORT void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    if (recording()) EVENT("[%d] colormask %d%d%d%d", gettid(), r, g, b, a);
    r_glColorMask(r, g, b, a);
}

EXPORT void glBlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1,
                              GLbitfield mask, GLenum filter) {
    if (recording())
        EVENT("[%d] blit read fbo %d -> draw fbo %d mask %#x (%dx%d -> %dx%d)", gettid(),
              get_int(GL_READ_FRAMEBUFFER_BINDING), get_int(GL_DRAW_FRAMEBUFFER_BINDING), mask,
              sx1 - sx0, sy1 - sy0, dx1 - dx0, dy1 - dy0);
    r_glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
}

EXPORT void glCopyTexSubImage2D(GLenum t, GLint l, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h) {
    if (recording()) EVENT("[%d] copytexsubimage2d %dx%d from read fbo %d", gettid(), w, h, get_int(GL_READ_FRAMEBUFFER_BINDING));
    r_glCopyTexSubImage2D(t, l, xo, yo, x, y, w, h);
}

EXPORT void glInvalidateFramebuffer(GLenum target, GLsizei n, const GLenum *a) {
    if (recording()) EVENT("[%d] invalidate n=%d first %#x (fbo %d)", gettid(), n, n > 0 ? a[0] : 0, get_int(GL_DRAW_FRAMEBUFFER_BINDING));
    r_glInvalidateFramebuffer(target, n, a);
}

EXPORT void glBindTexture(GLenum target, GLuint tex) {
    /* not logged: far too frequent (passed through for completeness) */
    r_glBindTexture(target, tex);
}

EXPORT void glGenerateMipmap(GLenum target) {
    if (recording()) EVENT("[%d] generatemipmap", gettid());
    r_glGenerateMipmap(target);
}

/*
 * Draw signature, e.g. "m4 iS p400 0:f3 1:s2 2:b4n 3:B4c":
 * mode, index type, program, then each enabled vertex attribute as
 * index:type+size (+n normalized, +I integer, +c client memory).
 * Types: b/B byte/ubyte, s/S short/ushort, i/I int/uint, f float, h half, r 2_10_10_10.
 */
static const char *type_char(GLint t) {
    switch (t) {
    case 0x1400: return "b"; case 0x1401: return "B"; case 0x1402: return "s"; case 0x1403: return "S";
    case 0x1404: return "i"; case 0x1405: return "I"; case 0x1406: return "f";
    case 0x140B: case 0x8D61: return "h"; case 0x8D9F: case 0x8368: return "r";
    default: return "x";
    }
}

static void build_sig(GLenum mode, GLenum itype, char *out, unsigned long cap) {
    int n = snprintf(out, cap, "m%x i%s p%d", mode, itype ? type_char((GLint)itype) : "-", get_int(GL_CURRENT_PROGRAM));
    for (GLuint a = 0; a < 8 && n > 0 && (unsigned long)n < cap - 16; a++) {
        GLint en = 0, sz = 0, ty = 0, no = 0, in = 0, buf = 0;
        r_glGetVertexAttribiv(a, 0x8622 /*ENABLED*/, &en);
        if (!en) continue;
        r_glGetVertexAttribiv(a, 0x8623 /*SIZE*/, &sz);
        r_glGetVertexAttribiv(a, 0x8625 /*TYPE*/, &ty);
        r_glGetVertexAttribiv(a, 0x886A /*NORMALIZED*/, &no);
        r_glGetVertexAttribiv(a, 0x88FD /*INTEGER*/, &in);
        r_glGetVertexAttribiv(a, 0x889F /*BUFFER_BINDING*/, &buf);
        n += snprintf(out + n, cap - n, " %u:%s%d%s%s%s", a, type_char(ty), sz, no ? "n" : "", in ? "I" : "", buf ? "" : "c");
    }
    /* byte offset/stride of each enabled attribute, in brackets so skip tokens don't match them */
    for (GLuint a = 0; a < 8 && n > 0 && (unsigned long)n < cap - 16; a++) {
        GLint en = 0, stride = 0;
        void *ptr = 0;
        r_glGetVertexAttribiv(a, 0x8622 /*ENABLED*/, &en);
        if (!en) continue;
        r_glGetVertexAttribiv(a, 0x8624 /*STRIDE*/, &stride);
        r_glGetVertexAttribPointerv(a, 0x8645 /*POINTER*/, &ptr);
        n += snprintf(out + n, cap - n, " [%u@%ld/%d]", a, (long)ptr, stride);
    }
}

/* A skip token matches a signature word exactly, or the part after "index:" ("s2" matches "1:s2") */
static int word_eq(const char *w, const char *tok) {
    int i = 0;
    while (tok[i] && w[i] && w[i] != ' ' && w[i] == tok[i]) i++;
    return !tok[i] && (!w[i] || w[i] == ' ');
}

static int sig_matches(const char *sig) {
    for (const char *w = sig; *w;) {
        for (int k = 0; k < g_nskip; k++) {
            if (word_eq(w, g_skip[k])) return 1;
            const char *c = w;
            while (*c && *c != ' ' && *c != ':') c++;
            if (*c == ':' && word_eq(c + 1, g_skip[k])) return 1;
        }
        while (*w && *w != ' ') w++;
        while (*w == ' ') w++;
    }
    return 0;
}

static int g_draw_skipped;

/* Returns 1 if this draw should be dropped (debug.gles.skip) */
static int draw_hook(GLenum mode, GLenum itype) {
    int rec = recording();
    if (!rec && !g_nskip) return 0;
    char sig[320];
    build_sig(mode, itype, sig, sizeof(sig));
    int skip = g_nskip && sig_matches(sig);
    if (rec) {
        GLint fbo = get_int(GL_DRAW_FRAMEBUFFER_BINDING);
        if (g_draws && (fbo != g_draw_fbo || skip != g_draw_skipped || __builtin_strcmp(sig, g_draw_sig))) flush_draws();
        if (!g_draws) {
            g_draw_fbo = fbo;
            g_draw_skipped = skip;
            __builtin_memcpy(g_draw_sig, sig, sizeof(sig));
            if (skip) __builtin_memcpy(g_draw_sig + __builtin_strlen(g_draw_sig), " SKIPPED", 9);
        }
        g_draws++;
    }
    return skip;
}

EXPORT void glDrawElements(GLenum m, GLsizei c, GLenum t, const void *i) { if (!draw_hook(m, t)) r_glDrawElements(m, c, t, i); }
EXPORT void glDrawRangeElements(GLenum m, GLuint s, GLuint e, GLsizei c, GLenum t, const void *i) { if (!draw_hook(m, t)) r_glDrawRangeElements(m, s, e, c, t, i); }
EXPORT void glDrawElementsInstanced(GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n) { if (!draw_hook(m, t)) r_glDrawElementsInstanced(m, c, t, i, n); }
EXPORT void glDrawArrays(GLenum m, GLint f, GLsizei c) { if (!draw_hook(m, 0)) r_glDrawArrays(m, f, c); }
EXPORT void glDrawArraysInstanced(GLenum m, GLint f, GLsizei c, GLsizei n) { if (!draw_hook(m, 0)) r_glDrawArraysInstanced(m, f, c, n); }

/*
 * ---- VAO emulation ----
 * For the packages listed in debug.gles.vaoemu (read when the app first uses a
 * VAO), vertex array objects are emulated here: the app gets fake VAO names,
 * WSA only ever sees VAO 0, and binding a VAO re-applies its recorded vertex
 * attribute state to VAO 0. This sidesteps WSA's own VAO handling.
 */
#define MAX_ATTRIBS 16
#define MAX_VAOS    4096
#define MAX_CTX     8
#define GL_ARRAY_BUFFER          0x8892
#define GL_ELEMENT_ARRAY_BUFFER  0x8893
#define GL_ARRAY_BUFFER_BINDING  0x8894
#define GL_VERTEX_ARRAY_BINDING  0x85B5

typedef struct { GLint enabled, size, integer, normalized, stride; GLenum type; const void *ptr; GLuint buffer, divisor; } vattr_t;
typedef struct { void *ctx; GLuint name; int used; GLuint elem; vattr_t a[MAX_ATTRIBS]; } vao_t;
typedef struct { void *ctx; GLuint cur; vao_t applied; int applied_valid; } ctxstate_t;

static vao_t g_vaos[MAX_VAOS];
static ctxstate_t g_ctxs[MAX_CTX];
static long g_vao_lock[8];
static GLuint g_next_vao = 1;
static int g_vao_emu = -1;                 /* -1: not decided yet */
static void *(*r_curctx)(void);

extern int open(const char *, int, ...);
extern long read(int, void *, unsigned long);
extern int close(int);

REAL(glGenVertexArrays, void, GLsizei, GLuint *)
REAL(glDeleteVertexArrays, void, GLsizei, const GLuint *)
REAL(glBindVertexArray, void, GLuint)
REAL(glIsVertexArray, GLboolean, GLuint)
REAL(glVertexAttribPointer, void, GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)
REAL(glVertexAttribIPointer, void, GLuint, GLint, GLenum, GLsizei, const void *)
REAL(glEnableVertexAttribArray, void, GLuint)
REAL(glDisableVertexAttribArray, void, GLuint)
REAL(glVertexAttribDivisor, void, GLuint, GLuint)
REAL(glGetIntegerv, void, GLenum, GLint *)

static int vao_emu_enabled(void) {
    if (g_vao_emu >= 0) return g_vao_emu;
    char list[92] = { 0 }, cmd[128] = { 0 };
    __system_property_get("debug.gles.vaoemu", list);
    int fd = open("/proc/self/cmdline", 0);
    if (fd >= 0) { read(fd, cmd, sizeof(cmd) - 1); close(fd); }
    int on = 0;
    unsigned long cl = __builtin_strlen(cmd);
    for (const char *p = list; *p && cl;) {         /* comma/space separated package names */
        while (*p == ',' || *p == ' ') p++;
        unsigned long n = 0;
        while (p[n] && p[n] != ',' && p[n] != ' ') n++;
        if (n == cl && !__builtin_memcmp(p, cmd, n)) on = 1;
        p += n;
    }
    g_vao_emu = on;
    if (on) LOG("[%d] VAO emulation ON for %s", gettid(), cmd);
    return on;
}

static void default_vao(vao_t *v) {
    __builtin_memset(v->a, 0, sizeof(v->a));
    for (int i = 0; i < MAX_ATTRIBS; i++) { v->a[i].size = 4; v->a[i].type = 0x1406; }
    v->elem = 0;
}

/* Caller holds g_vao_lock */
static vao_t *find_vao(void *ctx, GLuint name, int create) {
    vao_t *free_slot = 0;
    for (int i = 0; i < MAX_VAOS; i++) {
        if (g_vaos[i].used) { if (g_vaos[i].ctx == ctx && g_vaos[i].name == name) return &g_vaos[i]; }
        else if (!free_slot) free_slot = &g_vaos[i];
    }
    if (!create || !free_slot) return 0;
    free_slot->used = 1; free_slot->ctx = ctx; free_slot->name = name;
    default_vao(free_slot);
    return free_slot;
}

static ctxstate_t *ctx_state(void *ctx) {
    ctxstate_t *free_slot = 0;
    for (int i = 0; i < MAX_CTX; i++) {
        if (g_ctxs[i].ctx == ctx) return &g_ctxs[i];
        if (!g_ctxs[i].ctx && !free_slot) free_slot = &g_ctxs[i];
    }
    if (!free_slot) return 0;
    __builtin_memset(free_slot, 0, sizeof(*free_slot));
    free_slot->ctx = ctx;
    return free_slot;
}

/* Current (emulated) VAO of this thread's context; name 0 has its own record too */
static vao_t *cur_vao(ctxstate_t **cs_out) {
    void *ctx = r_curctx ? r_curctx() : 0;
    if (!ctx) return 0;
    ctxstate_t *cs = ctx_state(ctx);
    if (!cs) return 0;
    if (cs_out) *cs_out = cs;
    return find_vao(ctx, cs->cur, 1);
}

/* Make the real VAO 0 match v, sending only what differs from what was applied last */
static void apply_vao(ctxstate_t *cs, const vao_t *v) {
    GLint saved_array = get_int(GL_ARRAY_BUFFER_BINDING);
    GLuint bound = (GLuint)saved_array;
    for (GLuint i = 0; i < MAX_ATTRIBS; i++) {
        const vattr_t *a = &v->a[i];
        vattr_t *was = &cs->applied.a[i];
        if (cs->applied_valid && !__builtin_memcmp(a, was, sizeof(*a))) continue;
        if (a->ptr || a->buffer || a->enabled) {
            if (bound != a->buffer) { r_glBindBuffer(GL_ARRAY_BUFFER, a->buffer); bound = a->buffer; }
            if (a->integer) r_glVertexAttribIPointer(i, a->size, a->type, a->stride, a->ptr);
            else r_glVertexAttribPointer(i, a->size, a->type, (GLboolean)a->normalized, a->stride, a->ptr);
        }
        if (a->enabled) r_glEnableVertexAttribArray(i); else r_glDisableVertexAttribArray(i);
        r_glVertexAttribDivisor(i, a->divisor);
        *was = *a;
    }
    if (!cs->applied_valid || cs->applied.elem != v->elem) {
        r_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, v->elem);
        cs->applied.elem = v->elem;
    }
    if (bound != (GLuint)saved_array) r_glBindBuffer(GL_ARRAY_BUFFER, (GLuint)saved_array);
    cs->applied_valid = 1;
}

EXPORT void glGenVertexArrays(GLsizei n, GLuint *arrays) {
    if (!vao_emu_enabled()) { r_glGenVertexArrays(n, arrays); return; }
    void *ctx = r_curctx ? r_curctx() : 0;
    pthread_mutex_lock(g_vao_lock);
    for (GLsizei k = 0; arrays && k < n; k++) {
        GLuint name = g_next_vao++;
        if (!find_vao(ctx, name, 1)) LOG("VAO table full");
        arrays[k] = name;
    }
    pthread_mutex_unlock(g_vao_lock);
}

EXPORT void glDeleteVertexArrays(GLsizei n, const GLuint *arrays) {
    if (!vao_emu_enabled()) { r_glDeleteVertexArrays(n, arrays); return; }
    pthread_mutex_lock(g_vao_lock);
    ctxstate_t *cs = 0;
    vao_t *cur = cur_vao(&cs);
    void *ctx = cs ? cs->ctx : 0;
    for (GLsizei k = 0; arrays && k < n; k++) {
        if (!arrays[k]) continue;
        vao_t *v = find_vao(ctx, arrays[k], 0);
        if (!v) continue;
        if (cs && cs->cur == arrays[k]) {          /* deleting the bound VAO binds 0 */
            cs->cur = 0;
            apply_vao(cs, find_vao(ctx, 0, 1));
        }
        v->used = 0;
    }
    (void)cur;
    pthread_mutex_unlock(g_vao_lock);
}

EXPORT void glBindVertexArray(GLuint name) {
    if (!vao_emu_enabled()) { r_glBindVertexArray(name); return; }
    pthread_mutex_lock(g_vao_lock);
    ctxstate_t *cs = 0;
    cur_vao(&cs);
    if (cs) {
        vao_t *v = find_vao(cs->ctx, name, name == 0);
        if (v) { cs->cur = name; apply_vao(cs, v); }
    }
    pthread_mutex_unlock(g_vao_lock);
}

EXPORT GLboolean glIsVertexArray(GLuint name) {
    if (!vao_emu_enabled()) return r_glIsVertexArray(name);
    pthread_mutex_lock(g_vao_lock);
    void *ctx = r_curctx ? r_curctx() : 0;
    GLboolean r = name && find_vao(ctx, name, 0) ? 1 : 0;
    pthread_mutex_unlock(g_vao_lock);
    return r;
}

/* Record attribute state into the bound emulated VAO, and pass it through to VAO 0 */
#define RECORD(body) do { if (vao_emu_enabled()) { pthread_mutex_lock(g_vao_lock); ctxstate_t *cs = 0; \
    vao_t *v = cur_vao(&cs); if (v && i < MAX_ATTRIBS) { vattr_t *a = &v->a[i]; body; if (cs) cs->applied.a[i] = *a; } \
    pthread_mutex_unlock(g_vao_lock); } } while (0)

EXPORT void glVertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void *p) {
    r_glVertexAttribPointer(i, size, type, norm, stride, p);
    RECORD(a->size = size; a->type = type; a->normalized = norm; a->integer = 0; a->stride = stride; a->ptr = p;
           a->buffer = (GLuint)get_int(GL_ARRAY_BUFFER_BINDING));
}

EXPORT void glVertexAttribIPointer(GLuint i, GLint size, GLenum type, GLsizei stride, const void *p) {
    r_glVertexAttribIPointer(i, size, type, stride, p);
    RECORD(a->size = size; a->type = type; a->normalized = 0; a->integer = 1; a->stride = stride; a->ptr = p;
           a->buffer = (GLuint)get_int(GL_ARRAY_BUFFER_BINDING));
}

EXPORT void glEnableVertexAttribArray(GLuint i) { r_glEnableVertexAttribArray(i); RECORD(a->enabled = 1); }
EXPORT void glDisableVertexAttribArray(GLuint i) { r_glDisableVertexAttribArray(i); RECORD(a->enabled = 0); }
EXPORT void glVertexAttribDivisor(GLuint i, GLuint d) { r_glVertexAttribDivisor(i, d); RECORD(a->divisor = d); }

EXPORT void glBindBuffer(GLenum target, GLuint buf) {
    r_glBindBuffer(target, buf);
    if (target == GL_ELEMENT_ARRAY_BUFFER && vao_emu_enabled()) {
        pthread_mutex_lock(g_vao_lock);
        ctxstate_t *cs = 0;
        vao_t *v = cur_vao(&cs);
        if (v) { v->elem = buf; if (cs) cs->applied.elem = buf; }
        pthread_mutex_unlock(g_vao_lock);
    }
}

EXPORT void glGetIntegerv(GLenum pname, GLint *data) {
    if (pname == GL_VERTEX_ARRAY_BINDING && data && g_vao_emu == 1) {
        pthread_mutex_lock(g_vao_lock);
        void *ctx = r_curctx ? r_curctx() : 0;
        ctxstate_t *cs = ctx ? ctx_state(ctx) : 0;
        *data = cs ? (GLint)cs->cur : 0;
        pthread_mutex_unlock(g_vao_lock);
        return;
    }
    r_glGetIntegerv(pname, data);
}

/* Deleting a buffer detaches it from the bound VAO (GL spec); mirror that in the emulated one */
static void vao_forget_buffers(GLsizei n, const GLuint *ids) {
    if (g_vao_emu != 1 || !ids) return;
    pthread_mutex_lock(g_vao_lock);
    ctxstate_t *cs = 0;
    vao_t *v = cur_vao(&cs);
    for (GLsizei k = 0; v && k < n; k++) {
        if (!ids[k]) continue;
        for (int i = 0; i < MAX_ATTRIBS; i++)
            if (v->a[i].buffer == ids[k]) { v->a[i].buffer = 0; if (cs) cs->applied.a[i].buffer = 0; }
        if (v->elem == ids[k]) { v->elem = 0; if (cs) cs->applied.elem = 0; }
    }
    pthread_mutex_unlock(g_vao_lock);
}

/* name -> wrapper, for getProcAddress */
#define ENTRY(name) { #name, (void *)name, (void **)&r_##name }
static const struct { const char *name; void *ours; void **real; } g_traced[] = {
    ENTRY(glBindFramebuffer), ENTRY(glDrawBuffers), ENTRY(glReadBuffer), ENTRY(glFramebufferTexture2D),
    ENTRY(glFramebufferRenderbuffer), ENTRY(glClear), ENTRY(glColorMask), ENTRY(glBlitFramebuffer),
    ENTRY(glCopyTexSubImage2D), ENTRY(glInvalidateFramebuffer), ENTRY(glBindTexture), ENTRY(glGenerateMipmap),
    ENTRY(glDrawElements), ENTRY(glDrawRangeElements), ENTRY(glDrawElementsInstanced), ENTRY(glDrawArrays),
    ENTRY(glDrawArraysInstanced), ENTRY(glBufferData), ENTRY(glBufferSubData), ENTRY(glDeleteBuffers),
    ENTRY(glMapBufferRange), ENTRY(glGenVertexArrays), ENTRY(glDeleteVertexArrays), ENTRY(glBindVertexArray),
    ENTRY(glIsVertexArray), ENTRY(glVertexAttribPointer), ENTRY(glVertexAttribIPointer),
    ENTRY(glEnableVertexAttribArray), ENTRY(glDisableVertexAttribArray), ENTRY(glVertexAttribDivisor),
    ENTRY(glBindBuffer), ENTRY(glGetIntegerv),
};
#define N_TRACED (int)(sizeof(g_traced) / sizeof(g_traced[0]))

__attribute__((constructor))
static void load_real(void) {
    void *h = dlopen(REAL_DRIVER, RTLD_NOW);
    r_log = dlsym(dlopen("liblog.so", RTLD_NOW), "__android_log_print");
    if (!h) { LOG("cannot load %s", REAL_DRIVER); return; }
    for (int i = 0; i < N_FWD; i++) g_fwd[i] = dlsym(h, g_fwd_names[i]);
    for (int i = 0; i < N_TRACED; i++) *g_traced[i].real = dlsym(h, g_traced[i].name);
    r_getintegerv = dlsym(h, "glGetIntegerv");
    r_glGetVertexAttribiv = dlsym(h, "glGetVertexAttribiv");
    r_glGetVertexAttribPointerv = dlsym(h, "glGetVertexAttribPointerv");
    r_glBindBuffer = dlsym(h, "glBindBuffer");
    __system_property_get("debug.gles.reupload", g_reupload_value);
    r_getattach   = dlsym(h, "glGetFramebufferAttachmentParameteriv");
    r_getproc     = dlsym(h, "_Z14getProcAddressPKc");
    r_curctx      = dlsym(dlopen("libEGL.so", RTLD_NOW), "eglGetCurrentContext");
    /* don't replay a recording requested before this process started */
    int len = __system_property_get("debug.gles.trace", g_last_value);
    LOG("loaded (%d forwarded, %d traced); debug.gles.trace='%s' (len %d)", N_FWD, N_TRACED, g_last_value, len);
}

/*
 * The real driver's C++ getProcAddress(const char *), which WSA's EGL driver
 * uses to hand out GL entry points (eglGetProcAddress and Android's loader).
 * Without this, callers get the real functions and bypass the wrapper.
 */
EXPORT void *gles_getProcAddress(const char *name) __asm__("_Z14getProcAddressPKc");
void *gles_getProcAddress(const char *name) {
    if (name) {
        if (!__builtin_strcmp(name, "glDrawBuffersEXT")) return (void *)glDrawBuffers;
        if (!__builtin_strcmp(name, "glGenVertexArraysOES")) return (void *)glGenVertexArrays;
        if (!__builtin_strcmp(name, "glDeleteVertexArraysOES")) return (void *)glDeleteVertexArrays;
        if (!__builtin_strcmp(name, "glBindVertexArrayOES")) return (void *)glBindVertexArray;
        if (!__builtin_strcmp(name, "glIsVertexArrayOES")) return (void *)glIsVertexArray;
        if (!__builtin_strcmp(name, "glVertexAttribDivisorEXT")) return (void *)glVertexAttribDivisor;
        for (int i = 0; i < N_TRACED; i++)
            if (!__builtin_strcmp(name, g_traced[i].name)) return g_traced[i].ours;
    }
    return r_getproc ? r_getproc(name) : 0;
}
