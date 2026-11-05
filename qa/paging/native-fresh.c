/* Actual OS/2 zero-fill contract probe. This checks contents, not RSS.
 * Whole-system committable availability is sampled before full byte checks. */
#define INCL_DOSMEMMGR
#define INCL_DOSMISC
#include <os2.h>
#include <stdio.h>
#include <string.h>
#include "wasm-rt.h"

static int fail(const char *stage) { printf("FAIL %s\n", stage); return 1; }
static int bytes_are(const unsigned char *p, unsigned long length, int byte)
{
    unsigned long i;
    for (i = 0; i < length; ++i) if (p[i] != (unsigned char)byte) return 0;
    return 1;
}
static ULONG available(void)
{
    ULONG result = 0;
    DosQuerySysInfo(QSV_TOTAVAILMEM, QSV_TOTAVAILMEM, &result, sizeof(result));
    return result;
}
int main(void)
{
    wasm_rt_memory_t memory = {0};
    ULONG before, initial, grown;
    const uint64_t bad = (uint64_t)-1;
    wasm_rt_init();
    before = available();
    wasm_rt_allocate_memory(&memory, 128, 384, false);
    initial = available();
    if (!bytes_are(memory.data, 8388608UL, 0)) return fail("initial fresh zero bytes");
    memset(memory.data, 0x53, 8388608UL);
    if (wasm_rt_grow_memory(&memory, 128) != 128) return fail("first growth");
    grown = available();
    if (!bytes_are(memory.data, 8388608UL, 0x53)) return fail("live prefix after growth");
    if (!bytes_are(memory.data + 8388608UL, 8388608UL, 0)) return fail("fresh growth zero bytes");
    memset(memory.data + 8388608UL, 0x71, 8388608UL);
    if (wasm_rt_grow_memory(&memory, 0) != 256) return fail("zero growth");
    if (wasm_rt_grow_memory(&memory, 129) != bad) return fail("maximum rejection");
    if (memory.pages != 256 || memory.size != 16777216UL) return fail("failed growth metadata");
    if (wasm_rt_grow_memory(&memory, 128) != 256) return fail("second growth");
    if (!bytes_are(memory.data, 8388608UL, 0x53) ||
        !bytes_are(memory.data + 8388608UL, 8388608UL, 0x71) ||
        !bytes_are(memory.data + 16777216UL, 8388608UL, 0)) return fail("second growth preservation");
    if (wasm_rt_grow_memory(&memory, 1) != bad) return fail("full memory limit");
    wasm_rt_free_memory(&memory);
    wasm_rt_free();
    printf("SYSTEM committable_available_before=%lu after_initial=%lu after_growth=%lu (not RSS)\n",
           before, initial, grown);
    puts("PASS OS/2 fresh zero/growth/prefix/limit/lifetime contract");
    return 0;
}
