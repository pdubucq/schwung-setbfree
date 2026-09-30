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
 * setBfree Tonewheel Organ — Schwung sound generator.
 *
 * THREADING. Every entry point here runs on the SPI audio callback (SCHED_FIFO
 * 70, core 3, ~2370 us of slack per 128-frame block). See the contract at the
 * top of host/plugin_api_v1.h — it is not a summary, it is the rule this file
 * is written against.
 *
 * setBfree's init is far outside that budget: initToneGenerator() builds 91
 * tonewheels, compiles the play matrix, allocates the taper lists and the key
 * compression table. An earlier version of this file did all of it inline in
 * create_instance and the device reported
 *
 *     param-slow: set slot 2 synth:module took 4294967.295 ms on the SPI
 *     callback — the module is doing blocking work in its entry point
 *
 * which is the 0xFFFFFFFF us overflow sentinel: the callback was held long
 * enough to stop the whole device, not just this slot. docs/REALTIME_SAFETY.md
 * measured a comparable module load at 673 ms == ~232 consecutive dropped
 * frames.
 *
 * So: create_instance does one calloc and starts a worker. The worker DEMOTES
 * ITSELF FIRST (threads inherit FIFO 70, which would starve Move's own `Link
 * Main` at FIFO 35), pins to cores 0-2 to leave core 3 for SPI, builds the
 * organ, and publishes it with a release store. render_block emits silence
 * until it lands.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>

#include "host/plugin_api_v1.h"
#include "schwung_bfree.h"

/* The real setBfree headers, not a hand-written copy of them. The previous
 * setbfree_compat.h declared setDrawBars(), plugin_get_error() and friends with
 * signatures that did not match their definitions; C checks none of that across
 * translation units, so every one of those was silent undefined behaviour. */
#include "global_inst.h"   /* b_instance, and every component header */
#include "state.h"         /* allocRunningConfig / initRunningConfig */
#include "main.h"          /* SampleRateD */

/* oscGenerateFragment() IGNORES its lengthSamples argument: every mixdown loop
 * in tonegen.c is `for (i = 0; i < BUFFER_SIZE_SAMPLES; i++)`. It always writes
 * exactly 128 MONO floats. Feeding it `frames` and then reading frames*2 as
 * interleaved stereo — which is what this file used to do — reads uninitialised
 * heap for the whole upper half of every block. */
#define TG_BLOCK        BUFFER_SIZE_SAMPLES   /* 128, fixed by tonegen.c */
#define MAX_FRAMES      256                   /* host block ceiling */
#define MIDI_QUEUE_SIZE 512
#define NDRAWBARS       9

/* Playable range mapped onto the upper manual. setBfree keys 0..60 correspond
 * to MIDI 36..96 on channel A (midi.c map_to_real_key), and percussion only
 * triggers for key numbers < 64, i.e. the upper manual — which is where a
 * single-keyboard controller belongs. */
#define NOTE_LO 36
#define NOTE_HI 96

typedef struct {
    uint8_t status;
    uint8_t d1;
    uint8_t d2;
} midi_msg_t;

typedef struct plugin_instance {
    /* setBfree's own instance aggregate. setDrawBars() and the MIDI dispatch
     * take a b_instance*, NOT a b_tonegen* — passing the tonegen (as this file
     * used to) reinterprets tonewheel state as the ->synth and ->midicfg
     * pointers and jumps through whatever happens to be there. */
    b_instance b;

    /* Deferred init */
    pthread_t    worker;
    int          worker_started;
    volatile int abort_init;
    volatile int ready;        /* release-published by the worker */
    volatile int failed;
    char         error_msg[256];

    /* Carry for the fixed 128-sample tone generator fragment */
    float tg_buf[TG_BLOCK];
    int   tg_pos;
    int   tg_avail;

    /* Fixed scratch — render_block must not allocate */
    float mono[MAX_FRAMES];
    float wet[MAX_FRAMES];
    float outL[MAX_FRAMES];
    float outR[MAX_FRAMES];

    /* MIDI ring. on_midi and render_block are both on the callback thread, so
     * plain indices are sufficient; the ring exists to keep note handling at a
     * single point in the block, not to cross threads. */
    midi_msg_t midi_q[MIDI_QUEUE_SIZE];
    uint32_t   midi_head;
    uint32_t   midi_tail;

    /* Parameter cache. Authoritative before the organ exists, and the answer
     * get_param gives at all times. */
    int   drawbar[NDRAWBARS];   /* 0..8, Hammond order 16' 5 1/3' 8' ... 1' */
    int   perc_enable;          /* 0/1 */
    int   perc_volume;          /* 0 = normal, 1 = soft */
    int   perc_decay;           /* 0 = slow, 1 = fast */
    int   perc_harmonic;        /* 0 = 3rd, 1 = 2nd */
    int   vibrato_knob;         /* 0..5 = v1 c1 v2 c2 v3 c3 */
    int   vibrato_upper;        /* 0/1 */
    int   vibrato_lower;        /* 0/1 */
    int   od_enable;            /* 0/1 */
    float od_character;         /* 0..1 */
    float od_input;             /* 0..1 */
    float od_output;            /* 0..1 */
    float reverb_mix;           /* 0..1 */
    int   rotary;               /* 0 = slow, 1 = stop, 2 = fast */
    float volume;               /* 0..1, swell pedal */

    /* Set when a param changes before the organ is ready, so render_block can
     * flush the cache once on the first ready block. Without it a set_param
     * landing between "worker applied the cache" and "worker published ready"
     * would be dropped. */
    volatile int params_dirty;
} plugin_instance_t;

/* ================================================================== */
/* Parameter application                                              */
/* ================================================================== */

/*
 * Everything goes through setBfree's own MIDI control functions
 * (notifyControlChangeByName), which is the interface its own UI uses. They are
 * all plain field writes — no allocation, no I/O — so they are safe to call
 * from set_param on the callback thread. Reaching into the structs directly
 * would mean duplicating the routing/notify side effects each one performs.
 */

static void apply_drawbars(plugin_instance_t *inst)
{
    unsigned int setting[NDRAWBARS];
    for (int i = 0; i < NDRAWBARS; i++) {
        int v = inst->drawbar[i];
        if (v < 0) v = 0;
        if (v > 8) v = 8;   /* setDrawBar indexes drawBarLevel[bus][0..8] */
        setting[i] = (unsigned int)v;
    }
    /* The same registration on all three manuals: only the upper one is played
     * here, but the lower and pedal buses still feed the mix. */
    setDrawBars(&inst->b, 0, setting);
    setDrawBars(&inst->b, 1, setting);
    setDrawBars(&inst->b, 2, setting);
}

static void cc(plugin_instance_t *inst, const char *name, int val)
{
    if (val < 0) val = 0;
    if (val > 127) val = 127;
    notifyControlChangeByName(inst->b.midicfg, name, (unsigned char)val);
}

static int f_to_cc(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return (int)(v * 127.0f + 0.5f);
}

static void apply_all_params(plugin_instance_t *inst)
{
    if (!inst->b.midicfg || !inst->b.synth) return;

    apply_drawbars(inst);

    cc(inst, "percussion.enable",   inst->perc_enable   ? 127 : 0);
    cc(inst, "percussion.volume",   inst->perc_volume   ? 127 : 0);
    cc(inst, "percussion.decay",    inst->perc_decay    ? 127 : 0);
    cc(inst, "percussion.harmonic", inst->perc_harmonic ? 127 : 0);

    /* setVibratoFromMIDI switches on u/23: 0..5 -> V1 C1 V2 C2 V3 C3 */
    cc(inst, "vibrato.knob",  inst->vibrato_knob * 23 + 11);
    cc(inst, "vibrato.upper", inst->vibrato_upper ? 127 : 0);
    cc(inst, "vibrato.lower", inst->vibrato_lower ? 127 : 0);

    /* setCleanCC inverts: >63 means NOT clean, i.e. overdrive engaged. */
    cc(inst, "overdrive.enable",     inst->od_enable ? 127 : 0);
    cc(inst, "overdrive.character",  f_to_cc(inst->od_character));
    cc(inst, "overdrive.inputgain",  f_to_cc(inst->od_input));
    cc(inst, "overdrive.outputgain", f_to_cc(inst->od_output));

    cc(inst, "reverb.mix", f_to_cc(inst->reverb_mix));

    /* revControl switches on u/43: 0 slow, 1 stop, 2 fast */
    cc(inst, "rotary.speed-preset", inst->rotary * 43 + 21);

    cc(inst, "swellpedal1", f_to_cc(inst->volume));

    inst->params_dirty = 0;
}

static void set_defaults(plugin_instance_t *inst)
{
    /* 88 8000 000 — the standard full-bodied Hammond registration. */
    inst->drawbar[0] = 8; inst->drawbar[1] = 8; inst->drawbar[2] = 8;
    for (int i = 3; i < NDRAWBARS; i++) inst->drawbar[i] = 0;

    inst->perc_enable   = 1;
    inst->perc_volume   = 0;   /* normal */
    inst->perc_decay    = 1;   /* fast */
    inst->perc_harmonic = 1;   /* 2nd */

    inst->vibrato_knob  = 1;   /* C1 */
    inst->vibrato_upper = 1;
    inst->vibrato_lower = 0;

    inst->od_enable    = 0;
    inst->od_character = 0.5f;
    inst->od_input     = 0.3567f;  /* overdrive.inputgain default */
    inst->od_output    = 0.0787f;  /* overdrive.outputgain default */

    inst->reverb_mix = 0.25f;
    inst->rotary     = 0;          /* slow */
    inst->volume     = 0.8f;
}

/* ================================================================== */
/* Deferred initialisation worker                                     */
/* ================================================================== */

static void teardown_components(plugin_instance_t *inst)
{
    if (inst->b.progs)   { freeProgs(inst->b.progs);              inst->b.progs   = NULL; }
    if (inst->b.whirl)   { freeWhirl(inst->b.whirl);              inst->b.whirl   = NULL; }
    if (inst->b.preamp)  { freePreamp(inst->b.preamp);            inst->b.preamp  = NULL; }
    if (inst->b.reverb)  { freeReverb(inst->b.reverb);            inst->b.reverb  = NULL; }
    if (inst->b.synth)   { freeToneGenerator(inst->b.synth);      inst->b.synth   = NULL; }
    if (inst->b.midicfg) { freeMidiCfg(inst->b.midicfg);          inst->b.midicfg = NULL; }
    if (inst->b.state)   { freeRunningConfig(inst->b.state);      inst->b.state   = NULL; }
}

static void fail(plugin_instance_t *inst, const char *why)
{
    snprintf(inst->error_msg, sizeof(inst->error_msg), "%s", why);
    teardown_components(inst);
    __atomic_store_n(&inst->failed, 1, __ATOMIC_RELEASE);
}

static void *init_worker(void *arg)
{
    plugin_instance_t *inst = (plugin_instance_t *)arg;

    /* FIRST ACTION, before anything else: this thread was created from the SPI
     * callback and inherited SCHED_FIFO 70. Move's `Link Main` publisher runs
     * at FIFO 35, so an inherited-priority worker starves the very thing going
     * off-thread was supposed to protect. */
    {
        struct sched_param sp = { .sched_priority = 0 };
        sched_setscheduler(0, SCHED_OTHER, &sp);

        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(0, &set);
        CPU_SET(1, &set);
        CPU_SET(2, &set);   /* core 3 stays free for SPI */
        sched_setaffinity(0, sizeof(set), &set);
    }

    /* Allocation order mirrors setBfree's own startup: the running config must
     * exist before allocMidiCfg, because every control-function hook writes
     * through midicfg->rcstate (midi.c controlFunctionHook -> rc_add_midicc),
     * which dereferences it without a NULL check. */
    inst->b.state = allocRunningConfig();
    if (!inst->b.state) { fail(inst, "allocRunningConfig failed"); return NULL; }

    inst->b.midicfg = allocMidiCfg(inst->b.state);
    if (!inst->b.midicfg) { fail(inst, "allocMidiCfg failed"); return NULL; }

    /* Primes ctrlUseA/B/C. Without it useMIDIControlFunction sees a zeroed
     * table, reads 0 < 128 for every function id, and binds all of them to
     * controller 0. */
    midiPrimeControllerMapping(inst->b.midicfg);
    initControllerTable(inst->b.midicfg);

    inst->b.synth = allocTonegen();
    if (!inst->b.synth) { fail(inst, "allocTonegen failed"); return NULL; }

    inst->b.reverb = allocReverb();
    if (!inst->b.reverb) { fail(inst, "allocReverb failed"); return NULL; }

    inst->b.preamp = allocPreamp();
    if (!inst->b.preamp) { fail(inst, "allocPreamp failed"); return NULL; }

    inst->b.whirl = allocWhirl();
    if (!inst->b.whirl) { fail(inst, "allocWhirl failed"); return NULL; }

    inst->b.progs = allocProgs();
    if (!inst->b.progs) { fail(inst, "allocProgs failed"); return NULL; }

    if (inst->abort_init) { teardown_components(inst); return NULL; }

    /* THE expensive call, and the one the old code never made at all:
     * allocTonegen() only callocs and resets the vibrato scanner. Everything
     * that makes the generator produce sound — applyDefaultConfiguration,
     * compilePlayMatrix, initOscillators, initKeyCompTable, initEnvelopes —
     * lives here. Without it oscGenerateFragment runs against an unbuilt
     * generator. */
    initToneGenerator(inst->b.synth, inst->b.midicfg);
    initVibrato(inst->b.synth, inst->b.midicfg);

    if (inst->abort_init) { teardown_components(inst); return NULL; }

    initPreamp(inst->b.preamp, inst->b.midicfg);
    initReverb(inst->b.reverb, inst->b.midicfg, SampleRateD);
    initWhirl(inst->b.whirl, inst->b.midicfg, SampleRateD);
    initRunningConfig(inst->b.state, inst->b.midicfg);

    if (inst->abort_init) { teardown_components(inst); return NULL; }

    apply_all_params(inst);

    /* Release store pairs with the acquire load in render_block: everything
     * written above is visible to the audio thread before it sees ready. */
    __atomic_store_n(&inst->ready, 1, __ATOMIC_RELEASE);
    return NULL;
}

/* ================================================================== */
/* Lifecycle                                                          */
/* ================================================================== */

void *plugin_create_instance(const char *module_dir, const char *json_defaults)
{
    (void)module_dir;
    (void)json_defaults;

    /* One calloc of ~8 KB and a pthread_create. Everything expensive is on the
     * worker. */
    plugin_instance_t *inst = (plugin_instance_t *)calloc(1, sizeof(plugin_instance_t));
    if (!inst) return NULL;

    set_defaults(inst);

    if (pthread_create(&inst->worker, NULL, init_worker, inst) != 0) {
        free(inst);
        return NULL;
    }
    inst->worker_started = 1;

    return inst;
}

void plugin_destroy_instance(void *inst_ptr)
{
    if (!inst_ptr) return;
    plugin_instance_t *inst = (plugin_instance_t *)inst_ptr;

    if (inst->worker_started) {
        /* The worker owns the component pointers until it publishes, so the
         * join is not optional. It only blocks if the slot is unloaded inside
         * the init window; abort_init cuts that short at the stage boundaries. */
        inst->abort_init = 1;
        pthread_join(inst->worker, NULL);
        inst->worker_started = 0;
    }

    teardown_components(inst);
    free(inst);
}

/* ================================================================== */
/* MIDI                                                               */
/* ================================================================== */

void plugin_on_midi(void *inst_ptr, const uint8_t *msg, int len, int source)
{
    (void)source;
    if (!inst_ptr || !msg || len < 2) return;

    plugin_instance_t *inst = (plugin_instance_t *)inst_ptr;

    /* RAW MIDI: msg[0] is the status byte. The chain host calls
     * on_midi(inst, msg, 3, source) with a bare {status, d1, d2}
     * (chain_host.c:192, chain_midi.c:604/754/945). This file used to require
     * len >= 4 and read the status from msg[1], treating the buffer as a
     * 4-byte USB-MIDI packet — so it discarded every message the host sent. */
    midi_msg_t m;
    m.status = msg[0];
    m.d1     = msg[1];
    m.d2     = (len >= 3) ? msg[2] : 0;

    uint32_t next = (inst->midi_head + 1) % MIDI_QUEUE_SIZE;
    if (next == inst->midi_tail) {
        inst->midi_tail = (inst->midi_tail + 1) % MIDI_QUEUE_SIZE;  /* drop oldest */
    }
    inst->midi_q[inst->midi_head] = m;
    inst->midi_head = next;
}

/* Fold anything outside the upper manual into it by octaves, so the whole of
 * Move's keyboard sounds instead of the ends going dead. */
static int note_to_key(int note)
{
    while (note < NOTE_LO) note += 12;
    while (note > NOTE_HI) note -= 12;
    if (note < NOTE_LO || note > NOTE_HI) return -1;
    return note - NOTE_LO;
}

static void all_notes_off(plugin_instance_t *inst)
{
    for (int k = 0; k <= NOTE_HI - NOTE_LO; k++) {
        oscKeyOff(inst->b.synth, (unsigned char)k, (unsigned char)k);
    }
}

static void drain_midi(plugin_instance_t *inst)
{
    while (inst->midi_tail != inst->midi_head) {
        midi_msg_t m = inst->midi_q[inst->midi_tail];
        inst->midi_tail = (inst->midi_tail + 1) % MIDI_QUEUE_SIZE;

        switch (m.status & 0xF0) {
            case 0x90: {  /* note on (velocity 0 == note off) */
                int k = note_to_key(m.d1);
                if (k < 0) break;
                if (m.d2 > 0) oscKeyOn(inst->b.synth,  (unsigned char)k, (unsigned char)k);
                else          oscKeyOff(inst->b.synth, (unsigned char)k, (unsigned char)k);
                break;
            }
            case 0x80: {  /* note off */
                int k = note_to_key(m.d1);
                if (k >= 0) oscKeyOff(inst->b.synth, (unsigned char)k, (unsigned char)k);
                break;
            }
            case 0xB0:
                if (m.d1 == 120 || m.d1 == 123) {   /* all sound off / all notes off */
                    all_notes_off(inst);
                } else if (m.d1 == 11) {            /* expression -> swell pedal */
                    inst->volume = m.d2 / 127.0f;
                    cc(inst, "swellpedal1", m.d2);
                } else if (m.d1 == 91) {            /* reverb send */
                    inst->reverb_mix = m.d2 / 127.0f;
                    cc(inst, "reverb.mix", m.d2);
                }
                break;
            default:
                break;
        }
    }
}

/* ================================================================== */
/* Parameters                                                         */
/* ================================================================== */

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

void plugin_set_param(void *inst_ptr, const char *key, const char *val)
{
    if (!inst_ptr || !key || !val) return;
    plugin_instance_t *inst = (plugin_instance_t *)inst_ptr;

    int   ready = __atomic_load_n(&inst->ready, __ATOMIC_ACQUIRE);
    float f     = (float)atof(val);
    int   i     = atoi(val);

    if (strncmp(key, "drawbar_", 8) == 0) {
        int idx = atoi(key + 8);
        if (idx < 0 || idx >= NDRAWBARS) return;
        inst->drawbar[idx] = clampi(i, 0, 8);
        if (ready) apply_drawbars(inst); else inst->params_dirty = 1;
        return;
    }

    #define INT_PARAM(name, field, lo, hi, ccname, ccval)                 \
        if (strcmp(key, name) == 0) {                                     \
            inst->field = clampi(i, lo, hi);                              \
            if (ready) cc(inst, ccname, (ccval)); else inst->params_dirty = 1; \
            return;                                                       \
        }
    #define FLT_PARAM(name, field, ccname)                                \
        if (strcmp(key, name) == 0) {                                     \
            inst->field = clampf(f, 0.0f, 1.0f);                          \
            if (ready) cc(inst, ccname, f_to_cc(inst->field));            \
            else inst->params_dirty = 1;                                  \
            return;                                                       \
        }

    INT_PARAM("perc_enable",   perc_enable,   0, 1, "percussion.enable",   inst->perc_enable   ? 127 : 0)
    INT_PARAM("perc_volume",   perc_volume,   0, 1, "percussion.volume",   inst->perc_volume   ? 127 : 0)
    INT_PARAM("perc_decay",    perc_decay,    0, 1, "percussion.decay",    inst->perc_decay    ? 127 : 0)
    INT_PARAM("perc_harmonic", perc_harmonic, 0, 1, "percussion.harmonic", inst->perc_harmonic ? 127 : 0)

    INT_PARAM("vibrato_knob",  vibrato_knob,  0, 5, "vibrato.knob",  inst->vibrato_knob * 23 + 11)
    INT_PARAM("vibrato_upper", vibrato_upper, 0, 1, "vibrato.upper", inst->vibrato_upper ? 127 : 0)
    INT_PARAM("vibrato_lower", vibrato_lower, 0, 1, "vibrato.lower", inst->vibrato_lower ? 127 : 0)

    INT_PARAM("od_enable",     od_enable,     0, 1, "overdrive.enable", inst->od_enable ? 127 : 0)
    INT_PARAM("rotary",        rotary,        0, 2, "rotary.speed-preset", inst->rotary * 43 + 21)

    FLT_PARAM("od_character", od_character, "overdrive.character")
    FLT_PARAM("od_input",     od_input,     "overdrive.inputgain")
    FLT_PARAM("od_output",    od_output,    "overdrive.outputgain")
    FLT_PARAM("reverb_mix",   reverb_mix,   "reverb.mix")
    FLT_PARAM("volume",       volume,       "swellpedal1")

    #undef INT_PARAM
    #undef FLT_PARAM

    /* Slot recall. The host hands back whatever get_param("state") produced;
     * every other value comes back through its own key, so there is nothing
     * else to restore here. */
    if (strcmp(key, "state") == 0) return;
}

/*
 * The UI contract, served from here and NOT from module.json.
 *
 * chain_host.c routes synth:ui_hierarchy straight to this function with no
 * fallback (chain_host.c:2036) — parse_ui_hierarchy_cache runs for audio FX and
 * MIDI FX only. A ui_hierarchy declared in a sound generator's module.json is
 * silently ignored, which is why this module previously showed no pages at all.
 * synth:chain_params does fall back to the module.json copy, so both exist and
 * must agree.
 */
int plugin_get_param(void *inst_ptr, const char *key, char *out, int len)
{
    if (!inst_ptr || !key || !out || len <= 0) return -1;
    plugin_instance_t *inst = (plugin_instance_t *)inst_ptr;

    if (strncmp(key, "drawbar_", 8) == 0) {
        int idx = atoi(key + 8);
        if (idx < 0 || idx >= NDRAWBARS) return -1;
        return snprintf(out, len, "%d", inst->drawbar[idx]);
    }

    if (strcmp(key, "perc_enable")   == 0) return snprintf(out, len, "%d", inst->perc_enable);
    if (strcmp(key, "perc_volume")   == 0) return snprintf(out, len, "%d", inst->perc_volume);
    if (strcmp(key, "perc_decay")    == 0) return snprintf(out, len, "%d", inst->perc_decay);
    if (strcmp(key, "perc_harmonic") == 0) return snprintf(out, len, "%d", inst->perc_harmonic);
    if (strcmp(key, "vibrato_knob")  == 0) return snprintf(out, len, "%d", inst->vibrato_knob);
    if (strcmp(key, "vibrato_upper") == 0) return snprintf(out, len, "%d", inst->vibrato_upper);
    if (strcmp(key, "vibrato_lower") == 0) return snprintf(out, len, "%d", inst->vibrato_lower);
    if (strcmp(key, "od_enable")     == 0) return snprintf(out, len, "%d", inst->od_enable);
    if (strcmp(key, "rotary")        == 0) return snprintf(out, len, "%d", inst->rotary);

    if (strcmp(key, "od_character") == 0) return snprintf(out, len, "%.3f", inst->od_character);
    if (strcmp(key, "od_input")     == 0) return snprintf(out, len, "%.3f", inst->od_input);
    if (strcmp(key, "od_output")    == 0) return snprintf(out, len, "%.3f", inst->od_output);
    if (strcmp(key, "reverb_mix")   == 0) return snprintf(out, len, "%.3f", inst->reverb_mix);
    if (strcmp(key, "volume")       == 0) return snprintf(out, len, "%.3f", inst->volume);

    /* A readout, not a control: 0 while the worker is still building the organ.
     * Declared access:"read" in chain_params so the grid dots it. */
    if (strcmp(key, "ready") == 0)
        return snprintf(out, len, "%d", __atomic_load_n(&inst->ready, __ATOMIC_ACQUIRE));

    /* EMPTY WITH RETURN 0, never -1. A negative return means "the read did not
     * complete" and the grid is right to retry it; voice-poc measured 29 failed
     * reads per second on the device from getting this wrong, on the single
     * param channel every value on screen shares. */
    if (strcmp(key, "preset_name") == 0) {
        out[0] = '\0';
        return 0;
    }

    /* "I cannot split." The organ has one voice; there is nothing to route to
     * separate buses. Served as empty-with-0 rather than -1 for the same
     * reason preset_name is. */
    if (strcmp(key, "split_voices") == 0) {
        out[0] = '\0';
        return 0;
    }

    /* A module that does not answer `state` makes the slot autosave retry
     * forever, once every few seconds, on that same channel. Every value is
     * restored through its own key, so the payload only has to be valid. */
    if (strcmp(key, "state") == 0)
        return snprintf(out, len, "{\"v\":1}");

    if (strcmp(key, "chain_params") == 0) {
        static const char *j =
        "["
          "{\"key\":\"drawbar_0\",\"name\":\"16'\",\"type\":\"int\",\"min\":0,\"max\":8,\"default\":8},"
          "{\"key\":\"drawbar_1\",\"name\":\"5 1/3'\",\"type\":\"int\",\"min\":0,\"max\":8,\"default\":8},"
          "{\"key\":\"drawbar_2\",\"name\":\"8'\",\"type\":\"int\",\"min\":0,\"max\":8,\"default\":8},"
          "{\"key\":\"drawbar_3\",\"name\":\"4'\",\"type\":\"int\",\"min\":0,\"max\":8,\"default\":0},"
          "{\"key\":\"drawbar_4\",\"name\":\"2 2/3'\",\"type\":\"int\",\"min\":0,\"max\":8,\"default\":0},"
          "{\"key\":\"drawbar_5\",\"name\":\"2'\",\"type\":\"int\",\"min\":0,\"max\":8,\"default\":0},"
          "{\"key\":\"drawbar_6\",\"name\":\"1 3/5'\",\"type\":\"int\",\"min\":0,\"max\":8,\"default\":0},"
          "{\"key\":\"drawbar_7\",\"name\":\"1 1/3'\",\"type\":\"int\",\"min\":0,\"max\":8,\"default\":0},"
          "{\"key\":\"drawbar_8\",\"name\":\"1'\",\"type\":\"int\",\"min\":0,\"max\":8,\"default\":0},"
          "{\"key\":\"perc_enable\",\"name\":\"Perc\",\"type\":\"enum\",\"min\":0,\"max\":1,\"default\":1,\"options\":[\"Off\",\"On\"]},"
          "{\"key\":\"perc_volume\",\"name\":\"Level\",\"type\":\"enum\",\"min\":0,\"max\":1,\"default\":0,\"options\":[\"Norm\",\"Soft\"]},"
          "{\"key\":\"perc_decay\",\"name\":\"Decay\",\"type\":\"enum\",\"min\":0,\"max\":1,\"default\":1,\"options\":[\"Slow\",\"Fast\"]},"
          "{\"key\":\"perc_harmonic\",\"name\":\"Harm\",\"type\":\"enum\",\"min\":0,\"max\":1,\"default\":1,\"options\":[\"3rd\",\"2nd\"]},"
          "{\"key\":\"vibrato_knob\",\"name\":\"Scanner\",\"type\":\"enum\",\"min\":0,\"max\":5,\"default\":1,"
            "\"options\":[\"V1\",\"C1\",\"V2\",\"C2\",\"V3\",\"C3\"]},"
          "{\"key\":\"vibrato_upper\",\"name\":\"Upper\",\"type\":\"enum\",\"min\":0,\"max\":1,\"default\":1,\"options\":[\"Off\",\"On\"]},"
          "{\"key\":\"vibrato_lower\",\"name\":\"Lower\",\"type\":\"enum\",\"min\":0,\"max\":1,\"default\":0,\"options\":[\"Off\",\"On\"]},"
          "{\"key\":\"od_enable\",\"name\":\"Drive\",\"type\":\"enum\",\"min\":0,\"max\":1,\"default\":0,\"options\":[\"Off\",\"On\"]},"
          "{\"key\":\"od_character\",\"name\":\"Char\",\"type\":\"float\",\"min\":0,\"max\":1,\"default\":0.5},"
          "{\"key\":\"od_input\",\"name\":\"In\",\"type\":\"float\",\"min\":0,\"max\":1,\"default\":0.357},"
          "{\"key\":\"od_output\",\"name\":\"Out\",\"type\":\"float\",\"min\":0,\"max\":1,\"default\":0.079},"
          "{\"key\":\"reverb_mix\",\"name\":\"Reverb\",\"type\":\"float\",\"min\":0,\"max\":1,\"default\":0.25},"
          "{\"key\":\"rotary\",\"name\":\"Leslie\",\"type\":\"enum\",\"min\":0,\"max\":2,\"default\":0,"
            "\"options\":[\"Slow\",\"Stop\",\"Fast\"]},"
          "{\"key\":\"volume\",\"name\":\"Swell\",\"type\":\"float\",\"min\":0,\"max\":1,\"default\":0.8},"
          "{\"key\":\"ready\",\"name\":\"RDY\",\"type\":\"int\",\"min\":0,\"max\":1,\"access\":\"read\",\"live\":true}"
        "]";
        int n = (int)strlen(j);
        if (n >= len) return -1;
        memcpy(out, j, (size_t)n + 1);
        return n;
    }

    if (strcmp(key, "ui_hierarchy") == 0) {
        static const char *j =
        "{"
          "\"levels\":{"
            "\"root\":{\"name\":\"setBfree\","
              "\"knobs\":[\"drawbar_0\",\"drawbar_1\",\"drawbar_2\",\"volume\",\"reverb_mix\",\"rotary\",\"perc_enable\",\"ready\"],"
              "\"params\":["
                "{\"level\":\"drawbars\",\"label\":\"Drawbars\"},"
                "{\"level\":\"percussion\",\"label\":\"Percussion\"},"
                "{\"level\":\"vibrato\",\"label\":\"Vibrato\"},"
                "{\"level\":\"drive\",\"label\":\"Overdrive\"},"
                "{\"level\":\"output\",\"label\":\"Output\"}"
              "]},"
            "\"drawbars\":{\"name\":\"Drawbars\","
              "\"knobs\":[\"drawbar_0\",\"drawbar_1\",\"drawbar_2\",\"drawbar_3\","
                        "\"drawbar_4\",\"drawbar_5\",\"drawbar_6\",\"drawbar_7\"],"
              "\"params\":["
                "{\"key\":\"drawbar_0\"},{\"key\":\"drawbar_1\"},{\"key\":\"drawbar_2\"},"
                "{\"key\":\"drawbar_3\"},{\"key\":\"drawbar_4\"},{\"key\":\"drawbar_5\"},"
                "{\"key\":\"drawbar_6\"},{\"key\":\"drawbar_7\"},{\"key\":\"drawbar_8\"}"
              "]},"
            "\"percussion\":{\"name\":\"Percussion\","
              "\"knobs\":[\"perc_enable\",\"perc_volume\",\"perc_decay\",\"perc_harmonic\"],"
              "\"params\":["
                "{\"key\":\"perc_enable\"},{\"key\":\"perc_volume\"},"
                "{\"key\":\"perc_decay\"},{\"key\":\"perc_harmonic\"}"
              "]},"
            "\"vibrato\":{\"name\":\"Vibrato\","
              "\"knobs\":[\"vibrato_knob\",\"vibrato_upper\",\"vibrato_lower\"],"
              "\"params\":["
                "{\"key\":\"vibrato_knob\"},{\"key\":\"vibrato_upper\"},{\"key\":\"vibrato_lower\"}"
              "]},"
            "\"drive\":{\"name\":\"Overdrive\","
              "\"knobs\":[\"od_enable\",\"od_character\",\"od_input\",\"od_output\"],"
              "\"params\":["
                "{\"key\":\"od_enable\"},{\"key\":\"od_character\"},"
                "{\"key\":\"od_input\"},{\"key\":\"od_output\"}"
              "]},"
            "\"output\":{\"name\":\"Output\","
              "\"knobs\":[\"rotary\",\"reverb_mix\",\"volume\",\"ready\"],"
              "\"params\":["
                "{\"key\":\"rotary\"},{\"key\":\"reverb_mix\"},"
                "{\"key\":\"volume\"},{\"key\":\"ready\"}"
              "]}"
          "}"
        "}";
        int n = (int)strlen(j);
        if (n >= len) return -1;
        memcpy(out, j, (size_t)n + 1);
        return n;
    }

    return -1;   /* not served — distinct from "served, and empty" */
}

int plugin_get_error(void *inst_ptr, char *buf, int buf_len)
{
    if (!inst_ptr || !buf || buf_len <= 0) return 0;
    plugin_instance_t *inst = (plugin_instance_t *)inst_ptr;

    if (!__atomic_load_n(&inst->failed, __ATOMIC_ACQUIRE)) {
        buf[0] = '\0';
        return 0;
    }
    int n = snprintf(buf, (size_t)buf_len, "%s", inst->error_msg);
    if (n >= buf_len) n = buf_len - 1;
    return n;
}

/* ================================================================== */
/* Rendering                                                          */
/* ================================================================== */

/* Pull `frames` mono samples out of the tone generator, which only ever
 * produces them 128 at a time. */
static void tonegen_pull(plugin_instance_t *inst, float *dst, int frames)
{
    int done = 0;
    while (done < frames) {
        if (inst->tg_avail == 0) {
            oscGenerateFragment(inst->b.synth, inst->tg_buf, TG_BLOCK);
            inst->tg_pos   = 0;
            inst->tg_avail = TG_BLOCK;
        }
        int n = frames - done;
        if (n > inst->tg_avail) n = inst->tg_avail;
        memcpy(dst + done, inst->tg_buf + inst->tg_pos, (size_t)n * sizeof(float));
        done           += n;
        inst->tg_pos   += n;
        inst->tg_avail -= n;
    }
}

static inline int16_t to_i16(float x)
{
    /* NaN compares false against both bounds, so test for it explicitly rather
     * than letting it become an arbitrary int16. */
    if (!(x > -1.0f && x < 1.0f)) {
        if (x >= 1.0f)  return  32767;
        if (x <= -1.0f) return -32768;
        return 0;                       /* NaN */
    }
    return (int16_t)(x * 32767.0f);
}

void plugin_render_block(void *inst_ptr, int16_t *out_lr, int frames)
{
    if (!inst_ptr || !out_lr || frames <= 0) return;
    plugin_instance_t *inst = (plugin_instance_t *)inst_ptr;

    /* The host documents frames as always MOVE_FRAMES_PER_BLOCK (128) and the
     * scratch buffers are sized for twice that. If it ever asks for more,
     * output silence rather than overrunning them — and rather than leaving
     * the caller's buffer untouched, which would repeat whatever was in it. */
    if (frames > MAX_FRAMES) {
        memset(out_lr, 0, (size_t)frames * 2 * sizeof(int16_t));
        return;
    }

    /* Silence until the worker publishes. Acquire pairs with its release store;
     * MIDI received in the meantime stays queued and plays on the first real
     * block. */
    if (!__atomic_load_n(&inst->ready, __ATOMIC_ACQUIRE)) {
        memset(out_lr, 0, (size_t)frames * 2 * sizeof(int16_t));
        return;
    }

    if (inst->params_dirty) apply_all_params(inst);

    drain_midi(inst);

    /* setBfree's own signal chain, in order: tonegen -> preamp/overdrive ->
     * reverb -> Leslie. preamp() dispatches to overdrive() internally when the
     * drive is engaged and memcpys when it is not, so calling both (as this
     * used to) ran the saturation stage twice. */
    tonegen_pull(inst, inst->mono, frames);

    preamp(inst->b.preamp, inst->mono, inst->wet, (size_t)frames);
    reverb(inst->b.reverb, inst->wet, inst->mono, (size_t)frames);

    /* The Leslie is what makes the organ stereo — "Stop" still images across
     * both channels, it just does not rotate. */
    whirlProc(inst->b.whirl, inst->mono, inst->outL, inst->outR, (size_t)frames);

    for (int i = 0; i < frames; i++) {
        out_lr[i * 2]     = to_i16(inst->outL[i]);
        out_lr[i * 2 + 1] = to_i16(inst->outR[i]);
    }
}
