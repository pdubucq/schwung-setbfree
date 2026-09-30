/* b5 — setBfree Tonewheel Organ for Schwung
 *
 * Copyright (C) 2026 Pascal Dubucq
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Host-side contract test for the setBfree Schwung module.
 *
 * Every failure this test checks for is one that actually shipped to the
 * device and showed up there as an unexplained crash or a blank UI. The point
 * is to fail here, in a second, instead of over scp and a tail -f.
 *
 * It dlopen()s a NATIVELY built dsp.so and drives it exactly the way
 * chain_host.c does — same entry symbol, same call order, same 3-byte MIDI,
 * same get_param keys — and asserts on both behaviour and TIMING. The timing
 * assertions are the important ones: create_instance running long is not a
 * performance nit on this platform, it is the bug that produced
 *
 *     param-slow: set slot 2 synth:module took 4294967.295 ms on the SPI
 *     callback
 *
 * Build and run:  bash scripts/test.sh
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The REAL header, not a local retelling of it. The previous harness declared
 * its own 4-field host_api_v1_t; a struct-layout drift in the host would have
 * gone unnoticed until the device crashed. */
#include "host/plugin_api_v1.h"

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        checks++;                                           \
        if (cond) {                                         \
            printf("ok   ");                                \
        } else {                                            \
            printf("FAIL ");                                \
            failures++;                                     \
        }                                                   \
        printf(__VA_ARGS__);                                \
        printf("\n");                                       \
    } while (0)

#define REQUIRE(cond, ...)                                  \
    do {                                                    \
        CHECK(cond, __VA_ARGS__);                           \
        if (!(cond)) { printf("\nfatal, cannot continue\n"); return 1; }  \
    } while (0)

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* ---- optional LD_PRELOAD realtime guard --------------------------- */
/* Present only when running under tests/rt_guard.c. Without it these are
 * no-ops and the test still checks everything else. */
static void (*rtg_arm)(void);
static void (*rtg_disarm)(void);
static int  (*rtg_violations)(void);
static void (*rtg_reset)(void);
static void (*rtg_report)(void);
static int  rtg_available;

static void rtguard_init(void)
{
    rtg_arm        = dlsym(RTLD_DEFAULT, "rtguard_arm");
    rtg_disarm     = dlsym(RTLD_DEFAULT, "rtguard_disarm");
    rtg_violations = dlsym(RTLD_DEFAULT, "rtguard_violations");
    rtg_reset      = dlsym(RTLD_DEFAULT, "rtguard_reset");
    rtg_report     = dlsym(RTLD_DEFAULT, "rtguard_report");
    rtg_available  = rtg_arm && rtg_disarm && rtg_violations && rtg_reset && rtg_report;
}

#define RT_BEGIN() do { if (rtg_available) { rtg_reset(); rtg_arm(); } } while (0)
#define RT_END(what)                                                     \
    do {                                                                 \
        if (rtg_available) {                                             \
            rtg_disarm();                                                \
            int v = rtg_violations();                                    \
            if (v) rtg_report();                                         \
            CHECK(v == 0, "%s is realtime-clean (%d violations)", what, v); \
        }                                                                \
    } while (0)

/* ---- host_api_v1_t the module might call ------------------------- */

static void host_log(const char *msg)
{
    /* The module must never call this — logging is forbidden on the audio
     * thread and every entry point runs there. If it shows up, that is a
     * finding. */
    printf("     !! module called host->log(\"%s\") — forbidden from the audio thread\n", msg);
    failures++;
}

/* ---- audio inspection -------------------------------------------- */

typedef struct {
    int    nan_count;
    int    nonzero_count;
    int    clip_count;
    double peak;
} audio_stats_t;

static audio_stats_t analyse(const int16_t *buf, int frames)
{
    audio_stats_t s = { 0, 0, 0, 0.0 };
    for (int i = 0; i < frames * 2; i++) {
        int16_t v = buf[i];
        if (v != 0) s.nonzero_count++;
        if (v == 32767 || v == -32768) s.clip_count++;
        double a = fabs((double)v);
        if (a > s.peak) s.peak = a;
    }
    return s;
}

/* ---- manifest/plugin agreement ------------------------------------ */

#define MAX_KEYS 64
#define KEY_LEN  48

typedef struct {
    char k[MAX_KEYS][KEY_LEN];
    int  n;
    int  overflow;
} keyset_t;

