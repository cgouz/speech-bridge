/* sb_tts_vits.cpp — Speech Bridge TTS core (VITS / MMS implementation).
 *
 * Implements the SAME sb_tts.h ABI as the magpie core, backed by sherpa-onnx
 * running Meta MMS-TTS (VITS) models for the languages magpie lacks:
 * uz, ru, kaa.
 *
 * `sb_tts_model_load(path, ...)` — `path` is a directory (SB_TTS_VITS_DIR).
 * Each immediate subdirectory holding a `model.onnx` + `tokens.txt` is one
 * VITS voice; the language is inferred from the subdirectory name
 * (`*rus*`/`*_ru*`/`ru` -> ru, `*uzb*`/`uz` -> uz, `*kaa*` -> kaa). One
 * SherpaOnnxOfflineTts per language is created once; generate() is serialized
 * per language with a mutex.
 */
#include "sb_tts.h"

#include <cstring>
#include <dirent.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "sherpa-onnx/c-api/c-api.h"

namespace {

bool is_dir(const std::string &p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
bool is_file(const std::string &p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// Map a voice-directory name to a Speech Bridge language code, or "" if unknown.
std::string lang_from_dirname(std::string n) {
    for (auto &c : n) c = static_cast<char>(::tolower(c));
    if (n.find("uzb") != std::string::npos || n.find("-uz") != std::string::npos ||
        n == "uz")
        return "uz";
    if (n.find("rus") != std::string::npos || n.find("-ru") != std::string::npos ||
        n == "ru")
        return "ru";
    if (n.find("kaa") != std::string::npos || n.find("karakalpak") != std::string::npos ||
        n == "kaa")
        return "kaa";
    return "";
}

struct Voice {
    const SherpaOnnxOfflineTts *tts = nullptr;
    int   sample_rate = 0;
    std::mutex mtx;
    ~Voice() { if (tts) SherpaOnnxDestroyOfflineTts(tts); }
};

} // namespace

struct sb_tts_model {
    std::string device;
    std::string last_error;
    std::string langs_csv;
    std::map<std::string, std::unique_ptr<Voice>> voices;   // lang -> voice
};

struct sb_tts_ctx {
    sb_tts_model      *model = nullptr;
    std::vector<float> pcm;
    std::string        last_error;
};

static const SherpaOnnxOfflineTts *make_tts(const std::string &dir,
                                            std::string *err) {
    const std::string model = dir + "/model.onnx";
    const std::string tokens = dir + "/tokens.txt";
    if (!is_file(model) || !is_file(tokens)) {
        *err = "missing model.onnx or tokens.txt in " + dir;
        return nullptr;
    }
    SherpaOnnxOfflineTtsConfig cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.model.vits.model = model.c_str();
    cfg.model.vits.tokens = tokens.c_str();
    cfg.model.vits.noise_scale = 0.667f;
    cfg.model.vits.noise_scale_w = 0.8f;
    cfg.model.vits.length_scale = 1.0f;
    cfg.model.num_threads = 2;
    cfg.model.provider = "cpu";
    cfg.model.debug = 0;
    cfg.max_num_sentences = 1;
    const SherpaOnnxOfflineTts *tts = SherpaOnnxCreateOfflineTts(&cfg);
    if (!tts) *err = "SherpaOnnxCreateOfflineTts failed for " + dir;
    return tts;
}

extern "C" {

SB_API int sb_tts_abi_version(void) { return SB_ABI_VERSION; }

SB_API sb_tts_model *sb_tts_model_load(const char *path, const char *device) {
    if (!path || !*path || !is_dir(path)) return nullptr;
    auto *m = new sb_tts_model();
    m->device = device ? device : "cpu";

    DIR *d = opendir(path);
    if (!d) { delete m; return nullptr; }
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        std::string sub = std::string(path) + "/" + name;
        if (!is_dir(sub)) continue;
        std::string lang = lang_from_dirname(name);
        if (lang.empty() || m->voices.count(lang)) continue;

        std::string err;
        const SherpaOnnxOfflineTts *tts = make_tts(sub, &err);
        if (!tts) { m->last_error = err; continue; }
        auto v = std::unique_ptr<Voice>(new Voice());
        v->tts = tts;
        v->sample_rate = SherpaOnnxOfflineTtsSampleRate(tts);
        m->voices.emplace(lang, std::move(v));
    }
    closedir(d);

    if (m->voices.empty()) {
        if (m->last_error.empty())
            m->last_error = "no VITS voice directories found under " + std::string(path);
        delete m;
        return nullptr;
    }
    for (auto &kv : m->voices) {
        if (!m->langs_csv.empty()) m->langs_csv += ",";
        m->langs_csv += kv.first;
    }
    return m;
}

SB_API void sb_tts_model_free(sb_tts_model *m) { delete m; }

SB_API const char *sb_tts_model_last_error(sb_tts_model *m) {
    return m ? m->last_error.c_str() : "";
}

SB_API sb_tts_ctx *sb_tts_ctx_new(sb_tts_model *m) {
    if (!m) return nullptr;
    auto *c = new sb_tts_ctx();
    c->model = m;
    return c;
}

SB_API void sb_tts_ctx_free(sb_tts_ctx *c) { delete c; }

SB_API const char *sb_tts_last_error(sb_tts_ctx *c) {
    return c ? c->last_error.c_str() : "";
}

SB_API sb_status sb_tts_speak(sb_tts_ctx *c, const char *text,
                              const char *lang, const char *voice,
                              const float **pcm, size_t *n, int *sample_rate) {
    (void)voice;
    if (!c || !c->model || !text || !pcm || !n) return SB_ERR_BAD_ARG;
    *pcm = nullptr;
    *n = 0;
    if (sample_rate) *sample_rate = 0;

    std::string l = (lang && *lang) ? lang : "";
    auto it = c->model->voices.find(l);
    if (it == c->model->voices.end()) {
        c->last_error = "vits: unsupported language '" + l + "'";
        return SB_ERR_UNSUPPORTED;
    }
    Voice *v = it->second.get();

    SherpaOnnxGenerationConfig gc;
    std::memset(&gc, 0, sizeof(gc));
    gc.sid = 0;
    gc.speed = 1.0f;
    gc.silence_scale = 0.2f;

    const SherpaOnnxGeneratedAudio *audio = nullptr;
    {
        std::lock_guard<std::mutex> guard(v->mtx);
        audio = SherpaOnnxOfflineTtsGenerateWithConfig(v->tts, text, &gc, nullptr, nullptr);
    }
    if (!audio || !audio->samples || audio->n <= 0) {
        c->last_error = "vits: generation failed";
        if (audio) SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
        return SB_ERR_INFERENCE;
    }
    c->pcm.assign(audio->samples, audio->samples + audio->n);
    if (sample_rate) *sample_rate = audio->sample_rate;
    SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
    *pcm = c->pcm.data();
    *n = c->pcm.size();
    return SB_OK;
}

SB_API sb_status sb_tts_languages(sb_tts_model *m, char *csv_out, size_t cap) {
    if (!m || !csv_out || cap == 0) return SB_ERR_BAD_ARG;
    std::strncpy(csv_out, m->langs_csv.c_str(), cap - 1);
    csv_out[cap - 1] = '\0';
    return (m->langs_csv.size() >= cap) ? SB_ERR_BAD_ARG : SB_OK;
}

} // extern "C"
