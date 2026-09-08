/* sb_stt.h — Speech Bridge streaming STT core ABI.
 *
 * Backed by parakeet.cpp (cache-aware streaming RNN-T, nemotron uz-streaming
 * fine-tune: uz | ru | kaa). Streaming-first: the model is loaded once, each
 * session opens a cheap stream that carries its own encoder / RNN-T cache.
 *
 * Threading: one inference at a time per stream. The caller (Go) serializes
 * feed/poll/finish on a given sb_stt_stream. Streams created from one model
 * share the model weights read-only.
 *
 * See ../common/sb_abi.h for status codes and buffer conventions.
 */
#ifndef SB_STT_H
#define SB_STT_H

#include "sb_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sb_stt_model  sb_stt_model;
typedef struct sb_stt_stream sb_stt_stream;

/* Event kinds returned by sb_stt_poll. */
#define SB_STT_NONE       0   /* no event pending */
#define SB_STT_PARTIAL    1   /* live caption for the in-progress utterance; may be revised */
#define SB_STT_FINAL_EOU  2   /* utterance complete (end-of-utterance fired) */

typedef struct sb_stt_event {
    int         kind;      /* SB_STT_NONE | SB_STT_PARTIAL | SB_STT_FINAL_EOU */
    const char *text;      /* UTF-8, NUL-terminated; owned by the stream, valid
                              only until the next call on this stream */
    long        start_ms;  /* utterance start, ms from stream start */
    long        end_ms;    /* utterance end (FINAL_EOU) or current position (PARTIAL) */
} sb_stt_event;

/* ABI revision of this core surface (== SB_ABI_VERSION at build time).
 * Used by the one-process dlopen smoke test as the per-core info function. */
SB_API int sb_stt_abi_version(void);

/* Load a GGUF STT model. `device` is "cpu" | "metal" | "auto" (accepted for
 * symmetry; the ggml backend is selected at build time — Metal on Apple
 * Silicon, CPU elsewhere). Returns NULL on failure. Heavy; call once. */
SB_API sb_stt_model *sb_stt_model_load(const char *path, const char *device);
SB_API void          sb_stt_model_free(sb_stt_model *m);
SB_API const char   *sb_stt_model_last_error(sb_stt_model *m);

/* Open a streaming session. `lang` is "uz" | "ru" | "kaa" (or "auto"/NULL for
 * the model default). Cheap; call once per speaker session. Returns NULL on
 * failure (e.g. the model is not cache-aware streaming, or an unknown lang). */
SB_API sb_stt_stream *sb_stt_stream_new(sb_stt_model *m, const char *lang);
SB_API void           sb_stt_stream_free(sb_stt_stream *s);
SB_API const char    *sb_stt_last_error(sb_stt_stream *s);

/* Feed a block of 16 kHz MONO float PCM (`n` samples). Decodes as full encoder
 * chunks become available and queues PARTIAL / FINAL_EOU events. Poll after
 * every feed. */
SB_API sb_status sb_stt_feed(sb_stt_stream *s, const float *pcm16k, size_t n);

/* Drain one queued event into *out. When the queue is empty, returns SB_OK
 * with out->kind == SB_STT_NONE. Call in a loop until SB_STT_NONE. */
SB_API sb_status sb_stt_poll(sb_stt_stream *s, sb_stt_event *out);

/* Flush the end-of-audio tail: processes remaining buffered audio and queues
 * any final events. Poll again afterwards. Does not fabricate an EOU. */
SB_API sb_status sb_stt_finish(sb_stt_stream *s);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SB_STT_H */
