#!/usr/bin/env python3
"""Build/run actual host RNG and WASI wrapper with only OS/2 APIs stubbed.

This verifies portable C behavior on the build host, not the OS/2 loader/PM.
Run from any directory: python3 path/to/qa/rng/test-host.py [build-directory]
"""
from pathlib import Path
import hashlib
import os
import subprocess
import sys

KIT = Path(__file__).resolve().parents[2]
BUILD = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else KIT / "build" / "rng-tests"
BUILD.mkdir(parents=True, exist_ok=True)
INCLUDE = BUILD / "include"
INCLUDE.mkdir(exist_ok=True)
VENDOR = KIT / "native" / "vendor" / "jitterentropy"
HOST = KIT / "native" / "host"
LOG = []


def run(argv, **kwargs):
    result = subprocess.run([str(a) for a in argv], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=240, **kwargs)
    LOG.append("$ " + " ".join(str(a) for a in argv))
    LOG.append(result.stdout)
    print(result.stdout, end="", flush=True)
    if result.returncode:
        raise RuntimeError(f"exit {result.returncode}: {argv[0]}")
    return result.stdout


(INCLUDE / "os2.h").write_text("""#include <stdint.h>
typedef uint32_t ULONG;
#define QSV_MS_COUNT 14
unsigned long DosQuerySysInfo(unsigned long, unsigned long, void *, unsigned long);
unsigned long DosSleep(unsigned long);
""")

