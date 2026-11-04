/* Craft Trio randomness diagnostic; buildable as an OS/2 console program.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include "rng.h"
#include "jitterentropy.h"
#include "jitterentropy-sha3.h"
#include "jitterentropy-gcd.h"

static int failed(const char *stage)
{
    const char *detail = wc_rng_error();
    fprintf(stderr, "FAIL %s%s%s\n", stage, detail ? ": " : "", detail ? detail : "");
    wc_rng_shutdown();
    return 2;
}

static void print_digest(const unsigned char *data, size_t size)
{
    struct jent_sha_ctx hash;
    unsigned char digest[32];
    unsigned int i;
    jent_sha3_256_init(&hash);
    jent_sha3_update(&hash, data, size);
    jent_sha3_final(&hash, digest);
    for (i = 0; i < sizeof(digest); ++i) printf("%02x", (unsigned int)digest[i]);
}

static int check_read(size_t size, unsigned char restart_reference[32])
{
    unsigned char *allocation;
    unsigned char *data;
    size_t i;
    allocation = malloc(size + 32);
    if (!allocation) return failed("test allocation");
    memset(allocation, 0xa5, size + 32);
    data = allocation + 16;
    if (!wc_rng_fill(data, size)) {
        free(allocation);
        return failed("random read");
    }
    for (i = 0; i < 16; ++i) {
        if (allocation[i] != 0xa5 || data[size + i] != 0xa5) {
            free(allocation);
            return failed("buffer guards");
        }
    }
    printf("PASS read bytes=%lu SHA3-256=", (unsigned long)size);
    print_digest(data, size);
    printf("\n");
    fflush(stdout);
    if (size >= 32) memcpy(restart_reference, data, 32);
    free(allocation);
    return 0;
}

int main(int argc, char **argv)
{
    const size_t standard[] = { 1, 16, 32, 255, 4096 };
    unsigned char reference[32] = { 0 }, restarted[32];
    size_t i, requested = 0;
    int kat_only = 0;
    char *end;
    unsigned long parsed;

    if (argc == 2 && strcmp(argv[1], "--kat-only") == 0) kat_only = 1;
    else if (argc == 3 && strcmp(argv[1], "--bytes") == 0) {
        parsed = strtoul(argv[2], &end, 10);
        if (!argv[2][0] || *end || !parsed || parsed > 1048576UL) {
            fprintf(stderr, "Use --bytes 1..1048576\n");
            return 1;
        }
        requested = (size_t)parsed;
    } else if (argc != 1) {
        fprintf(stderr, "Usage: RNGTEST [--kat-only | --bytes 1..1048576]\n");
        return 1;
    }

#if defined(__OS2__)
    puts("Craft Trio RNGTEST: OpenWatcom i386 OS/2 console diagnostic");
#else
    puts("Craft Trio RNGTEST: OpenWatcom i386 Linux ABI diagnostic; OS/2 RNG glue");
#endif
    printf("jitterentropy version=%lu pointer=%lu uint64=%lu\n",
           (unsigned long)JENT_VERSION, (unsigned long)sizeof(void *), (unsigned long)sizeof(uint64_t));
    if (sizeof(void *) != 4 || sizeof(uint64_t) != 8 || ULONG_MAX != 0xffffffffUL)
        return failed("32-bit type assumptions");
    if (jent_sha3_tester()) return failed("upstream SHA3-256/XDRBG known-answer tests");
    puts("PASS upstream SHA3-256/XDRBG known-answer tests");
    if (jent_gcd_selftest()) return failed("upstream GCD known-answer test");
    puts("PASS upstream GCD known-answer test");
    fflush(stdout);
    if (kat_only) return 0;
    if (wc_rng_prepare()) return failed("native RNG initialization");
    puts("PASS native RNG initialization and startup health tests");
    if (!wc_rng_fill(NULL, 0)) return failed("zero-length read");
    if (wc_rng_fill(NULL, 1)) return failed("reject NULL nonempty output");
    puts("PASS zero-length read and NULL output rejection");
    if (requested) {
        if (check_read(requested, reference)) return 2;
    } else {
        for (i = 0; i < sizeof(standard) / sizeof(standard[0]); ++i)
            if (check_read(standard[i], reference)) return 2;
    }
    if (!wc_rng_fill(reference, sizeof(reference))) return failed("reference read");
    wc_rng_shutdown();
    if (wc_rng_prepare()) return failed("restart initialization");
    if (!wc_rng_fill(restarted, sizeof(restarted))) return failed("restart read");
    if (!memcmp(reference, restarted, sizeof(reference))) return failed("independent restart output");
    wc_rng_shutdown();
    puts("PASS shutdown/reinitialize independent output");
    puts("PASS all requested tests; no ENTROPY.BIN is read or written");
    return 0;
}
