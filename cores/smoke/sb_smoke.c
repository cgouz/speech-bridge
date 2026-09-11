/* sb_smoke.c — one-process isolation proof for milestone 2.
 *
 * dlopen()s all four Speech Bridge core libraries with RTLD_NOW | RTLD_LOCAL in
 * a single process and calls one info function from each. If the engines' three
 * mutually-incompatible ggml copies (or onnxruntime) leaked symbols, resolving
 * or calling these would crash or return garbage. It passes iff every core
 * loads, every info function resolves, and each returns the expected ABI value.
 *
 * Build: see cores/smoke/CMakeLists.txt (driven by scripts/build.sh).
 * Usage: sb_smoke <lib_dir>        (default: ../lib relative to argv0 dir)
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SB_ABI_EXPECTED 2

typedef int (*abi_fn)(void);

static int check_one(const char *dir, const char *libname, const char *symbol) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", dir, libname);

    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "  FAIL  %-22s dlopen: %s\n", libname, dlerror());
        return 1;
    }

    dlerror();
    abi_fn fn = (abi_fn)(size_t)dlsym(h, symbol);
    const char *err = dlerror();
    if (err || !fn) {
        fprintf(stderr, "  FAIL  %-22s dlsym(%s): %s\n", libname, symbol,
                err ? err : "null");
        dlclose(h);
        return 1;
    }

    int v = fn();
    if (v != SB_ABI_EXPECTED) {
        fprintf(stderr, "  FAIL  %-22s %s() = %d, expected %d\n", libname, symbol,
                v, SB_ABI_EXPECTED);
        dlclose(h);
        return 1;
    }

    printf("  ok    %-22s %s() = %d\n", libname, symbol, v);
    /* keep the handle open: all four resident at once is the whole point */
    return 0;
}

int main(int argc, char **argv) {
    const char *dir = "lib";
    char buf[4096];

    if (argc > 1) {
        dir = argv[1];
    } else {
        /* default: <dir of argv0>/../lib */
        const char *slash = strrchr(argv[0], '/');
        if (slash) {
            size_t n = (size_t)(slash - argv[0]);
            snprintf(buf, sizeof(buf), "%.*s/../lib", (int)n, argv[0]);
            dir = buf;
        }
    }

#if defined(__APPLE__)
    const char *ext = "dylib";
#else
    const char *ext = "so";
#endif

    char stt[64], mt[64], magpie[64], vits[64];
    snprintf(stt, sizeof(stt), "libsb_stt.%s", ext);
    snprintf(mt, sizeof(mt), "libsb_mt.%s", ext);
    snprintf(magpie, sizeof(magpie), "libsb_tts_magpie.%s", ext);
    snprintf(vits, sizeof(vits), "libsb_tts_vits.%s", ext);

    printf("==> one-process dlopen smoke test   lib_dir=%s\n", dir);

    int rc = 0;
    rc |= check_one(dir, stt, "sb_stt_abi_version");
    rc |= check_one(dir, mt, "sb_mt_abi_version");
    rc |= check_one(dir, magpie, "sb_tts_abi_version");
    rc |= check_one(dir, vits, "sb_tts_abi_version");

    if (rc == 0)
        printf("==> smoke test PASSED: all four cores coexist in one process\n");
    else
        fprintf(stderr, "==> smoke test FAILED\n");
    return rc;
}
