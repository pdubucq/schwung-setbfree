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
 * LD_PRELOAD realtime-safety interposer.
 *
 * The threading contract at the top of plugin_api_v1.h forbids allocation,
 * file I/O and logging from every plugin entry point, because all of them run
 * on the SPI audio callback. Nothing enforces that: a violation compiles,
 * links, passes every structural check, and then shows up on the device as a
 * device-wide audio dropout with no attribution.
 *
 * This makes it a test failure instead. The harness arms the guard around the
 * calls that must be clean and disarms it around the ones that may allocate
 * (the deferred-init worker, and the harness's own bookkeeping).
 *
 * Arming is per-thread on purpose: the init worker is allowed to malloc, and
 * it runs concurrently with the audio-thread calls we are policing.
 *
 * Usage:
 *   LD_PRELOAD=./build-host/rt_guard.so ./build-host/host_test
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>   /* mode_t, for the open() interposer's varargs */
#include <unistd.h>

#define MAX_VIOLATIONS 64

static __thread int armed = 0;

/* Recording a violation must not itself trip the guard, and must not allocate.
 * Fixed storage, no printf until the report. */
static struct {
    const char *what;
} violations[MAX_VIOLATIONS];
static int violation_count = 0;
static int violation_overflow = 0;

static void record(const char *what)
{
    if (!armed) return;
    if (violation_count < MAX_VIOLATIONS) {
        violations[violation_count++].what = what;
    } else {
        violation_overflow++;
    }
}

/* ---- public control surface, dlsym'd by the harness ---------------- */

__attribute__((visibility("default"))) void rtguard_arm(void)   { armed = 1; }
__attribute__((visibility("default"))) void rtguard_disarm(void){ armed = 0; }

__attribute__((visibility("default"))) int rtguard_violations(void)
{
    return violation_count + violation_overflow;
}

__attribute__((visibility("default"))) void rtguard_reset(void)
{
    violation_count = 0;
    violation_overflow = 0;
}

__attribute__((visibility("default"))) void rtguard_report(void)
{
    int was = armed;
    armed = 0;
    for (int i = 0; i < violation_count; i++) {
        fprintf(stderr, "     RT VIOLATION: %s\n", violations[i].what);
    }
    if (violation_overflow) {
        fprintf(stderr, "     RT VIOLATION: ... and %d more\n", violation_overflow);
    }
    armed = was;
}

/* ---- interposers --------------------------------------------------- */

#define REAL(name, type) \
    static type real_##name; \
    if (!real_##name) real_##name = (type)dlsym(RTLD_NEXT, #name);

typedef void *(*malloc_fn)(size_t);
typedef void *(*calloc_fn)(size_t, size_t);
typedef void *(*realloc_fn)(void *, size_t);
typedef void  (*free_fn)(void *);

/* calloc is called by dlsym() itself on some glibc paths. A small static
 * arena breaks that recursion. */
static char   bootstrap[65536];
static size_t bootstrap_used = 0;
static int    in_dlsym = 0;

static void *bootstrap_alloc(size_t n)
{
    n = (n + 15) & ~(size_t)15;
    if (bootstrap_used + n > sizeof(bootstrap)) return NULL;
    void *p = bootstrap + bootstrap_used;
    bootstrap_used += n;
    return p;
}

static int in_bootstrap(void *p)
{
    return (char *)p >= bootstrap && (char *)p < bootstrap + sizeof(bootstrap);
}

void *malloc(size_t n)
{
    static malloc_fn real = NULL;
    if (!real) {
        if (in_dlsym) return bootstrap_alloc(n);
        in_dlsym = 1;
        real = (malloc_fn)dlsym(RTLD_NEXT, "malloc");
        in_dlsym = 0;
    }
    record("malloc() on the audio thread");
    return real(n);
}

void *calloc(size_t a, size_t b)
{
    static calloc_fn real = NULL;
    if (!real) {
        if (in_dlsym) {
            void *p = bootstrap_alloc(a * b);
            if (p) memset(p, 0, a * b);
            return p;
        }
        in_dlsym = 1;
        real = (calloc_fn)dlsym(RTLD_NEXT, "calloc");
        in_dlsym = 0;
    }
    record("calloc() on the audio thread");
    return real(a, b);
}

void *realloc(void *p, size_t n)
{
    static realloc_fn real = NULL;
    if (!real) real = (realloc_fn)dlsym(RTLD_NEXT, "realloc");
    record("realloc() on the audio thread");
    return real(p, n);
}

void free(void *p)
{
    static free_fn real = NULL;
    if (in_bootstrap(p)) return;
    if (!real) real = (free_fn)dlsym(RTLD_NEXT, "free");
    record("free() on the audio thread");
    real(p);
}

typedef int    (*open_fn)(const char *, int, ...);
typedef FILE  *(*fopen_fn)(const char *, const char *);
typedef void  *(*opendir_fn)(const char *);
typedef void  *(*dlopen_fn)(const char *, int);

int open(const char *path, int flags, ...)
{
    static open_fn real = NULL;
    if (!real) real = (open_fn)dlsym(RTLD_NEXT, "open");
    record("open() on the audio thread");
    mode_t mode = 0;
    va_list ap;
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
    return real(path, flags, mode);
}

FILE *fopen(const char *path, const char *mode)
{
    static fopen_fn real = NULL;
    if (!real) real = (fopen_fn)dlsym(RTLD_NEXT, "fopen");
    record("fopen() on the audio thread");
    return real(path, mode);
}

void *opendir(const char *path)
{
    static opendir_fn real = NULL;
    if (!real) real = (opendir_fn)dlsym(RTLD_NEXT, "opendir");
    record("opendir() on the audio thread");
    return real(path);
}
