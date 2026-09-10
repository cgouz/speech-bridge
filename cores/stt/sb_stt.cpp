/* sb_stt.cpp — Speech Bridge STT core: a thin, exception-safe wrapper that maps
 * the sb_stt.h ABI onto parakeet.cpp's flat C API (parakeet_capi.h).
 *
 * parakeet's cache-aware streaming decoder finalizes text incrementally as
 * encoder chunks complete and emits <EOU> / <EOB> turn-taking events. We
 * accumulate the finalized text of the in-progress utterance, surface it as
 * SB_STT_PARTIAL after each feed, and close it as SB_STT_FINAL_EOU when <EOU>
 * fires. <EOB> (backchannel) events are ignored for the meeting use case.
 */
#include "sb_stt.h"

#include <cctype>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <string>

#include "parakeet_capi.h"

namespace {

constexpr long kSampleRate = 16000;

long samples_to_ms(uint64_t samples) {
    return static_cast<long>((samples * 1000ULL) / static_cast<uint64_t>(kSampleRate));
}

struct QEvent {
    int         kind;
    std::string text;
    long        start_ms;
    long        end_ms;
};

} // namespace

struct sb_stt_model {
    parakeet_ctx *ctx = nullptr;
    std::string   device;
    std::string   last_error;
};

struct sb_stt_stream {
    sb_stt_model    *model = nullptr;
    parakeet_stream *ps = nullptr;
    std::deque<QEvent> queue;
    std::string      last_polled_text;   // keeps sb_stt_event.text alive
    std::string      last_error;
    std::string      utt_text;            // finalized text of the in-progress utterance
    long             utt_start_ms = -1;
    uint64_t         samples_fed = 0;
};

// --- helpers ---------------------------------------------------------------

static void stream_capture_error(sb_stt_stream *s, const char *fallback) {
    const char *e = s->model ? parakeet_capi_last_error(s->model->ctx) : "";
    s->last_error = (e && *e) ? e : fallback;
}

// Strip prompt/segment tags the nemotron streaming model can emit inline, e.g.
// "<en-US>", "<ru>", "<EOU>", "<EOB>" — parakeet strips <EOU>/<EOB> from feed()
// text but the locale tag can leak. Collapses any resulting double spaces.
static void strip_tags(std::string &t) {
    std::string out;
    out.reserve(t.size());
    for (size_t i = 0; i < t.size();) {
        if (t[i] == '<') {
            size_t close = t.find('>', i);
            if (close != std::string::npos && close - i <= 8) {
                bool tagish = true;
                for (size_t k = i + 1; k < close; ++k) {
                    char c = t[k];
                    if (!(std::isalnum((unsigned char)c) || c == '-' || c == '_')) { tagish = false; break; }
                }
                if (tagish) { i = close + 1; continue; }
            }
        }
        out.push_back(t[i]);
        ++i;
    }
    // squeeze spaces and trim
    std::string sq;
    bool prev_sp = false;
    for (char c : out) {
        bool sp = (c == ' ' || c == '\t');
        if (sp && prev_sp) continue;
        sq.push_back(c);
        prev_sp = sp;
    }
    size_t a = sq.find_first_not_of(' ');
    size_t b = sq.find_last_not_of(' ');
    t = (a == std::string::npos) ? "" : sq.substr(a, b - a + 1);
}

// Consume parakeet's freshly finalized text + EOU/EOB events for this feed and
// turn them into queued sb_stt events. parakeet's stream_feed returns text that
// already carries its own spacing — append verbatim, do not insert separators.
static void ingest(sb_stt_stream *s, char *new_text, long ms_at_feed_start) {
    const bool have_text = new_text && new_text[0] != '\0';
    if (have_text) {
        if (s->utt_text.empty()) {
            s->utt_start_ms = ms_at_feed_start;
        }
        s->utt_text += new_text;
    }

    long eou_end_ms = -1;
    parakeet_stream_event *events = nullptr;
    int n = parakeet_capi_stream_drain_events(s->ps, &events);
    for (int i = 0; i < n; ++i) {
        if (events[i].is_eob == 0) {
            long t = static_cast<long>(events[i].time_sec * 1000.0f);
            if (t > eou_end_ms) eou_end_ms = t;
        }
    }
    parakeet_capi_free_events(events);

    const long cur_ms = samples_to_ms(s->samples_fed);

    if (eou_end_ms >= 0) {
        if (s->utt_start_ms < 0) s->utt_start_ms = ms_at_feed_start;
        std::string clean = s->utt_text;
        strip_tags(clean);
        s->queue.push_back(QEvent{SB_STT_FINAL_EOU, clean,
                                  s->utt_start_ms,
                                  eou_end_ms > 0 ? eou_end_ms : cur_ms});
        s->utt_text.clear();
        s->utt_start_ms = -1;
    } else if (have_text) {
        std::string clean = s->utt_text;
        strip_tags(clean);
        s->queue.push_back(QEvent{SB_STT_PARTIAL, clean,
                                  s->utt_start_ms, cur_ms});
    }
}

// --- ABI ------------------------------------------------------------------

