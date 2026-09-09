/* sb_tts_magpie.cpp — Speech Bridge TTS core (magpie implementation).
 *
 * Maps the sb_tts.h ABI onto magpie-tts.cpp's flat C API (magpie_tts_capi.h).
 * Covers {en, de, es, fr, it, pt-BR, hi, ko, vi, ar}; 22050 Hz mono f32.
 *
 * magpie exposes a single loaded-model context and no separate per-session
 * object. We treat one sb_tts_model as one engine instance (a pool of size 1)
 * and serialize speak() calls on it with an internal mutex — TTS synthesis is
 * stateless per call, so no cross-sentence state is lost. Multiple sb_tts_ctx
 * created from one model share that instance; the pipeline uses one ctx per
 * engine per session and feeds sentences serially. Memory cost of a larger
 * pool is documented in docs/blockers.md.
 */
#include "sb_tts.h"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <vector>

#include "magpie_tts_capi.h"

namespace {
constexpr int kMagpieSampleRate = 22050;
const char *kMagpieLangs = "en,de,es,fr,it,pt-BR,hi,ko,vi,ar";

std::string map_lang(const char *lang) {
    // magpie wants a bare language code; pt-BR -> pt, everything else unchanged.
    if (!lang || !*lang) return "en";
    std::string l(lang);
    if (l == "pt-BR" || l == "pt_BR") return "pt";
    return l;
}
} // namespace

struct sb_tts_model {
    magpie_tts_ctx *mctx = nullptr;
    std::string     device;
    std::string     last_error;
    std::mutex      mtx;   // guards mctx (single engine instance)
};

struct sb_tts_ctx {
    sb_tts_model      *model = nullptr;
    std::vector<float> pcm;        // owns the buffer returned to the caller
    std::string        last_error;
};

extern "C" {

SB_API int sb_tts_abi_version(void) { return SB_ABI_VERSION; }

SB_API sb_tts_model *sb_tts_model_load(const char *path, const char *device) {
    if (!path || !*path) return nullptr;
    try {
        auto *m = new sb_tts_model();
        m->device = device ? device : "auto";
        m->mctx = magpie_tts_capi_load(path);
        if (!m->mctx) { delete m; return nullptr; }
        return m;
    } catch (...) { return nullptr; }
}

SB_API void sb_tts_model_free(sb_tts_model *m) {
    if (!m) return;
    if (m->mctx) magpie_tts_capi_free(m->mctx);
    delete m;
}

SB_API const char *sb_tts_model_last_error(sb_tts_model *m) {
    if (!m) return "";
    if (m->mctx) {
        const char *e = magpie_tts_capi_last_error(m->mctx);
        if (e && *e) { m->last_error = e; }
    }
    return m->last_error.c_str();
}

SB_API sb_tts_ctx *sb_tts_ctx_new(sb_tts_model *m) {
    if (!m || !m->mctx) return nullptr;
    try {
        auto *c = new sb_tts_ctx();
        c->model = m;
        return c;
    } catch (...) { return nullptr; }
}

SB_API void sb_tts_ctx_free(sb_tts_ctx *c) { delete c; }

SB_API const char *sb_tts_last_error(sb_tts_ctx *c) {
    return c ? c->last_error.c_str() : "";
}

SB_API sb_status sb_tts_speak(sb_tts_ctx *c, const char *text,
                              const char *lang, const char *voice,
                              const float **pcm, size_t *n, int *sample_rate) {
    if (!c || !c->model || !c->model->mctx || !text || !pcm || !n)
        return SB_ERR_BAD_ARG;
    *pcm = nullptr;
    *n = 0;
    if (sample_rate) *sample_rate = kMagpieSampleRate;

    try {
        const std::string l = map_lang(lang);
        std::lock_guard<std::mutex> guard(c->model->mtx);
        int ns = 0;
        float *out = magpie_tts_capi_synthesize(c->model->mctx, text, l.c_str(),
                                                (voice && *voice) ? voice : nullptr,
                                                &ns);
        if (!out || ns <= 0) {
            const char *e = magpie_tts_capi_last_error(c->model->mctx);
            c->last_error = (e && *e) ? e : "magpie synthesize failed";
            if (out) magpie_tts_capi_free_audio(out);
            return SB_ERR_INFERENCE;
        }
        c->pcm.assign(out, out + ns);
        magpie_tts_capi_free_audio(out);
        *pcm = c->pcm.data();
        *n = static_cast<size_t>(ns);
        return SB_OK;
    } catch (const std::exception &e) {
        c->last_error = e.what();
        return SB_ERR_INFERENCE;
    } catch (...) {
        c->last_error = "unknown exception in sb_tts_speak";
        return SB_ERR_INFERENCE;
    }
}

SB_API sb_status sb_tts_languages(sb_tts_model *m, char *csv_out, size_t cap) {
    if (!m || !csv_out || cap == 0) return SB_ERR_BAD_ARG;
    std::strncpy(csv_out, kMagpieLangs, cap - 1);
    csv_out[cap - 1] = '\0';
    return (std::strlen(kMagpieLangs) >= cap) ? SB_ERR_BAD_ARG : SB_OK;
}

} // extern "C"
