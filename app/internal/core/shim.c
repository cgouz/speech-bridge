/* shim.c — dlopen + dlsym the core libraries; see shim.h. */
#include "shim.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

/* ---------------- STT ---------------- */
static void *h_stt;
static int (*p_stt_abi)(void);
static sb_stt_model *(*p_stt_model_load)(const char *, const char *);
static void (*p_stt_model_free)(sb_stt_model *);
static const char *(*p_stt_model_last_error)(sb_stt_model *);
static sb_stt_stream *(*p_stt_stream_new)(sb_stt_model *, const char *);
static void (*p_stt_stream_free)(sb_stt_stream *);
static const char *(*p_stt_last_error)(sb_stt_stream *);
static sb_status (*p_stt_feed)(sb_stt_stream *, const float *, size_t);
static sb_status (*p_stt_poll)(sb_stt_stream *, sb_stt_event *);
static sb_status (*p_stt_finish)(sb_stt_stream *);

/* ---------------- MT ---------------- */
static void *h_mt;
static int (*p_mt_abi)(void);
static sb_mt_model *(*p_mt_model_load)(const char *, int);
static void (*p_mt_model_free)(sb_mt_model *);
static const char *(*p_mt_model_last_error)(sb_mt_model *);
static sb_mt_ctx *(*p_mt_ctx_new)(sb_mt_model *);
static void (*p_mt_ctx_free)(sb_mt_ctx *);
static const char *(*p_mt_last_error)(sb_mt_ctx *);
static sb_status (*p_mt_translate)(sb_mt_ctx *, const char *, const char *,
                                   const char *, char *, size_t);

/* ---------------- TTS (two engines, same ABI) ---------------- */
typedef struct {
    void *h;
    int (*abi)(void);
    sb_tts_model *(*model_load)(const char *, const char *);
    void (*model_free)(sb_tts_model *);
    const char *(*model_last_error)(sb_tts_model *);
    sb_tts_ctx *(*ctx_new)(sb_tts_model *);
    void (*ctx_free)(sb_tts_ctx *);
    const char *(*last_error)(sb_tts_ctx *);
    sb_status (*speak)(sb_tts_ctx *, const char *, const char *, const char *,
                       const float **, size_t *, int *);
    sb_status (*languages)(sb_tts_model *, char *, size_t);
} tts_vt;

static tts_vt g_tts[2]; /* [0]=magpie [1]=vits */

static void seterr(char *err, size_t cap, const char *msg) {
    if (err && cap) {
        snprintf(err, cap, "%s", msg);
    }
}

#define SYM(handle, dst, name, err, cap)                       \
    do {                                                      \
        *(void **)(&(dst)) = dlsym((handle), (name));          \
        if (!(dst)) {                                          \
            seterr((err), (cap), dlerror());                   \
            dlclose((handle));                                 \
            return -1;                                         \
        }                                                     \
    } while (0)

int sbn_open_stt(const char *path, char *err, size_t cap) {
    if (!path || !*path) return 1;
    if (h_stt) return 0;
    dlerror();
    h_stt = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h_stt) { seterr(err, cap, dlerror()); return -1; }
    SYM(h_stt, p_stt_abi, "sb_stt_abi_version", err, cap);
    SYM(h_stt, p_stt_model_load, "sb_stt_model_load", err, cap);
    SYM(h_stt, p_stt_model_free, "sb_stt_model_free", err, cap);
    SYM(h_stt, p_stt_model_last_error, "sb_stt_model_last_error", err, cap);
    SYM(h_stt, p_stt_stream_new, "sb_stt_stream_new", err, cap);
    SYM(h_stt, p_stt_stream_free, "sb_stt_stream_free", err, cap);
    SYM(h_stt, p_stt_last_error, "sb_stt_last_error", err, cap);
    SYM(h_stt, p_stt_feed, "sb_stt_feed", err, cap);
    SYM(h_stt, p_stt_poll, "sb_stt_poll", err, cap);
    SYM(h_stt, p_stt_finish, "sb_stt_finish", err, cap);
    return 0;
}

int sbn_open_mt(const char *path, char *err, size_t cap) {
    if (!path || !*path) return 1;
    if (h_mt) return 0;
    dlerror();
    h_mt = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h_mt) { seterr(err, cap, dlerror()); return -1; }
    SYM(h_mt, p_mt_abi, "sb_mt_abi_version", err, cap);
    SYM(h_mt, p_mt_model_load, "sb_mt_model_load", err, cap);
    SYM(h_mt, p_mt_model_free, "sb_mt_model_free", err, cap);
    SYM(h_mt, p_mt_model_last_error, "sb_mt_model_last_error", err, cap);
    SYM(h_mt, p_mt_ctx_new, "sb_mt_ctx_new", err, cap);
    SYM(h_mt, p_mt_ctx_free, "sb_mt_ctx_free", err, cap);
    SYM(h_mt, p_mt_last_error, "sb_mt_last_error", err, cap);
    SYM(h_mt, p_mt_translate, "sb_mt_translate", err, cap);
    return 0;
}

