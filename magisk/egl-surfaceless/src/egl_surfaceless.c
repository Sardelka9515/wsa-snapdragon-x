/*
 * EGL_KHR_surfaceless_context for WSA.
 *
 * WSA's EGL driver (/vendor/lib64/egl/libEGL_emulation.so) can't make a context
 * current without a surface: eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
 * ctx) fails with EGL_BAD_MATCH. Real devices allow it, and some games (e.g.
 * Bully: Anniversary Edition) rely on it, then crash on glGetString() == NULL.
 *
 * This library stands in for libEGL_emulation.so. Android's EGL loader resolves
 * each egl* function with dlsym() on the driver and falls back to the driver's
 * eglGetProcAddress(), so we only export the few functions we change and hand
 * everything else to the real driver (loaded from libEGL_emulation_real.so).
 *
 * Surfaceless eglMakeCurrent() binds a hidden 1x1 pbuffer made from the
 * context's own config instead, and EGL_KHR_surfaceless_context is added to the
 * extension string. Built without libc headers; it only needs libdl.
 */

typedef void *EGLDisplay, *EGLSurface, *EGLContext, *EGLConfig;
typedef int EGLint;
typedef unsigned int EGLBoolean, EGLenum;
typedef void (*fn_t)(void);

#define EGL_NO_SURFACE      ((EGLSurface)0)
#define EGL_NO_CONTEXT      ((EGLContext)0)
#define EGL_NONE            0x3038
#define EGL_CONFIG_ID       0x3028
#define EGL_WIDTH           0x3057
#define EGL_HEIGHT          0x3056
#define EGL_EXTENSIONS      0x3055
#define EGL_DRAW            0x3059
#define EGL_READ            0x305A
#define RTLD_NOW            2
#define REAL_DRIVER         "/vendor/lib64/egl/libEGL_emulation_real.so"
#define EXT                 "EGL_KHR_surfaceless_context"
#define MAX_CTX             256

extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int pthread_mutex_lock(void *);
extern int pthread_mutex_unlock(void *);

/* bionic: PTHREAD_MUTEX_INITIALIZER is all zero; the type is 40 bytes on LP64 */
static long g_lock[8];
static void *g_real;
static fn_t (*r_getproc)(const char *);
static EGLBoolean (*r_make)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
static EGLBoolean (*r_destroyctx)(EGLDisplay, EGLContext);
static const char *(*r_query)(EGLDisplay, EGLint);
static EGLBoolean (*r_querycontext)(EGLDisplay, EGLContext, EGLint, EGLint *);
static EGLBoolean (*r_choose)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
static EGLSurface (*r_pbuffer)(EGLDisplay, EGLConfig, const EGLint *);
static EGLBoolean (*r_destroysurf)(EGLDisplay, EGLSurface);
static EGLSurface (*r_cursurf)(EGLint);
static int (*r_log)(int, const char *, const char *, ...);

static struct { EGLDisplay dpy; EGLContext ctx; EGLSurface pb; } g_tab[MAX_CTX];
static char g_ext[8192];
static EGLDisplay g_ext_dpy;

#define LOG(...) do { if (r_log) r_log(5 /*WARN*/, "egl-surfaceless", __VA_ARGS__); } while (0)

static unsigned long slen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }

static int has_word(const char *s, const char *w) {
    unsigned long n = slen(w);
    for (const char *p = s; *p; p++)
        if ((p == s || p[-1] == ' ') && !__builtin_memcmp(p, w, n) && (p[n] == ' ' || !p[n])) return 1;
    return 0;
}

static int init(void) {
    if (g_real) return 1;
    pthread_mutex_lock(g_lock);
    if (!g_real) {
        void *h = dlopen(REAL_DRIVER, RTLD_NOW);
        if (h) {
            r_getproc      = dlsym(h, "eglGetProcAddress");
            r_make         = dlsym(h, "eglMakeCurrent");
            r_destroyctx   = dlsym(h, "eglDestroyContext");
            r_query        = dlsym(h, "eglQueryString");
            r_querycontext = dlsym(h, "eglQueryContext");
            r_choose       = dlsym(h, "eglChooseConfig");
            r_pbuffer      = dlsym(h, "eglCreatePbufferSurface");
            r_destroysurf  = dlsym(h, "eglDestroySurface");
            r_cursurf      = dlsym(h, "eglGetCurrentSurface");
            r_log          = dlsym((void *)0 /*RTLD_DEFAULT*/, "__android_log_print");
            __atomic_store_n(&g_real, h, __ATOMIC_RELEASE);
        }
    }
    pthread_mutex_unlock(g_lock);
    return g_real != 0;
}

