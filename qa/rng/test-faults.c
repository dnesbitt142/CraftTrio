/* Deterministic error-path regression tests for native/host/rng.c.
 * The test substitutes the entropy-library boundary, never a release build.
 * Compile with WC_RNG_FAULT_TEST and WC_JENT_PORT_TEST.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "rng.h"
#include "jitterentropy.h"

enum Mode { GOOD, NO_TSC, INIT_FAIL, ALLOC_FAIL, START_FAIL, START_SHORT,
            RUN_FAIL, RUN_SHORT };
static enum Mode mode;
static int init_calls, alloc_calls, read_calls, free_calls;
static int dummy;

/* LeakSanitizer needs /proc, unavailable in the managed build sandbox. */
int __lsan_is_turned_off(void) { return 1; }

int wc_rng_cpu_has_tsc(void) { return mode != NO_TSC; }

int jent_entropy_init_ex(unsigned int osr, unsigned int flags)
{
    assert(osr == 3);
    assert(flags == (JENT_FORCE_FIPS | JENT_DISABLE_INTERNAL_TIMER));
    ++init_calls;
    return mode == INIT_FAIL ? 9 : 0;
}

struct rand_data *jent_entropy_collector_alloc(unsigned int osr,
                                               unsigned int flags)
{
    assert(osr == 3);
    assert(flags == (JENT_FORCE_FIPS | JENT_DISABLE_INTERNAL_TIMER));
    ++alloc_calls;
    return mode == ALLOC_FAIL ? NULL : (struct rand_data *)&dummy;
}

void jent_entropy_collector_free(struct rand_data *ec)
{
    assert(ec == (struct rand_data *)&dummy);
    ++free_calls;
}

ssize_t jent_read_entropy(struct rand_data *ec, char *data, size_t size)
{
    assert(ec == (struct rand_data *)&dummy);
    ++read_calls;
    /* Deliberately dirty partial output before reporting failure. */
    memset(data, 0x5a, size / 2);
    if ((mode == START_FAIL && read_calls == 1) ||
        (mode == RUN_FAIL && read_calls == 2)) return -7;
    if ((mode == START_SHORT && read_calls == 1) ||
        (mode == RUN_SHORT && read_calls == 2)) return (ssize_t)(size / 2);
    memset(data, (unsigned char)(read_calls + 0x20), size);
    return (ssize_t)size;
}

static int all_zero(const unsigned char *data, size_t size)
{
    size_t i;
    for (i = 0; i < size; ++i) if (data[i]) return 0;
    return 1;
}

static void begin(enum Mode next)
{
    wc_rng_shutdown();
    mode = next;
    init_calls = alloc_calls = read_calls = free_calls = 0;
    assert(wc_rng_error() == NULL);
}

static void startup_failure(enum Mode next, const char *fragment)
{
    const char *error;
    unsigned char bytes[64];
    int called;
    begin(next);
    error = wc_rng_prepare();
    assert(error && strstr(error, fragment));
    assert(strstr(error, "ENTROPY.BIN is no longer used"));
    called = init_calls + alloc_calls + read_calls;
    assert(wc_rng_prepare() == error);
    memset(bytes, 0xa5, sizeof(bytes));
    assert(!wc_rng_fill(bytes, sizeof(bytes)));
    assert(all_zero(bytes, sizeof(bytes)));
    assert(init_calls + alloc_calls + read_calls == called);
    assert(wc_rng_fill(NULL, 0));
    assert(!wc_rng_fill(NULL, 1));
    wc_rng_shutdown();
    assert(wc_rng_error() == NULL);
    printf("PASS startup failure: %s\n", fragment);
}

static void runtime_failure(enum Mode next)
{
    unsigned char bytes[64];
    int called;
    begin(next);
    assert(wc_rng_prepare() == NULL);
    assert(read_calls == 1);
    assert(wc_rng_prepare() == NULL && read_calls == 1);
    memset(bytes, 0xa5, sizeof(bytes));
    assert(!wc_rng_fill(bytes, sizeof(bytes)));
    assert(all_zero(bytes, sizeof(bytes)));
    assert(wc_rng_error() && strstr(wc_rng_error(), "generation health test"));
    assert(free_calls == 1);
    called = read_calls;
    memset(bytes, 0x66, sizeof(bytes));
    assert(!wc_rng_fill(bytes, sizeof(bytes)));
    assert(all_zero(bytes, sizeof(bytes)) && read_calls == called);
    wc_rng_shutdown();
    assert(wc_rng_error() == NULL && free_calls == 1);
    mode = GOOD;
    assert(wc_rng_prepare() == NULL);
    assert(wc_rng_fill(bytes, sizeof(bytes)));
    assert(bytes[0] && bytes[sizeof(bytes) - 1]);
    wc_rng_shutdown();
    printf("PASS runtime %s: partial output wiped, error sticky, reset recovers\n",
           next == RUN_FAIL ? "failure" : "short read");
}

int main(void)
{
    unsigned char bytes[64];
    begin(GOOD);
    assert(wc_rng_fill(NULL, 0));
    assert(!wc_rng_fill(NULL, 1));
    assert(init_calls == 0);
    assert(wc_rng_prepare() == NULL && init_calls == 1 && read_calls == 1);
    assert(wc_rng_fill(bytes, sizeof(bytes)) && read_calls == 2);
    assert(bytes[0] == 0x22 && bytes[63] == 0x22);
    assert(wc_rng_prepare() == NULL && init_calls == 1);
    wc_rng_shutdown();
    assert(free_calls == 1);
    puts("PASS success: preflight read, cached collector, complete output, cleanup");
    startup_failure(NO_TSC, "CPU has no timestamp counter");
    startup_failure(INIT_FAIL, "initialization, code 9");
    startup_failure(ALLOC_FAIL, "collector allocation or startup health test");
    startup_failure(START_FAIL, "startup generation health test, code -7");
    startup_failure(START_SHORT, "startup generation health test, code 16");
    runtime_failure(RUN_FAIL);
    runtime_failure(RUN_SHORT);
    puts("PASS all deterministic RNG error-path regressions");
    return 0;
}
