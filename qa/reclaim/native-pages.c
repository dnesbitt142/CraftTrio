/* Native OS/2 memory API diagnostic. Not executed on OS/2 during this build. */
#define INCL_DOSMEMMGR
#include <os2.h>
#include <stdio.h>
#include <string.h>
#include "wasm-rt.h"

static int failure(const char* stage) {
    fprintf(stderr, "FAIL %s\n", stage);
    return 1;
}
int main(void) {
    wasm_rt_memory_t memory = {0};
    uint32_t base = 65536U, length = 4U * 4096U;
    ULONG query_size, flags;
    unsigned int i;
    wasm_rt_init();
    wasm_rt_allocate_memory(&memory, 4, 8, false);
    if (wasm_rt_memory_pages(&memory, 0, base, length)) return failure("register owner span");
    memset(memory.data + base, 0x5a, length);
    for (i = 0; i < 16; ++i) {
        if (wasm_rt_memory_pages(&memory, 2, base + 4096, length - 4096))
            return failure("DosSetMem decommit");
        query_size = length - 4096; flags = 0;
        if (DosQueryMem(memory.data + base + 4096, &query_size, &flags) ||
            (flags & PAG_COMMIT)) return failure("DosQueryMem confirms uncommitted tail");
        if (wasm_rt_reclaimed_memory_bytes(&memory) != length - 4096)
            return failure("actual reclaimed byte count");
        if (memory.data[base] != 0x5a || memory.data[base + 4095] != 0x5a)
            return failure("live prefix preserved");
        if (wasm_rt_memory_pages(&memory, 1, base + 4096, length - 4096))
            return failure("DosSetMem recommit");
        if (memory.data[base + 4096] || memory.data[base + length - 1])
            return failure("recommitted bytes zeroed");
        memset(memory.data + base + 4096, (int)i, length - 4096);
    }
    if (wasm_rt_reclaimed_memory_bytes(&memory)) return failure("committed count restored");
    if (wasm_rt_memory_pages(&memory, 3, base, length)) return failure("unregister owner");
    wasm_rt_free_memory(&memory);
    wasm_rt_free();
    puts("PASS OS/2 native register/decommit/query/recommit/zeroing/prefix/lifetime checks");
    return 0;
}
