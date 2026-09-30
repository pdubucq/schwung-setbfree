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
 * Symbols setBfree's own main.c would provide.
 *
 * We link setBfree's sources without its main.c, so cfgParser.c's dispatch
 * table still needs mainConfig() and mainDoc() to exist. The previous version
 * defined them as
 *
 *     void *mainConfig = NULL;
 *     void *mainDoc = NULL;
 *
 * i.e. as DATA, while main.h declares them as FUNCTIONS and cfgParser.c:73
 * calls mainConfig(cfg). The linker is happy to resolve a call against a data
 * symbol; the CPU then jumps to the contents of a pointer-sized object in
 * .bss. These are real functions now, and including main.h means the compiler
 * checks them against the declarations the rest of setBfree compiles against.
 */

#include <stdlib.h>

#include "main.h"

/* setBfree's global sample rate. Move runs at 44100. */
double SampleRateD = 44100.0;

/* Config file support is deliberately absent: parsing a .cfg means open() and
 * read(), and every entry point in this module runs on the SPI audio callback.
 * Returning 0 means "not my key", which is exactly what a main section with no
 * settings should say. */
int mainConfig(ConfigContext *cfg)
{
    (void)cfg;
    return 0;
}

const ConfigDoc *mainDoc(void)
{
    return NULL;
}