static fn_t real_fn(const char *name) {
    fn_t f = (fn_t)dlsym(g_real, name);
    return f ? f : (r_getproc ? r_getproc(name) : 0);
}

/* The hidden pbuffer for ctx, created on first use. Caller holds g_lock. */
static EGLSurface pbuffer_for(EGLDisplay dpy, EGLContext ctx) {
    int free_slot = -1;
    for (int i = 0; i < MAX_CTX; i++) {
        if (g_tab[i].ctx == ctx && g_tab[i].dpy == dpy) return g_tab[i].pb;
        if (!g_tab[i].ctx && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) { LOG("too many contexts"); return EGL_NO_SURFACE; }
    EGLint id = 0, n = 0;
    EGLConfig cfg = 0;
    if (!r_querycontext(dpy, ctx, EGL_CONFIG_ID, &id)) return EGL_NO_SURFACE;
    const EGLint want[] = { EGL_CONFIG_ID, id, EGL_NONE };
    if (!r_choose(dpy, want, &cfg, 1, &n) || n < 1) { LOG("no config %d", id); return EGL_NO_SURFACE; }
    const EGLint size[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    EGLSurface pb = r_pbuffer(dpy, cfg, size);
    if (!pb) { LOG("pbuffer for config %d failed", id); return EGL_NO_SURFACE; }
    g_tab[free_slot].dpy = dpy;
    g_tab[free_slot].ctx = ctx;
    g_tab[free_slot].pb = pb;
    return pb;
}

static int is_ours(EGLSurface s) {
    if (!s) return 0;
    for (int i = 0; i < MAX_CTX; i++) if (g_tab[i].pb == s) return 1;
    return 0;
}

__attribute__((visibility("default")))
EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
    if (!init()) return 0;
    if (ctx != EGL_NO_CONTEXT && draw == EGL_NO_SURFACE && read == EGL_NO_SURFACE) {
        pthread_mutex_lock(g_lock);
        EGLSurface pb = pbuffer_for(dpy, ctx);
        pthread_mutex_unlock(g_lock);
        if (pb) return r_make(dpy, pb, pb, ctx);
    }
    return r_make(dpy, draw, read, ctx);
}

__attribute__((visibility("default")))
EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx) {
    if (!init()) return 0;
    EGLBoolean ok = r_destroyctx(dpy, ctx);
    pthread_mutex_lock(g_lock);
    for (int i = 0; i < MAX_CTX; i++) {
        if (g_tab[i].ctx == ctx && g_tab[i].dpy == dpy) {
            r_destroysurf(dpy, g_tab[i].pb);   /* deferred by EGL if still current */
            g_tab[i].ctx = 0; g_tab[i].dpy = 0; g_tab[i].pb = 0;
        }
    }
    pthread_mutex_unlock(g_lock);
    return ok;
}

/* Hide the stand-in pbuffer: a surfaceless context reports EGL_NO_SURFACE. */
__attribute__((visibility("default")))
EGLSurface eglGetCurrentSurface(EGLint which) {
    if (!init()) return EGL_NO_SURFACE;
    EGLSurface s = r_cursurf(which);
    pthread_mutex_lock(g_lock);
    int ours = is_ours(s);
    pthread_mutex_unlock(g_lock);
    return ours ? EGL_NO_SURFACE : s;
}

__attribute__((visibility("default")))
const char *eglQueryString(EGLDisplay dpy, EGLint name) {
    if (!init()) return 0;
    const char *s = r_query(dpy, name);
    if (name != EGL_EXTENSIONS || !s || has_word(s, EXT)) return s;
    pthread_mutex_lock(g_lock);
    if (g_ext_dpy != dpy || !g_ext[0]) {
        unsigned long n = slen(s);
        if (n + sizeof(EXT) + 2 > sizeof(g_ext)) { pthread_mutex_unlock(g_lock); return s; }
        __builtin_memcpy(g_ext, s, n);
        if (n && g_ext[n - 1] != ' ') g_ext[n++] = ' ';
        __builtin_memcpy(g_ext + n, EXT, sizeof(EXT));
        g_ext_dpy = dpy;
    }
    pthread_mutex_unlock(g_lock);
    return g_ext;
}

__attribute__((visibility("default")))
fn_t eglGetProcAddress(const char *name) {
    if (!init()) return 0;
    if (!__builtin_strcmp(name, "eglMakeCurrent"))       return (fn_t)eglMakeCurrent;
    if (!__builtin_strcmp(name, "eglDestroyContext"))    return (fn_t)eglDestroyContext;
    if (!__builtin_strcmp(name, "eglGetCurrentSurface")) return (fn_t)eglGetCurrentSurface;
    if (!__builtin_strcmp(name, "eglQueryString"))       return (fn_t)eglQueryString;
    return real_fn(name);
}
