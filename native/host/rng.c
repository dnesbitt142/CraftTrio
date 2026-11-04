/* Native, file-independent randomness for the three Craft applications.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "rng.h"
#include "jitterentropy.h"

#define WC_RNG_OSR 3U
#define WC_RNG_FLAGS (JENT_FORCE_FIPS | JENT_DISABLE_INTERNAL_TIMER)

static struct rand_data *collector;
static char failure[384];

#if defined(WC_RNG_FAULT_TEST)
/* Only the test binary supplies this function. Never linked into CRAFTPM. */
extern int wc_rng_cpu_has_tsc(void);
#elif defined(__WATCOMC__)
static int wc_rng_cpu_has_tsc(void);
#pragma aux wc_rng_cpu_has_tsc = \
    "pushfd" \
    "pop eax" \
    "mov ecx,eax" \
    "xor eax,00200000h" \
    "push eax" \
    "popfd" \
    "pushfd" \
    "pop eax" \
    "push ecx" \
    "popfd" \
    "xor eax,ecx" \
    "and eax,00200000h" \
    "jz no_tsc" \
    "xor eax,eax" \
    "db 0fh,0a2h" \
    "cmp eax,1" \
    "jb no_tsc" \
    "mov eax,1" \
    "db 0fh,0a2h" \
    "mov eax,edx" \
    "shr eax,4" \
    "and eax,1" \
    "jmp tsc_done" \
    "no_tsc: xor eax,eax" \
    "tsc_done:" \
    value [eax] modify exact [eax ebx ecx edx];
#else
#include <cpuid.h>
static int wc_rng_cpu_has_tsc(void)
{
    unsigned int a, b, c, d;
    return __get_cpuid(1, &a, &b, &c, &d) && (d & (1U << 4));
}
#endif

static void record_failure(const char *stage, long code)
{
    snprintf(failure, sizeof(failure),
        "Native random source failed (%s, code %ld).\n"
        "The CPU timing entropy health tests did not pass. On a virtual machine, "
        "check its CPU and timestamp-counter settings. A Pentium or newer CPU "
        "with an accessible TSC is required. See STARTUP.TXT. "
        "ENTROPY.BIN is no longer used.", stage, code);
    failure[sizeof(failure) - 1] = 0;
    if (collector) jent_entropy_collector_free(collector);
    collector = NULL;
}

const char *wc_rng_error(void)
{
    return failure[0] ? failure : NULL;
}

const char *wc_rng_prepare(void)
{
    int code;
    char probe[32];
    ssize_t count;
    if (failure[0]) return failure;
    if (collector) return NULL;
    if (!wc_rng_cpu_has_tsc()) {
        record_failure("CPU has no timestamp counter", -1);
        return failure;
    }
    code = jent_entropy_init_ex(WC_RNG_OSR, WC_RNG_FLAGS);
    if (code) {
        record_failure("initialization", code);
        return failure;
    }
    collector = jent_entropy_collector_alloc(WC_RNG_OSR, WC_RNG_FLAGS);
    if (!collector) {
        record_failure("collector allocation or startup health test", -1);
        return failure;
    }
    count = jent_read_entropy(collector, probe, sizeof(probe));
    jent_memset_secure(probe, sizeof(probe));
    if (count != (ssize_t)sizeof(probe)) {
        record_failure("startup generation health test", (long)count);
        return failure;
    }
    return NULL;
}

int wc_rng_fill(void *data, size_t size)
{
    ssize_t count;
    if (!size) return 1;
    if (!data) return 0;
    if (wc_rng_prepare()) {
        jent_memset_secure(data, size);
        return 0;
    }
    /* Guest memory is at most 128 MiB, well below 32-bit SSIZE_MAX. */
    if (size > (size_t)LONG_MAX) {
        jent_memset_secure(data, size);
        record_failure("request exceeds supported size", -1);
        return 0;
    }
    count = jent_read_entropy(collector, (char *)data, size);
    if (count != (ssize_t)size) {
        jent_memset_secure(data, size);
        record_failure("generation health test", (long)count);
        return 0;
    }
    return 1;
}

void wc_rng_shutdown(void)
{
    if (collector) jent_entropy_collector_free(collector);
    collector = NULL;
    memset(failure, 0, sizeof(failure));
}
