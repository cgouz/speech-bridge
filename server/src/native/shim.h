/* shim.h — typed C entry points over dlopen'd Speech Bridge core libraries.
 *
 * cgo cannot call function pointers directly, so every core function is
 * resolved with dlsym into a static pointer and exposed here as a plain
 * function. Each library is opened with RTLD_NOW | RTLD_LOCAL and never linked
 * at Go build time (constraint 2).
 */
#ifndef SB_NATIVE_SHIM_H
#define SB_NATIVE_SHIM_H

#include <stddef.h>

#include "sb_stt.h"
#include "sb_mt.h"
#include "sb_tts.h"

#ifdef __cplusplus
/* Ported to the C++ orchestrator (server/): unlike the original cgo build,
 * where shim.h was only ever included from shim.c, native_core.cpp includes
 * it directly as a C++ translation unit. Without extern "C", the sbn_*
 * declarations below would get C++ (mangled) linkage while shim.c still
 * defines them with C linkage, and the link would fail. */
extern "C" {
#endif

/* Open one library by absolute path. Returns 0 on success; on failure writes a
 * message into err (NUL-terminated, capped at errcap) and returns -1.
 * A NULL/empty path is "not configured" -> returns 1 and leaves the slot unset. */
int sbn_open_stt(const char *path, char *err, size_t errcap);
int sbn_open_mt(const char *path, char *err, size_t errcap);
int sbn_open_tts_magpie(const char *path, char *err, size_t errcap);
int sbn_open_tts_vits(const char *path, char *err, size_t errcap);

int sbn_have_stt(void);
int sbn_have_mt(void);
int sbn_have_tts_magpie(void);
int sbn_have_tts_vits(void);

/* ---- STT ---- */
int             sbn_stt_abi_version(void);
sb_stt_model   *sbn_stt_model_load(const char *path, const char *device);
void            sbn_stt_model_free(sb_stt_model *m);
const char     *sbn_stt_model_last_error(sb_stt_model *m);
sb_stt_stream  *sbn_stt_stream_new(sb_stt_model *m, const char *lang);
void            sbn_stt_stream_free(sb_stt_stream *s);
const char     *sbn_stt_last_error(sb_stt_stream *s);
sb_status       sbn_stt_feed(sb_stt_stream *s, const float *pcm, size_t n);
sb_status       sbn_stt_poll(sb_stt_stream *s, sb_stt_event *out);
sb_status       sbn_stt_finish(sb_stt_stream *s);

/* ---- MT ---- */
int          sbn_mt_abi_version(void);
sb_mt_model *sbn_mt_model_load(const char *path, int n_ctx, const char *device);
void         sbn_mt_model_free(sb_mt_model *m);
const char  *sbn_mt_model_last_error(sb_mt_model *m);
sb_mt_ctx   *sbn_mt_ctx_new(sb_mt_model *m);
void         sbn_mt_ctx_free(sb_mt_ctx *c);
const char  *sbn_mt_last_error(sb_mt_ctx *c);
sb_status    sbn_mt_translate(sb_mt_ctx *c, const char *text, const char *src,
                              const char *dst, char *out, size_t out_cap);

/* ---- TTS (magpie | vits, selected by `engine`: 0 = magpie, 1 = vits) ---- */
int           sbn_tts_abi_version(int engine);
sb_tts_model *sbn_tts_model_load(int engine, const char *path, const char *device);
void          sbn_tts_model_free(int engine, sb_tts_model *m);
const char   *sbn_tts_model_last_error(int engine, sb_tts_model *m);
sb_tts_ctx   *sbn_tts_ctx_new(int engine, sb_tts_model *m);
void          sbn_tts_ctx_free(int engine, sb_tts_ctx *c);
const char   *sbn_tts_last_error(int engine, sb_tts_ctx *c);
sb_status     sbn_tts_speak(int engine, sb_tts_ctx *c, const char *text,
                            const char *lang, const char *voice,
                            const float **pcm, size_t *n, int *sample_rate);
sb_status     sbn_tts_languages(int engine, sb_tts_model *m, char *csv, size_t cap);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SB_NATIVE_SHIM_H */
