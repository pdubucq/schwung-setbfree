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
 * Shared declarations for the setBfree Schwung module.
 *
 * Included by BOTH schwung_bfree.c (the definitions) and schwung_wrapper.c
 * (the callers), so the compiler verifies they agree. The previous layout —
 * `extern` prototypes written by hand in the wrapper — let three signatures
 * drift out of sync silently.
 */

#ifndef SCHWUNG_BFREE_H
#define SCHWUNG_BFREE_H

#include <stdint.h>

void *plugin_create_instance(const char *module_dir, const char *json_defaults);
void  plugin_destroy_instance(void *inst);
void  plugin_on_midi(void *inst, const uint8_t *msg, int len, int source);
void  plugin_set_param(void *inst, const char *key, const char *val);
int   plugin_get_param(void *inst, const char *key, char *out, int len);
int   plugin_get_error(void *inst, char *buf, int buf_len);
void  plugin_render_block(void *inst, int16_t *out_lr, int frames);

#endif /* SCHWUNG_BFREE_H */
