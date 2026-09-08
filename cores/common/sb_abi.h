/* sb_abi.h — shared conventions for every Speech Bridge core.
 *
 * Every core (libsb_stt, libsb_mt, libsb_tts_magpie, libsb_tts_vits) is a
 * self-contained shared library that statically links its OWN engine and its
 * OWN ggml. Only symbols marked SB_API are exported; everything else is
 * hidden (-fvisibility=hidden + a linker export filter). Go dlopen's each lib
 * with RTLD_NOW | RTLD_LOCAL.
 *
 * Buffer conventions:
 *   - const char* out params are UTF-8, NUL-terminated.
 *   - Text written into a caller buffer (char *out, size_t out_cap) is
 *     truncated to fit and always NUL-terminated; SB_ERR_BAD_ARG if out_cap
 *     is 0.
 *   - A (const float **pcm, size_t *n) buffer returned by a core is owned by
 *     the core and valid ONLY until the next call on that same context. The
 *     caller copies it out immediately.
 *   - Every module exposes const char *sb_<mod>_last_error(ctx) returning a
 *     pointer owned by the context, valid until the next call on it.
 *
 * Lifecycle: *_model_load() is heavy and called once per process; weights are
 * shared read-only across contexts. *_ctx_new() / *_stream_new() is cheap and
 * per session. One inference at a time per context — the caller serializes.
 */
#ifndef SB_ABI_H
#define SB_ABI_H

#include <stddef.h>

#if defined(_WIN32)
#  define SB_API __declspec(dllexport)
#else
#  define SB_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* 0 = OK; negative = error. Keep in sync with app/internal/core. */
typedef int sb_status;

#define SB_OK                 0
#define SB_ERR_BAD_ARG       -1
#define SB_ERR_MODEL_LOAD    -2
#define SB_ERR_INFERENCE     -3
#define SB_ERR_UNSUPPORTED   -4  /* e.g. unsupported language */
#define SB_ERR_BUSY          -5

/* ABI revision of the sb_* core surface. Bump on any breaking change. */
#define SB_ABI_VERSION 1

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SB_ABI_H */