static int open_tts(int engine, const char *path, char *err, size_t cap) {
    if (!path || !*path) return 1;
    tts_vt *v = &g_tts[engine];
    if (v->h) return 0;
    dlerror();
    v->h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!v->h) { seterr(err, cap, dlerror()); return -1; }
    SYM(v->h, v->abi, "sb_tts_abi_version", err, cap);
    SYM(v->h, v->model_load, "sb_tts_model_load", err, cap);
    SYM(v->h, v->model_free, "sb_tts_model_free", err, cap);
    SYM(v->h, v->model_last_error, "sb_tts_model_last_error", err, cap);
    SYM(v->h, v->ctx_new, "sb_tts_ctx_new", err, cap);
    SYM(v->h, v->ctx_free, "sb_tts_ctx_free", err, cap);
    SYM(v->h, v->last_error, "sb_tts_last_error", err, cap);
    SYM(v->h, v->speak, "sb_tts_speak", err, cap);
    SYM(v->h, v->languages, "sb_tts_languages", err, cap);
    return 0;
}

int sbn_open_tts_magpie(const char *path, char *err, size_t cap) { return open_tts(0, path, err, cap); }
int sbn_open_tts_vits(const char *path, char *err, size_t cap) { return open_tts(1, path, err, cap); }

int sbn_have_stt(void) { return h_stt != NULL; }
int sbn_have_mt(void) { return h_mt != NULL; }
int sbn_have_tts_magpie(void) { return g_tts[0].h != NULL; }
int sbn_have_tts_vits(void) { return g_tts[1].h != NULL; }

/* ---- STT wrappers ---- */
int sbn_stt_abi_version(void) { return p_stt_abi ? p_stt_abi() : -1; }
sb_stt_model *sbn_stt_model_load(const char *p, const char *d) { return p_stt_model_load(p, d); }
void sbn_stt_model_free(sb_stt_model *m) { p_stt_model_free(m); }
const char *sbn_stt_model_last_error(sb_stt_model *m) { return p_stt_model_last_error(m); }
sb_stt_stream *sbn_stt_stream_new(sb_stt_model *m, const char *l) { return p_stt_stream_new(m, l); }
void sbn_stt_stream_free(sb_stt_stream *s) { p_stt_stream_free(s); }
const char *sbn_stt_last_error(sb_stt_stream *s) { return p_stt_last_error(s); }
sb_status sbn_stt_feed(sb_stt_stream *s, const float *p, size_t n) { return p_stt_feed(s, p, n); }
sb_status sbn_stt_poll(sb_stt_stream *s, sb_stt_event *o) { return p_stt_poll(s, o); }
sb_status sbn_stt_finish(sb_stt_stream *s) { return p_stt_finish(s); }

/* ---- MT wrappers ---- */
int sbn_mt_abi_version(void) { return p_mt_abi ? p_mt_abi() : -1; }
sb_mt_model *sbn_mt_model_load(const char *p, int n) { return p_mt_model_load(p, n); }
void sbn_mt_model_free(sb_mt_model *m) { p_mt_model_free(m); }
const char *sbn_mt_model_last_error(sb_mt_model *m) { return p_mt_model_last_error(m); }
sb_mt_ctx *sbn_mt_ctx_new(sb_mt_model *m) { return p_mt_ctx_new(m); }
void sbn_mt_ctx_free(sb_mt_ctx *c) { p_mt_ctx_free(c); }
const char *sbn_mt_last_error(sb_mt_ctx *c) { return p_mt_last_error(c); }
sb_status sbn_mt_translate(sb_mt_ctx *c, const char *t, const char *s,
                           const char *d, char *o, size_t cap) {
    return p_mt_translate(c, t, s, d, o, cap);
}

/* ---- TTS wrappers ---- */
int sbn_tts_abi_version(int e) { return g_tts[e].abi ? g_tts[e].abi() : -1; }
sb_tts_model *sbn_tts_model_load(int e, const char *p, const char *d) { return g_tts[e].model_load(p, d); }
void sbn_tts_model_free(int e, sb_tts_model *m) { g_tts[e].model_free(m); }
const char *sbn_tts_model_last_error(int e, sb_tts_model *m) { return g_tts[e].model_last_error(m); }
sb_tts_ctx *sbn_tts_ctx_new(int e, sb_tts_model *m) { return g_tts[e].ctx_new(m); }
void sbn_tts_ctx_free(int e, sb_tts_ctx *c) { g_tts[e].ctx_free(c); }
const char *sbn_tts_last_error(int e, sb_tts_ctx *c) { return g_tts[e].last_error(c); }
sb_status sbn_tts_speak(int e, sb_tts_ctx *c, const char *t, const char *l,
                        const char *v, const float **pcm, size_t *n, int *sr) {
    return g_tts[e].speak(c, t, l, v, pcm, n, sr);
}
sb_status sbn_tts_languages(int e, sb_tts_model *m, char *csv, size_t cap) {
    return g_tts[e].languages(m, csv, cap);
}
