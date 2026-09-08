/* sb_tts.h — Speech Bridge text-to-speech core ABI.
 *
 * ONE ABI, TWO interchangeable implementations:
 *   - libsb_tts_magpie : magpie-tts.cpp  — {en,de,es,fr,it,pt-BR,hi,ko,vi,ar}
 *   - libsb_tts_vits   : sherpa-onnx MMS-TTS (VITS) — {uz,ru,kaa}
 * The Go pipeline routes by target language and reports coverage via
 * /v1/capabilities (computed from sb_tts_languages + which models loaded).
 *
 * Audio: mono float PCM. magpie is 22050 Hz; VITS sample rate is per-model and
 * returned in *sample_rate. Feed TTS one SENTENCE at a time (constraint 5).
 *
 * The (const float **pcm, size_t *n) buffer is owned by the ctx and valid only
 * until the next call on that ctx — copy it out immediately (constraint 6).
 *
 * Implemented in milestone 2. See ../common/sb_abi.h.
 */
#ifndef SB_TTS_H
#define SB_TTS_H

#include "sb_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sb_tts_model sb_tts_model;
typedef struct sb_tts_ctx   sb_tts_ctx;

SB_API int sb_tts_abi_version(void);

/* magpie: `path` is the GGUF. vits: `path` is the model directory. */
SB_API sb_tts_model *sb_tts_model_load(const char *path, const char *device);
SB_API void          sb_tts_model_free(sb_tts_model *m);
SB_API const char   *sb_tts_model_last_error(sb_tts_model *m);

SB_API sb_tts_ctx *sb_tts_ctx_new(sb_tts_model *m);
SB_API void        sb_tts_ctx_free(sb_tts_ctx *c);
SB_API const char *sb_tts_last_error(sb_tts_ctx *c);

SB_API sb_status sb_tts_speak(sb_tts_ctx *c, const char *text,
                              const char *lang, const char *voice,
                              const float **pcm, size_t *n, int *sample_rate);

/* Comma-separated supported language codes written into `csv_out`. */
SB_API sb_status sb_tts_languages(sb_tts_model *m, char *csv_out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* SB_TTS_H */
