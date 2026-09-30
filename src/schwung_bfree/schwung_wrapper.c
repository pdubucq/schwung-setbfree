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
 * Schwung Plugin API v2 entry point for the setBfree tonewheel organ.
 *
 * This is deliberately thin. The previous version declared
 *
 *     extern int plugin_get_error(void *inst, char *buf, int buf_len);
 *
 * against a definition of `const char* plugin_get_error(void*)` and called it
 * with three arguments. C does not diagnose a prototype that contradicts a
 * definition in another translation unit — it just compiles, links, and
 * misbehaves. The prototypes below are kept in a shared header with the
 * definitions precisely so the compiler checks them.
 */

#include <stdlib.h>
#include <string.h>

#include "host/plugin_api_v1.h"
#include "schwung_bfree.h"

static void *create_instance_fn(const char *module_dir, const char *json_defaults)
{
    return plugin_create_instance(module_dir, json_defaults);
}

static void destroy_instance_fn(void *instance)
{
    plugin_destroy_instance(instance);
}

static void on_midi_fn(void *instance, const uint8_t *msg, int len, int source)
{
    /* The host sends RAW MIDI, length 3 — {status, data1, data2} — not a
     * 4-byte USB-MIDI packet. Rejecting len < 4 here, as this used to, dropped
     * every message the chain ever delivered. */
    if (!instance || !msg || len < 2) return;
    plugin_on_midi(instance, msg, len, source);
}

static void set_param_fn(void *instance, const char *key, const char *value)
{
    if (!instance || !key || !value) return;
    plugin_set_param(instance, key, value);
}

static int get_param_fn(void *instance, const char *key, char *out_value, int out_len)
{
    if (!instance || !key || !out_value || out_len <= 0) return -1;
    /* Pass the return through verbatim. Deriving it with strlen() — as this
     * used to — cannot distinguish "served, empty" from "not served", and both
     * of those have to be distinguishable: the grid retries a negative read,
     * so collapsing them turns preset_name into a permanent retry loop. */
    return plugin_get_param(instance, key, out_value, out_len);
}

static int get_error_fn(void *instance, char *buf, int buf_len)
{
    if (!instance || !buf || buf_len <= 0) return 0;
    return plugin_get_error(instance, buf, buf_len);
}

static void render_block_fn(void *instance, int16_t *out_lr, int frames)
{
    if (!instance || !out_lr || frames <= 0) return;
    plugin_render_block(instance, out_lr, frames);
}

/* ================================================================== */
/* Stable ABI entry point                                             */
/* ================================================================== */

__attribute__((visibility("default")))
const plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host)
{
    /* Runs on the SPI callback during chain load, so it does nothing but hand
     * back a static table. The host pointer is not retained: every logging
     * call it offers is forbidden from the audio thread, which is the only
     * thread this module's entry points ever run on. */
    (void)host;

    static const plugin_api_v2_t api = {
        .api_version      = MOVE_PLUGIN_API_VERSION_2,
        .create_instance  = create_instance_fn,
        .destroy_instance = destroy_instance_fn,
        .on_midi          = on_midi_fn,
        .set_param        = set_param_fn,
        .get_param        = get_param_fn,
        .get_error        = get_error_fn,
        .render_block     = render_block_fn,
    };
    return &api;
}