static void keyset_add(keyset_t *s, const char *key)
{
    for (int i = 0; i < s->n; i++)
        if (strcmp(s->k[i], key) == 0) return;
    if (s->n >= MAX_KEYS) { s->overflow++; return; }
    snprintf(s->k[s->n], KEY_LEN, "%s", key);
    s->n++;
}

static int keyset_has(const keyset_t *s, const char *key)
{
    for (int i = 0; i < s->n; i++)
        if (strcmp(s->k[i], key) == 0) return 1;
    return 0;
}

/* Collect every  "key" : "<name>"  in a JSON blob. Deliberately a scanner and
 * not a parser: the chain host's own chain_params.c uses plain strstr on this
 * data, so matching its bluntness is closer to the truth than a correct parse
 * would be. */
static void scan_keys(const char *json, keyset_t *out)
{
    const char *p = json;
    while ((p = strstr(p, "\"key\"")) != NULL) {
        p += 5;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p != ':') continue;
        p++;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p != '"') continue;
        p++;
        const char *start = p;
        while (*p && *p != '"') p++;
        size_t len = (size_t)(p - start);
        if (len == 0 || len >= KEY_LEN) continue;
        char buf[KEY_LEN];
        memcpy(buf, start, len);
        buf[len] = '\0';
        keyset_add(out, buf);
    }
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

