/*
 * Stand-in for WSA's /vendor/lib64/egl/libGLESv2_emulation.so.
 *
 * Every exported function forwards to the real driver (gles_forwarders.c,
 * loaded from libGLESv2_emulation_real.so). The functions defined here are
 * passed through unchanged too, but each distinct way an app calls them is
 * logged once (tag "gles-wrapper"), to find what WSA's GL translation gets
 * wrong for a given game.
 */

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLsizei, GLint;
typedef unsigned char GLboolean;
typedef long GLintptr, GLsizeiptr;

#define RTLD_NOW                    2
#define REAL_DRIVER                 "/vendor/lib64/egl/libGLESv2_emulation_real.so"
#define GL_ARRAY_BUFFER_BINDING     0x8894
#define GL_ELEMENT_ARRAY_BUFFER_BINDING 0x8895

extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int pthread_mutex_lock(void *);
extern int pthread_mutex_unlock(void *);

#include "gles_forwarders.c"

static void *(*r_getproc)(const char *);
static void (*r_getintegerv)(GLenum, GLint *);
static int (*r_log)(int, const char *, const char *, ...);

#define LOG(...) do { if (r_log) r_log(5 /*WARN*/, "gles-wrapper", __VA_ARGS__); } while (0)

/* ---- log each distinct call pattern once ---- */

static unsigned long g_seen[512];
static int g_nseen;
static long g_lock[8];   /* bionic: zero-initialised pthread_mutex_t */

static int first_time(unsigned long key) {
    int is_new = 1;
    pthread_mutex_lock(g_lock);
    for (int i = 0; i < g_nseen; i++) if (g_seen[i] == key) { is_new = 0; break; }
    if (is_new && g_nseen < 512) g_seen[g_nseen++] = key;
    else if (is_new) is_new = 0;          /* table full: stop logging */
    pthread_mutex_unlock(g_lock);
    return is_new;
}

static unsigned long mix(unsigned long h, unsigned long v) { return (h ^ v) * 0x100000001b3ul; }
static GLint binding(GLenum which) { GLint b = 0; r_getintegerv(which, &b); return b; }

/* ---- traced functions: declare real pointer + wrapper ---- */

#define REAL(name, ret, ...) static ret (*r_##name)(__VA_ARGS__);

REAL(glVertexAttribPointer, void, GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)
REAL(glVertexAttribIPointer, void, GLuint, GLint, GLenum, GLsizei, const void *)
REAL(glDrawElements, void, GLenum, GLsizei, GLenum, const void *)
REAL(glDrawRangeElements, void, GLenum, GLuint, GLuint, GLsizei, GLenum, const void *)
REAL(glDrawElementsInstanced, void, GLenum, GLsizei, GLenum, const void *, GLsizei)
REAL(glDrawArrays, void, GLenum, GLint, GLsizei)
REAL(glDrawArraysInstanced, void, GLenum, GLint, GLsizei, GLsizei)
REAL(glMapBufferRange, void *, GLenum, GLintptr, GLsizeiptr, GLbitfield)
REAL(glFlushMappedBufferRange, void, GLenum, GLintptr, GLsizeiptr)
REAL(glBufferData, void, GLenum, GLsizeiptr, const void *, GLenum)
REAL(glBufferSubData, void, GLenum, GLintptr, GLsizeiptr, const void *)
REAL(glVertexAttribDivisor, void, GLuint, GLuint)
REAL(glBindVertexArray, void, GLuint)

#define EXPORT __attribute__((visibility("default")))

REAL(glEnable, void, GLenum)
REAL(glDisable, void, GLenum)

/* Which vertex attribs were last pointed at client memory (approximate: ignores VAO switches) */
static unsigned g_client_attribs;
static int g_restart_enabled;

EXPORT void glEnable(GLenum cap) {
    if (cap == 0x8D69 /*GL_PRIMITIVE_RESTART_FIXED_INDEX*/) g_restart_enabled = 1;
    if (first_time(mix(10, cap))) LOG("glEnable %#x", cap);
    r_glEnable(cap);
}

EXPORT void glDisable(GLenum cap) {
    if (cap == 0x8D69) g_restart_enabled = 0;
    if (first_time(mix(11, cap))) LOG("glDisable %#x", cap);
    r_glDisable(cap);
}

EXPORT void glVertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void *p) {
    GLint vbo = binding(GL_ARRAY_BUFFER_BINDING);
    if (i < 32) { if (vbo) g_client_attribs &= ~(1u << i); else g_client_attribs |= 1u << i; }
    if (first_time(mix(mix(mix(1, size), type), norm * 2 + (vbo == 0))))
        LOG("glVertexAttribPointer size=%d type=%#x normalized=%d stride=%d %s", size, type, norm, stride,
            vbo ? "(buffer)" : "(CLIENT MEMORY)");
    r_glVertexAttribPointer(i, size, type, norm, stride, p);
}

EXPORT void glVertexAttribIPointer(GLuint i, GLint size, GLenum type, GLsizei stride, const void *p) {
    GLint vbo = binding(GL_ARRAY_BUFFER_BINDING);
    if (first_time(mix(mix(mix(2, size), type), vbo == 0)))
        LOG("glVertexAttribIPointer size=%d type=%#x stride=%d %s", size, type, stride, vbo ? "(buffer)" : "(CLIENT MEMORY)");
    r_glVertexAttribIPointer(i, size, type, stride, p);
}