(BUILD / "test-services.c").write_text(r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "craftpm.h"
#include "guest_services.h"
#include "rng.h"

static uint8_t memory[262144];
static int memory_available = 1;
int __lsan_is_turned_off(void) { return 1; }
#ifdef WC_RNG_SANITIZER
#define LARGE_REQUEST 1025U
#else
#define LARGE_REQUEST 65537U
#endif
static const WcPmConfig config = { .engine_log = "TESTENGINE.LOG" };
uint8_t *wc_pm_memory(size_t *size)
{ *size = sizeof(memory); return memory_available ? memory : NULL; }
const WcPmConfig *wc_pm_config(void) { return &config; }
unsigned long DosQuerySysInfo(unsigned long a, unsigned long b, void *p, unsigned long n)
{ (void)a; (void)b; memset(p, 0, n); return 0; }
unsigned long DosSleep(unsigned long n) { (void)n; return 0; }
int32_t wc_gui_save(const char *a,uint32_t b,const void *c,uint32_t d,uint32_t e,char *f,uint32_t g)
{ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;return -1; }

static int all_equal(size_t start, size_t size, unsigned char value)
{
    size_t i;
    for (i = start; i < start + size; ++i) if (memory[i] != value) return 0;
    return 1;
}

int main(void)
{
    unsigned char first[64];
    size_t i;
    const char *error = wc_rng_prepare();
    if (error) { fprintf(stderr, "RNG preflight failed: %s\n", error); return 1; }
    assert(wc_rng_error() == NULL);
    memset(memory, 0xa5, sizeof(memory));
    assert(wc_wasi_random_get(64, 64) == 0);
    assert(!all_equal(64, 64, 0xa5));
    assert(all_equal(0, 64, 0xa5));
    assert(all_equal(128, sizeof(memory) - 128, 0xa5));
    memcpy(first, memory + 64, sizeof(first));
    printf("SAMPLE64 ");
    for (i = 0; i < sizeof(first); ++i) printf("%02x", first[i]);
    puts("");
    assert(wc_wasi_random_get(64, 64) == 0);
    assert(memcmp(first, memory + 64, sizeof(first)) != 0);
    puts("PASS complete fills, guard bytes, repeated requests differ");

    memset(memory, 0x33, sizeof(memory));
    assert(wc_wasi_random_get(UINT32_MAX, 0) == 0);
    assert(wc_wasi_random_get(sizeof(memory), 0) == 0);
    assert(wc_wasi_random_get(UINT32_MAX, 1) == 21);
    assert(wc_wasi_random_get(1, UINT32_MAX) == 21);
    assert(wc_wasi_random_get(sizeof(memory), 1) == 21);
    assert(wc_wasi_random_get(sizeof(memory) - 1, 2) == 21);
    assert(all_equal(0, sizeof(memory), 0x33));
    memory_available = 0;
    assert(wc_wasi_random_get(0, 1) == 21);
    memory_available = 1;
    assert(wc_wasi_random_get(sizeof(memory) - 1, 1) == 0);
    assert(all_equal(0, sizeof(memory) - 1, 0x33));
    puts("PASS zero length, invalid ranges/overflow, unavailable memory, boundary fill");

    memset(memory, 0xa5, sizeof(memory));
    assert(wc_wasi_random_get(32, LARGE_REQUEST) == 0);
    assert(!all_equal(32, LARGE_REQUEST, 0xa5));
    assert(all_equal(0, 32, 0xa5));
    assert(all_equal(32 + LARGE_REQUEST, sizeof(memory) - 32 - LARGE_REQUEST, 0xa5));
    assert(wc_wasi_random_get(32, LARGE_REQUEST) == 0);
    printf("PASS repeated %u-byte requests\n", LARGE_REQUEST);
    wc_guest_services_shutdown();
    assert(wc_rng_error() == NULL);
    assert(wc_wasi_random_get(32, 64) == 0);
    assert(memcmp(first, memory + 32, sizeof(first)) != 0);
    wc_guest_services_shutdown();
    puts("PASS shutdown/reinitialize without files");
    return 0;
}
''')

compiler = os.environ.get("CC", "gcc")
base_flags = [compiler, "-std=c99", "-O0", "-g", "-DWC_JENT_PORT_TEST",
              "-I", INCLUDE, "-I", HOST, "-I", VENDOR]
vendor_sources = sorted((VENDOR / "src").glob("*.c"))
sources = [HOST / "guest_services.c", HOST / "rng.c", *vendor_sources, BUILD / "test-services.c"]

try:
    executable = BUILD / "test-services"
    run([*base_flags, *sources, "-o", executable])
    samples = []
    for name in ["empty-working-directory", "other-working-directory"]:
        cwd = BUILD / name
        cwd.mkdir(exist_ok=True)
        assert not (cwd / "ENTROPY.BIN").exists()
        out = run([executable], cwd=cwd)
        samples.append(next(line.split()[1] for line in out.splitlines() if line.startswith("SAMPLE64 ")))
    # A bogus legacy filename must not influence the native source either.
    legacy = BUILD / "bogus-legacy-file"
    legacy.mkdir(exist_ok=True)
    (legacy / "ENTROPY.BIN").write_bytes(bytes(16))
    out = run([executable], cwd=legacy)
    samples.append(next(line.split()[1] for line in out.splitlines() if line.startswith("SAMPLE64 ")))
    assert len(set(samples)) == len(samples)
    LOG.append("PASS fresh-process samples differ across unrelated CWDs; bogus ENTROPY.BIN ignored")
    print(LOG[-1], flush=True)

    faults = BUILD / "test-faults"
    sanitizers = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    run([*base_flags, *sanitizers, "-DWC_RNG_FAULT_TEST", HOST / "rng.c",
         KIT / "qa/rng/test-faults.c", "-o", faults])
    # This managed runtime has no /proc task inventory for LeakSanitizer.
    # Address and undefined-behavior checks remain enabled.
    run([faults], cwd=BUILD, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})

    sanitized = BUILD / "test-services-sanitized"
    run([*base_flags, *sanitizers, "-DWC_RNG_SANITIZER", *sources, "-o", sanitized])
    run([sanitized], cwd=BUILD / "empty-working-directory",
        env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
    LOG.append("PASS AddressSanitizer/UndefinedBehaviorSanitizer: actual source and injected faults")
    print(LOG[-1], flush=True)
except Exception as error:
    LOG.append("FAIL " + str(error))
    raise
finally:
    (BUILD / "host-regression.log").write_text("\n".join(LOG))