extern "C" {

SB_API int sb_stt_abi_version(void) { return SB_ABI_VERSION; }

SB_API sb_stt_model *sb_stt_model_load(const char *path, const char *device) {
    if (!path || !*path) return nullptr;
    try {
        auto *m = new sb_stt_model();
        m->device = device ? device : "auto";
        m->ctx = parakeet_capi_load(path);
        if (!m->ctx) {
            delete m;
            return nullptr;
        }
        return m;
    } catch (const std::exception &e) {
        return nullptr;
    } catch (...) {
        return nullptr;
    }
}

SB_API void sb_stt_model_free(sb_stt_model *m) {
    if (!m) return;
    if (m->ctx) parakeet_capi_free(m->ctx);
    delete m;
}

SB_API const char *sb_stt_model_last_error(sb_stt_model *m) {
    if (!m) return "";
    const char *e = m->ctx ? parakeet_capi_last_error(m->ctx) : "";
    if (e && *e) { m->last_error = e; return m->last_error.c_str(); }
    return m->last_error.c_str();
}

SB_API sb_stt_stream *sb_stt_stream_new(sb_stt_model *m, const char *lang) {
    if (!m || !m->ctx) return nullptr;
    try {
        auto *s = new sb_stt_stream();
        s->model = m;
        const char *l = (lang && *lang) ? lang : "auto";
        s->ps = parakeet_capi_stream_begin_lang(m->ctx, l);
        if (!s->ps) {
            m->last_error = parakeet_capi_last_error(m->ctx);
            delete s;
            return nullptr;
        }
        return s;
    } catch (...) {
        return nullptr;
    }
}

SB_API void sb_stt_stream_free(sb_stt_stream *s) {
    if (!s) return;
    if (s->ps) parakeet_capi_stream_free(s->ps);
    delete s;
}

SB_API const char *sb_stt_last_error(sb_stt_stream *s) {
    if (!s) return "";
    return s->last_error.c_str();
}

SB_API sb_status sb_stt_feed(sb_stt_stream *s, const float *pcm16k, size_t n) {
    if (!s || !s->ps) return SB_ERR_BAD_ARG;
    if (n == 0) return SB_OK;
    if (!pcm16k) return SB_ERR_BAD_ARG;
    if (n > static_cast<size_t>(INT_MAX)) return SB_ERR_BAD_ARG;
    try {
        const long ms_at_feed_start = samples_to_ms(s->samples_fed);
        int eou = 0;
        char *txt = parakeet_capi_stream_feed(s->ps, pcm16k,
                                              static_cast<int>(n), &eou);
        if (!txt) {
            stream_capture_error(s, "parakeet_capi_stream_feed failed");
            return SB_ERR_INFERENCE;
        }
        s->samples_fed += n;
        ingest(s, txt, ms_at_feed_start);
        parakeet_capi_free_string(txt);
        return SB_OK;
    } catch (const std::exception &e) {
        s->last_error = e.what();
        return SB_ERR_INFERENCE;
    } catch (...) {
        s->last_error = "unknown exception in sb_stt_feed";
        return SB_ERR_INFERENCE;
    }
}

SB_API sb_status sb_stt_poll(sb_stt_stream *s, sb_stt_event *out) {
    if (!s || !out) return SB_ERR_BAD_ARG;
    if (s->queue.empty()) {
        out->kind = SB_STT_NONE;
        out->text = "";
        out->start_ms = 0;
        out->end_ms = 0;
        return SB_OK;
    }
    QEvent ev = std::move(s->queue.front());
    s->queue.pop_front();
    s->last_polled_text = std::move(ev.text);
    out->kind = ev.kind;
    out->text = s->last_polled_text.c_str();
    out->start_ms = ev.start_ms;
    out->end_ms = ev.end_ms;
    return SB_OK;
}

SB_API sb_status sb_stt_finish(sb_stt_stream *s) {
    if (!s || !s->ps) return SB_ERR_BAD_ARG;
    try {
        char *txt = parakeet_capi_stream_finalize(s->ps);
        if (!txt) {
            stream_capture_error(s, "parakeet_capi_stream_finalize failed");
            return SB_ERR_INFERENCE;
        }
        const long ms_now = samples_to_ms(s->samples_fed);
        ingest(s, txt, ms_now);
        parakeet_capi_free_string(txt);
        // Close any residual in-progress utterance the tail did not EOU.
        if (!s->utt_text.empty()) {
            std::string clean = s->utt_text;
            strip_tags(clean);
            if (!clean.empty()) {
                s->queue.push_back(QEvent{SB_STT_FINAL_EOU, clean,
                                          s->utt_start_ms < 0 ? 0 : s->utt_start_ms,
                                          ms_now});
            }
            s->utt_text.clear();
            s->utt_start_ms = -1;
        }
        return SB_OK;
    } catch (const std::exception &e) {
        s->last_error = e.what();
        return SB_ERR_INFERENCE;
    } catch (...) {
        s->last_error = "unknown exception in sb_stt_finish";
        return SB_ERR_INFERENCE;
    }
}

} // extern "C"