static void log_draw(const char *fn, GLenum mode, GLenum type, int instanced) {
    GLint ibo = type ? binding(GL_ELEMENT_ARRAY_BUFFER_BINDING) : 1;
    if (first_time(mix(mix(mix(mix(mix(3, (unsigned long)fn), mode), type), ibo == 0), g_client_attribs != 0)))
        LOG("%s mode=%#x index type=%#x %s%s%s", fn, mode, type, type && !ibo ? "(indices in CLIENT MEMORY)" : "",
            instanced ? " instanced" : "", g_client_attribs ? " +client vertex attribs" : "");
}

EXPORT void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *idx) {
    log_draw("glDrawElements", mode, type, 0);
    if (type == 0x1403 /*GL_UNSIGNED_SHORT*/ && idx && count > 0 && !binding(GL_ELEMENT_ARRAY_BUFFER_BINDING)) {
        const unsigned short *ix = idx;
        unsigned mn = 0xffff, mx = 0, restarts = 0;
        for (GLsizei k = 0; k < count; k++) {
            if (ix[k] == 0xffff) { restarts++; continue; }
            if (ix[k] < mn) mn = ix[k];
            if (ix[k] > mx) mx = ix[k];
        }
        if (first_time(mix(mix(mix(20, mode), restarts != 0), (mn != 0) * 2 + (g_client_attribs != 0))))
            LOG("  client indices mode=%#x count=%d min=%u max=%u restart(0xffff)=%u primitive-restart %s, client attribs %#x",
                mode, count, mn, mx, restarts, g_restart_enabled ? "ENABLED" : "disabled", g_client_attribs);
    }
    r_glDrawElements(mode, count, type, idx);
}

EXPORT void glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *idx) {
    log_draw("glDrawRangeElements", mode, type, 0);
    r_glDrawRangeElements(mode, start, end, count, type, idx);
}

EXPORT void glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void *idx, GLsizei n) {
    log_draw("glDrawElementsInstanced", mode, type, 1);
    r_glDrawElementsInstanced(mode, count, type, idx, n);
}

EXPORT void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    log_draw("glDrawArrays", mode, 0, 0);
    r_glDrawArrays(mode, first, count);
}

EXPORT void glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei n) {
    log_draw("glDrawArraysInstanced", mode, 0, 1);
    r_glDrawArraysInstanced(mode, first, count, n);
}

EXPORT void *glMapBufferRange(GLenum target, GLintptr off, GLsizeiptr len, GLbitfield access) {
    if (first_time(mix(mix(4, target), access)))
        LOG("glMapBufferRange target=%#x access=%#x (len %ld)", target, access, (long)len);
    return r_glMapBufferRange(target, off, len, access);
}

EXPORT void glFlushMappedBufferRange(GLenum target, GLintptr off, GLsizeiptr len) {
    if (first_time(mix(5, target))) LOG("glFlushMappedBufferRange target=%#x", target);
    r_glFlushMappedBufferRange(target, off, len);
}

EXPORT void glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage) {
    if (first_time(mix(mix(mix(6, target), usage), data == 0)))
        LOG("glBufferData target=%#x usage=%#x %s", target, usage, data ? "with data" : "no data");
    r_glBufferData(target, size, data, usage);
}

EXPORT void glBufferSubData(GLenum target, GLintptr off, GLsizeiptr size, const void *data) {
    if (first_time(mix(7, target))) LOG("glBufferSubData target=%#x", target);
    r_glBufferSubData(target, off, size, data);
}

EXPORT void glVertexAttribDivisor(GLuint i, GLuint d) {
    if (first_time(mix(8, d != 0))) LOG("glVertexAttribDivisor divisor=%u", d);
    r_glVertexAttribDivisor(i, d);
}

EXPORT void glBindVertexArray(GLuint vao) {
    if (first_time(mix(9, vao != 0))) LOG("glBindVertexArray %s", vao ? "(VAO)" : "(0)");
    r_glBindVertexArray(vao);
}

/* name -> wrapper, for getProcAddress */
#define ENTRY(name) { #name, (void *)name, (void **)&r_##name }
static const struct { const char *name; void *ours; void **real; } g_traced[] = {
    ENTRY(glVertexAttribPointer), ENTRY(glVertexAttribIPointer), ENTRY(glDrawElements),
    ENTRY(glDrawRangeElements), ENTRY(glDrawElementsInstanced), ENTRY(glDrawArrays),
    ENTRY(glDrawArraysInstanced), ENTRY(glMapBufferRange), ENTRY(glFlushMappedBufferRange),
    ENTRY(glBufferData), ENTRY(glBufferSubData), ENTRY(glVertexAttribDivisor), ENTRY(glBindVertexArray),
    ENTRY(glEnable), ENTRY(glDisable),
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
    r_getproc     = dlsym(h, "_Z14getProcAddressPKc");
    LOG("loaded (%d forwarded, %d traced)", N_FWD, N_TRACED);
}

/*
 * The real driver's C++ getProcAddress(const char *), which WSA's EGL driver
 * uses to hand out GL entry points (eglGetProcAddress and Android's loader).
 * Without this, callers get the real functions and bypass the wrapper.
 */
EXPORT void *gles_getProcAddress(const char *name) __asm__("_Z14getProcAddressPKc");
void *gles_getProcAddress(const char *name) {
    if (name)
        for (int i = 0; i < N_TRACED; i++)
            if (!__builtin_strcmp(name, g_traced[i].name)) return g_traced[i].ours;
    return r_getproc ? r_getproc(name) : 0;
}