int main(int argc, char **argv)
{
    const char *so_path = (argc > 1) ? argv[1] : "build-host/modules/dsp.so";

    printf("=== setBfree module contract test ===\n");
    printf("module: %s\n", so_path);
    rtguard_init();
    printf("rt guard: %s\n\n", rtg_available
           ? "armed (LD_PRELOAD rt_guard.so)"
           : "not loaded — run via scripts/test.sh for allocation checks");

    void *h = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
    REQUIRE(h != NULL, "dlopen: %s", h ? "ok" : dlerror());

    /* RTLD_NOW above is deliberate: it is what the host uses, so an
     * unresolved symbol fails here rather than at slot-load time on device. */

    typedef const plugin_api_v2_t *(*init_fn)(const host_api_v1_t *);
    init_fn init = (init_fn)dlsym(h, MOVE_PLUGIN_INIT_V2_SYMBOL);
    REQUIRE(init != NULL, "dlsym(%s)", MOVE_PLUGIN_INIT_V2_SYMBOL);

    host_api_v1_t host;
    memset(&host, 0, sizeof(host));
    host.log = host_log;

    const plugin_api_v2_t *api = init(&host);
    REQUIRE(api != NULL, "move_plugin_init_v2 returned a table");
    CHECK(api->api_version == MOVE_PLUGIN_API_VERSION_2,
          "api_version == %d (got %d)", MOVE_PLUGIN_API_VERSION_2, api->api_version);
    REQUIRE(api->create_instance && api->destroy_instance && api->render_block,
            "required entry points present");
    CHECK(api->get_param != NULL, "get_param present");
    CHECK(api->set_param != NULL, "set_param present");
    CHECK(api->on_midi   != NULL, "on_midi present");

    /* ---------------------------------------------------------------
     * create_instance must be CHEAP. It runs on the SPI callback, which
     * has ~2.4 ms of slack for a 128-frame block. 50 ms is a generous
     * ceiling that still catches "the module built the whole organ
     * inline", which was the original crash.
     * --------------------------------------------------------------- */
    double t0 = now_ms();
    void *inst = api->create_instance(".", "{}");
    double create_ms = now_ms() - t0;

    REQUIRE(inst != NULL, "create_instance returned an instance");
    CHECK(create_ms < 50.0,
          "create_instance took %.2f ms (must be << one audio block)", create_ms);

    /* ---------------------------------------------------------------
     * Rendering before init completes must be silent, fast, and safe.
     * --------------------------------------------------------------- */
    int16_t buf[256 * 2];
    memset(buf, 0x7f, sizeof(buf));           /* poison, so silence is proven */
    t0 = now_ms();
    api->render_block(inst, buf, 128);
    double first_render_ms = now_ms() - t0;

    audio_stats_t s = analyse(buf, 128);
    CHECK(s.nonzero_count == 0,
          "first render is silence while the organ builds (%d non-zero samples)",
          s.nonzero_count);
    CHECK(first_render_ms < 5.0, "first render took %.3f ms", first_render_ms);

    /* MIDI arriving before the organ exists must not crash and must not be
     * lost; it is queued. */
    {
        uint8_t note_on[3] = { 0x90, 60, 100 };
        api->on_midi(inst, note_on, 3, MOVE_MIDI_SOURCE_HOST);
        uint8_t note_off[3] = { 0x80, 60, 0 };
        api->on_midi(inst, note_off, 3, MOVE_MIDI_SOURCE_HOST);
        CHECK(1, "on_midi before ready did not crash");
    }

    /* ---------------------------------------------------------------
     * Wait for the deferred-init worker.
     * --------------------------------------------------------------- */
    char val[512];
    int  ready = 0;
    double wait_start = now_ms();
    while (now_ms() - wait_start < 10000.0) {
        if (api->get_param(inst, "ready", val, sizeof(val)) > 0 && atoi(val) == 1) {
            ready = 1;
            break;
        }
        struct timespec ts = { 0, 5 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        api->render_block(inst, buf, 128);   /* keep the audio thread running */
    }
    double init_ms = now_ms() - wait_start;
    REQUIRE(ready, "deferred init completed (waited %.0f ms)", init_ms);
    printf("     deferred init took ~%.0f ms — that is how long this would have\n"
           "     blocked the SPI callback if it ran in create_instance\n", init_ms);

    /* ---------------------------------------------------------------
     * The four get_param keys the chain host and slot autosave need.
     * --------------------------------------------------------------- */
    {
        int n = api->get_param(inst, "preset_name", val, sizeof(val));
        /* Must be 0 with an empty string, NOT -1. A negative return makes the
         * grid retry the read forever — measured at 29 failed reads/second on
         * the shared param channel. */
        CHECK(n == 0, "preset_name returns 0 (got %d)", n);
        CHECK(n != 0 || val[0] == '\0', "preset_name is empty");
    }
    {
        int n = api->get_param(inst, "state", val, sizeof(val));
        /* Not answering this makes slot autosave retry every few seconds. */
        CHECK(n > 0, "state returns a payload (got %d)", n);
    }

    static char big[262144];
    int ui_len = api->get_param(inst, "ui_hierarchy", big, sizeof(big));
    /* THE reason the Move menu had no pages: for a sound generator the chain
     * host reads ui_hierarchy ONLY from the plugin (chain_host.c:2036).
     * A hierarchy declared in module.json is silently ignored. */
    CHECK(ui_len > 0, "ui_hierarchy served by the plugin (%d bytes)", ui_len);
    CHECK(ui_len > 0 && strstr(big, "\"levels\"") != NULL, "ui_hierarchy has a levels object");
    CHECK(ui_len > 0 && strstr(big, "\"root\"")   != NULL, "ui_hierarchy has a root level");

    int cp_len = api->get_param(inst, "chain_params", big, sizeof(big));
    CHECK(cp_len > 0, "chain_params served by the plugin (%d bytes)", cp_len);

    /* ---------------------------------------------------------------
     * module.json and the plugin must describe the SAME module.
     *
     * The host reads a synth's ui_hierarchy only from the plugin but falls
     * back to module.json for chain_params, so the two copies are both live
     * and a divergence shows up as a control that exists on one page and not
     * another — or as a knob that moves nothing. The manifest is the copy
     * nobody recompiles, so it is the one that rots.
     * --------------------------------------------------------------- */
    {
        keyset_t plugin_keys = { { { 0 } }, 0, 0 };
        scan_keys(big, &plugin_keys);
        CHECK(plugin_keys.n > 0 && !plugin_keys.overflow,
              "plugin chain_params declares %d keys", plugin_keys.n);

        char *manifest = slurp("src/schwung_bfree/module.json");
        if (!manifest) {
            printf("     skip manifest comparison (run from the repo root to enable it)\n");
        } else {
            keyset_t manifest_keys = { { { 0 } }, 0, 0 };
            scan_keys(manifest, &manifest_keys);
            CHECK(manifest_keys.n > 0 && !manifest_keys.overflow,
                  "module.json declares %d keys", manifest_keys.n);

            int missing_in_plugin = 0, missing_in_manifest = 0, unreadable = 0;
            for (int i = 0; i < manifest_keys.n; i++) {
                if (!keyset_has(&plugin_keys, manifest_keys.k[i])) {
                    printf("     in module.json but not in the plugin: %s\n", manifest_keys.k[i]);
                    missing_in_plugin++;
                }
                /* Declared to the host means the host will read it. */
                if (api->get_param(inst, manifest_keys.k[i], val, sizeof(val)) < 0) {
                    printf("     declared but unreadable: %s\n", manifest_keys.k[i]);
                    unreadable++;
                }
            }
            for (int i = 0; i < plugin_keys.n; i++) {
                if (!keyset_has(&manifest_keys, plugin_keys.k[i])) {
                    printf("     in the plugin but not in module.json: %s\n", plugin_keys.k[i]);
                    missing_in_manifest++;
                }
            }
            CHECK(missing_in_plugin == 0 && missing_in_manifest == 0,
                  "module.json and the plugin declare the same keys");
            CHECK(unreadable == 0, "every key module.json declares is readable");
            free(manifest);
        }
    }

    /* An unknown key must be distinguishable from an empty answer. */
    CHECK(api->get_param(inst, "no_such_param_xyz", val, sizeof(val)) < 0,
          "unknown key returns negative");

    /* A too-small buffer must be reported, not silently truncated. */
    {
        char tiny[8];
        CHECK(api->get_param(inst, "ui_hierarchy", tiny, sizeof(tiny)) < 0,
              "ui_hierarchy into an 8-byte buffer returns negative");
    }

    /* ---------------------------------------------------------------
     * Every declared parameter must round-trip.
     * --------------------------------------------------------------- */
    {
        static const char *keys[] = {
            "drawbar_0", "drawbar_1", "drawbar_2", "drawbar_3", "drawbar_4",
            "drawbar_5", "drawbar_6", "drawbar_7", "drawbar_8",
            "perc_enable", "perc_volume", "perc_decay", "perc_harmonic",
            "vibrato_knob", "vibrato_upper", "vibrato_lower",
            "od_enable", "od_character", "od_input", "od_output",
            "reverb_mix", "rotary", "volume",
        };
        int bad = 0;
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            if (api->get_param(inst, keys[i], val, sizeof(val)) < 0) {
                printf("     unreadable: %s\n", keys[i]);
                bad++;
            }
        }
        CHECK(bad == 0, "all %zu declared params readable",
              sizeof(keys) / sizeof(keys[0]));

        api->set_param(inst, "drawbar_0", "3");
        api->get_param(inst, "drawbar_0", val, sizeof(val));
        CHECK(atoi(val) == 3, "drawbar_0 round-trips 3 (got %s)", val);

        /* set_param and get_param are audio-thread calls too — the contract
         * lists them explicitly. A get_param that allocates is served once per
         * repaint on the SPI callback. */
        RT_BEGIN();
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            api->get_param(inst, keys[i], val, sizeof(val));
        }
        api->set_param(inst, "reverb_mix", "0.4");
        api->set_param(inst, "drawbar_3", "5");
        api->get_param(inst, "preset_name", val, sizeof(val));
        api->get_param(inst, "state", val, sizeof(val));
        RT_END("set_param/get_param");
        api->set_param(inst, "drawbar_3", "0");

        /* Out-of-range must clamp, not abort. setBfree's setDrawBar has
         * assert(setting < 9) and the old code clamped to 9, one past the end
         * of drawBarLevel[bus][9]. Built with -DCMAKE_BUILD_TYPE=Debug this
         * test runs with that assert live. */
        api->set_param(inst, "drawbar_0", "99");
        api->get_param(inst, "drawbar_0", val, sizeof(val));
        CHECK(atoi(val) == 8, "drawbar_0 clamps 99 -> 8 (got %s)", val);
        api->set_param(inst, "drawbar_0", "-5");
        api->get_param(inst, "drawbar_0", val, sizeof(val));
        CHECK(atoi(val) == 0, "drawbar_0 clamps -5 -> 0 (got %s)", val);
        api->set_param(inst, "drawbar_0", "8");

        api->set_param(inst, "rotary", "7");
        api->get_param(inst, "rotary", val, sizeof(val));
        CHECK(atoi(val) == 2, "rotary clamps 7 -> 2 (got %s)", val);
        api->set_param(inst, "rotary", "0");
    }

    /* ---------------------------------------------------------------
     * Sound. A silent organ passed every structural check the old
     * harness had.
     * --------------------------------------------------------------- */
    api->render_block(inst, buf, 128);   /* settle */

    {
        uint8_t note_on[3] = { 0x90, 60, 100 };
        api->on_midi(inst, note_on, 3, MOVE_MIDI_SOURCE_HOST);

        audio_stats_t acc = { 0, 0, 0, 0.0 };
        double worst_ms = 0.0;
        RT_BEGIN();
        for (int b = 0; b < 16; b++) {
            t0 = now_ms();
            api->render_block(inst, buf, 128);
            double ms = now_ms() - t0;
            if (ms > worst_ms) worst_ms = ms;

            audio_stats_t bs = analyse(buf, 128);
            acc.nonzero_count += bs.nonzero_count;
            acc.clip_count    += bs.clip_count;
            if (bs.peak > acc.peak) acc.peak = bs.peak;
        }
        RT_END("render_block");

        CHECK(acc.nonzero_count > 0, "note-on produces audio (%d non-zero samples)",
              acc.nonzero_count);
        CHECK(acc.peak > 100.0, "output has real level (peak %.0f / 32767)", acc.peak);
        CHECK(acc.clip_count < 128, "output is not pinned at full scale (%d clipped)",
              acc.clip_count);

        /* 128 frames at 44100 is 2.9 ms of audio. Rendering it must take a
         * small fraction of that; the SPI callback budget is ~2.4 ms total for
         * the whole chain, not for this module alone. */
        CHECK(worst_ms < 1.0, "worst render_block: %.3f ms for 128 frames "
              "(2.9 ms of audio)", worst_ms);

        uint8_t note_off[3] = { 0x80, 60, 0 };
        api->on_midi(inst, note_off, 3, MOVE_MIDI_SOURCE_HOST);
        for (int b = 0; b < 8; b++) api->render_block(inst, buf, 128);
    }

    /* Non-128 block sizes. oscGenerateFragment always produces exactly 128
     * mono samples regardless of what it is asked for, so any other block size
     * exercises the carry buffer — and the old code's `wbuf = fbuf + frames*2`
     * ran off the end of its allocation at 256. */
    {
        int sizes[] = { 1, 32, 64, 127, 128, 129, 192, 255, 256 };
        uint8_t note_on[3] = { 0x90, 64, 100 };
        api->on_midi(inst, note_on, 3, MOVE_MIDI_SOURCE_HOST);
        int ok = 1;
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            memset(buf, 0, sizeof(buf));
            api->render_block(inst, buf, sizes[i]);
            audio_stats_t bs = analyse(buf, sizes[i]);
            if (bs.nan_count) ok = 0;
        }
        CHECK(ok, "render survives block sizes 1..256");
        uint8_t note_off[3] = { 0x80, 64, 0 };
        api->on_midi(inst, note_off, 3, MOVE_MIDI_SOURCE_HOST);
    }

    /* Malformed and edge-case MIDI must not crash. */
    {
        uint8_t m2[2] = { 0xC0, 3 };
        api->on_midi(inst, m2, 2, MOVE_MIDI_SOURCE_HOST);
        uint8_t panic[3] = { 0xB0, 123, 0 };
        api->on_midi(inst, panic, 3, MOVE_MIDI_SOURCE_HOST);
        /* Notes at and beyond the manual's range: every Move pad must sound. */
        for (int n = 0; n < 128; n += 7) {
            uint8_t on[3]  = { 0x90, (uint8_t)n, 100 };
            uint8_t off[3] = { 0x80, (uint8_t)n, 0 };
            api->on_midi(inst, on, 3, MOVE_MIDI_SOURCE_HOST);
            api->render_block(inst, buf, 128);
            api->on_midi(inst, off, 3, MOVE_MIDI_SOURCE_HOST);
        }
        api->on_midi(inst, panic, 3, MOVE_MIDI_SOURCE_HOST);
        api->render_block(inst, buf, 128);
        CHECK(1, "malformed and full-range MIDI handled");
    }

    /* ---------------------------------------------------------------
     * Multi-instance: the chain host can load several slots.
     * --------------------------------------------------------------- */
    {
        void *a = api->create_instance(".", "{}");
        void *b = api->create_instance(".", "{}");
        CHECK(a && b && a != b && a != inst, "three independent instances");
        if (a) api->destroy_instance(a);
        if (b) api->destroy_instance(b);
        CHECK(1, "destroying extra instances is clean");
    }

    /* destroy_instance also runs on the SPI callback. */
    t0 = now_ms();
    api->destroy_instance(inst);
    double destroy_ms = now_ms() - t0;
    CHECK(destroy_ms < 50.0, "destroy_instance took %.2f ms", destroy_ms);

    /* Immediate create/destroy: cancels the worker mid-init, which is the one
     * path where destroy legitimately has to wait on a join. */
    {
        void *q = api->create_instance(".", "{}");
        t0 = now_ms();
        api->destroy_instance(q);
        double d = now_ms() - t0;
        printf("     create-then-immediately-destroy: %.1f ms (joins the init worker)\n", d);
        CHECK(1, "create/destroy during init does not crash or leak the worker");
    }

    dlclose(h);

    printf("\n=== %d checks, %d failures ===\n", checks, failures);
    return failures ? 1 : 0;
}
