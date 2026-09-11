/* sb_mt.h — Speech Bridge machine-translation core ABI.
 *
 * Backed by llama.cpp running MADLAD-400 3B (T5). Prompt form "<2xx> text" is
 * built internally. Context: 512 tokens. One SENTENCE per call (constraint 5).
 * MADLAD emits Uzbek in CYRILLIC — server/src/text transliterates.
 *
 * Implemented in milestone 2. See ../common/sb_abi.h.
 */
#ifndef SB_MT_H
#define SB_MT_H

#include "sb_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sb_mt_model sb_mt_model;
typedef struct sb_mt_ctx   sb_mt_ctx;

SB_API int sb_mt_abi_version(void);

/* device: "cpu" | "metal" | "cuda" | "auto" (NULL treated as "auto"). "auto"
 * and any named GPU backend both offload every layer when SB_METAL/SB_CUDA
 * was compiled in and a matching device is actually present at runtime;
 * llama.cpp falls back to CPU on its own otherwise — this is a request, not
 * a guarantee. */
SB_API sb_mt_model *sb_mt_model_load(const char *path, int n_ctx /* 512 */,
                                     const char *device);
SB_API void         sb_mt_model_free(sb_mt_model *m);
SB_API const char  *sb_mt_model_last_error(sb_mt_model *m);

SB_API sb_mt_ctx *sb_mt_ctx_new(sb_mt_model *m);
SB_API void       sb_mt_ctx_free(sb_mt_ctx *c);
SB_API const char *sb_mt_last_error(sb_mt_ctx *c);

/* ONE sentence. `src` may be "auto". Builds "<2{dst}> {text}" internally.
 * Writes UTF-8 into `out` (NUL-terminated, truncated to out_cap). */
SB_API sb_status sb_mt_translate(sb_mt_ctx *c, const char *text,
                                 const char *src, const char *dst,
                                 char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif
#endif /* SB_MT_H */
