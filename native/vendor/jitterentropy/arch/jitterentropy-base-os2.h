/* Craft Trio's OpenWatcom platform glue for jitterentropy 3.7.0.
 * SPDX-License-Identifier: BSD-3-Clause
 * No changes to the noise source, conditioner, or health-test thresholds.
 */
#ifndef WC_JITTERENTROPY_OS2_H
#define WC_JITTERENTROPY_OS2_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <errno.h>

#if defined(__WATCOMC__)
#include <sys/types.h>
#define __attribute__(attributes)

/* The caller must check CPUID's TSC bit before entering the library.
 * Use bytes so the rest of the program can retain its i386 baseline.
 * This deliberately uses neither SSE nor the much newer RDRAND.
 */
static void jent_get_nstime(uint64_t *out);
#pragma aux jent_get_nstime = \
    "db 0fh,31h" \
    "mov [ecx],eax" \
    "mov [ecx+4],edx" \
    parm [ecx] modify exact [eax edx];
#else
/* Compile the identical port glue with GCC for regression tests. */
#include <sys/types.h>
static inline void jent_get_nstime(uint64_t *out)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    *out = ((uint64_t)hi << 32) | lo;
}
#endif

static inline void jent_memset_secure(void *data, size_t size)
{
    volatile unsigned char *p = (volatile unsigned char *)data;
    while (size--) *p++ = 0;
}

static inline void *jent_zalloc(size_t size)
{
    return calloc(1, size);
}

static inline void jent_zfree(void *data, size_t size)
{
    if (data) {
        jent_memset_secure(data, size);
        free(data);
    }
}

/* No locked-memory or FIPS certification claim. The caller explicitly
 * enables the library's full runtime health tests with JENT_FORCE_FIPS.
 * Internal timer threads are disabled, so these platform hooks are unused.
 * Returning zero for cache size selects the upstream 256 KiB default.
 */
static inline int jent_fips_enabled(void) { return 0; }
static inline long jent_ncpu(void) { return 1; }
static inline uint32_t jent_cache_size_roundup(int all_caches)
{
    (void)all_caches;
    return 0;
}
static inline void jent_yield(void) { }
static inline uint64_t rol64(uint64_t x, int n)
{
    return (x << (n & 63)) | (x >> ((64 - n) & 63));
}
#endif
