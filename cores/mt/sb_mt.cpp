/* sb_mt.cpp — Speech Bridge machine-translation core.
 *
 * Wraps llama.cpp running MADLAD-400 3B (T5 encoder-decoder). One SENTENCE per
 * call. Prompt form "<2{dst}> {text}" is built here; MADLAD is tokenized with
 * its own sentencepiece vocab (the "<2xx>" tags are real tokens). Greedy decode
 * to end-of-generation.
 *
 * KV/encoder state is per sb_mt_ctx and cleared at the start of every
 * translate() call, so calls are independent. The Go layer serializes calls on
 * one ctx (constraint 4).
 */
#include "sb_mt.h"

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "llama.h"

namespace {
std::once_flag g_backend_once;
void backend_init() { llama_backend_init(); }
constexpr int kMaxNewTokens = 512;
} // namespace

struct sb_mt_model {
    llama_model *model = nullptr;
    int          n_ctx = 512;
    std::string  last_error;
};

struct sb_mt_ctx {
    sb_mt_model    *owner = nullptr;
    llama_context  *lctx = nullptr;
    llama_sampler  *smpl = nullptr;
    const llama_vocab *vocab = nullptr;
    std::string     last_error;
};

extern "C" {

SB_API int sb_mt_abi_version(void) { return SB_ABI_VERSION; }

SB_API sb_mt_model *sb_mt_model_load(const char *path, int n_ctx) {
    if (!path || !*path) return nullptr;
    std::call_once(g_backend_once, backend_init);
    try {
        auto *m = new sb_mt_model();
        m->n_ctx = n_ctx > 0 ? n_ctx : 512;
        llama_model_params mp = llama_model_default_params();
        mp.n_gpu_layers = 0;   // CPU-first; Metal wiring lands with SB_METAL
        m->model = llama_model_load_from_file(path, mp);
        if (!m->model) { delete m; return nullptr; }
        return m;
    } catch (...) { return nullptr; }
}

SB_API void sb_mt_model_free(sb_mt_model *m) {
    if (!m) return;
    if (m->model) llama_model_free(m->model);
    delete m;
}

SB_API const char *sb_mt_model_last_error(sb_mt_model *m) {
    return m ? m->last_error.c_str() : "";
}

SB_API sb_mt_ctx *sb_mt_ctx_new(sb_mt_model *m) {
    if (!m || !m->model) return nullptr;
    try {
        auto *c = new sb_mt_ctx();
        c->owner = m;
        c->vocab = llama_model_get_vocab(m->model);

        llama_context_params cp = llama_context_default_params();
        cp.n_ctx = static_cast<uint32_t>(m->n_ctx);
        cp.n_batch = static_cast<uint32_t>(m->n_ctx);
        cp.n_ubatch = static_cast<uint32_t>(m->n_ctx);
        cp.n_threads = 0;         // 0 => llama picks a sensible default
        cp.n_threads_batch = 0;
        c->lctx = llama_init_from_model(m->model, cp);
        if (!c->lctx) { delete c; return nullptr; }

        llama_sampler_chain_params sp = llama_sampler_chain_default_params();
        c->smpl = llama_sampler_chain_init(sp);
        llama_sampler_chain_add(c->smpl, llama_sampler_init_greedy());
        return c;
    } catch (...) { return nullptr; }
}

SB_API void sb_mt_ctx_free(sb_mt_ctx *c) {
    if (!c) return;
    if (c->smpl) llama_sampler_free(c->smpl);
    if (c->lctx) llama_free(c->lctx);
    delete c;
}

SB_API const char *sb_mt_last_error(sb_mt_ctx *c) {
    return c ? c->last_error.c_str() : "";
}

SB_API sb_status sb_mt_translate(sb_mt_ctx *c, const char *text,
                                 const char *src, const char *dst,
                                 char *out, size_t out_cap) {
    (void)src;
    if (!c || !c->lctx || !text || !out || out_cap == 0) return SB_ERR_BAD_ARG;
    if (!dst || !*dst) return SB_ERR_BAD_ARG;
    out[0] = '\0';

    try {
        const std::string prompt = std::string("<2") + dst + "> " + text;

        // tokenize
        std::vector<llama_token> toks(prompt.size() + 8);
        int n = llama_tokenize(c->vocab, prompt.c_str(), (int)prompt.size(),
                               toks.data(), (int)toks.size(),
                               /*add_special=*/true, /*parse_special=*/true);
        if (n < 0) {
            toks.resize(-n);
            n = llama_tokenize(c->vocab, prompt.c_str(), (int)prompt.size(),
                               toks.data(), (int)toks.size(), true, true);
        }
        if (n <= 0) { c->last_error = "tokenize failed"; return SB_ERR_INFERENCE; }
        toks.resize(n);

        llama_memory_clear(llama_get_memory(c->lctx), true);

        llama_token cur = 0;   // stable address reused for every decode batch
        llama_batch batch = llama_batch_get_one(toks.data(), (int)toks.size());
        if (llama_model_has_encoder(c->owner->model)) {
            if (llama_encode(c->lctx, batch) != 0) {
                c->last_error = "llama_encode failed";
                return SB_ERR_INFERENCE;
            }
            cur = llama_model_decoder_start_token(c->owner->model);
            if (cur == LLAMA_TOKEN_NULL) cur = llama_vocab_bos(c->vocab);
            batch = llama_batch_get_one(&cur, 1);
        }

        std::string result;
        for (int step = 0; step < kMaxNewTokens; ++step) {
            if (llama_decode(c->lctx, batch) != 0) {
                c->last_error = "llama_decode failed";
                return SB_ERR_INFERENCE;
            }
            cur = llama_sampler_sample(c->smpl, c->lctx, -1);
            if (llama_vocab_is_eog(c->vocab, cur)) break;

            char piece[256];
            int pn = llama_token_to_piece(c->vocab, cur, piece, sizeof(piece), 0, false);
            if (pn > 0) result.append(piece, pn);

            batch = llama_batch_get_one(&cur, 1);
        }

        // MADLAD sentencepiece often prefixes a leading space.
        size_t start = result.find_first_not_of(' ');
        if (start == std::string::npos) start = result.size();
        const std::string trimmed = result.substr(start);

        std::strncpy(out, trimmed.c_str(), out_cap - 1);
        out[out_cap - 1] = '\0';
        return (trimmed.size() >= out_cap) ? SB_ERR_BAD_ARG : SB_OK;
    } catch (const std::exception &e) {
        c->last_error = e.what();
        return SB_ERR_INFERENCE;
    } catch (...) {
        c->last_error = "unknown exception in sb_mt_translate";
        return SB_ERR_INFERENCE;
    }
}

} // extern "C"
