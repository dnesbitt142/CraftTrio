/* Exercise the production OS/2 branch with protected demand-zero API shims.
 * mincore is sampled before byte checks touch pages; this is not OS/2 RSS. */
#define _GNU_SOURCE
static int qa_fresh_growth;
#define main reclaim_main
#include "protected-base.inc"
#undef main

static unsigned resident(const uint8_t *address, size_t length)
{
    unsigned char flags[1024];
    unsigned count = 0, i;
    assert(length / 4096 <= sizeof(flags));
    assert(mincore((void *)address, length, flags) == 0);
    for (i = 0; i < length / 4096; ++i) if (flags[i] & 1) ++count;
    return count;
}

int main(int argc, char **argv)
{
    wasm_rt_memory_t memory = {0};
    unsigned eager = argc == 2 && !strcmp(argv[1], "eager");
    unsigned first, grown;
    const uint64_t bad = (uint64_t)-1;
    wasm_rt_init();
    wasm_rt_allocate_memory(&memory, 16, 64, false);
    first = resident(memory.data, (size_t)memory.size);
    assert(first == (eager ? 256U : 0U));
    assert(all_bytes(memory.data, (size_t)memory.size, 0));
    memset(memory.data, 0x53, (size_t)memory.size);
    assert(wasm_rt_grow_memory(&memory, 16) == 16);
    grown = resident(memory.data + 1048576, 1048576);
    assert(grown == (eager ? 256U : 0U));
    assert(all_bytes(memory.data, 1048576, 0x53));
    assert(all_bytes(memory.data + 1048576, 1048576, 0));
    memset(memory.data + 1048576, 0x71, 1048576);
    assert(wasm_rt_grow_memory(&memory, 0) == 32);
    assert(wasm_rt_grow_memory(&memory, 33) == bad);
    assert(memory.pages == 32 && memory.size == 2097152);
    fail_setmem = 1;
    assert(wasm_rt_grow_memory(&memory, 16) == bad);
    assert(memory.pages == 32 && memory.size == 2097152);
    fail_setmem = 0;
    assert(wasm_rt_grow_memory(&memory, 16) == 32);
    assert(all_bytes(memory.data, 1048576, 0x53));
    assert(all_bytes(memory.data + 1048576, 1048576, 0x71));
    assert(all_bytes(memory.data + 2097152, 1048576, 0));
    /* A partial fresh commit remains zero-backed. It cannot be dirtied by
     * valid callers because memory.grow has not changed the logical size. */
    fail_setmem = 4;
    qa_fresh_growth = 1;
    assert(wasm_rt_grow_memory(&memory, 16) == bad);
    assert(memory.pages == 48 && memory.size == 3145728);
    fail_setmem = 0;
    assert(wasm_rt_grow_memory(&memory, 16) == 48);
    qa_fresh_growth = 0;
    assert(all_bytes(memory.data, 1048576, 0x53));
    assert(all_bytes(memory.data + 1048576, 1048576, 0x71));
    assert(all_bytes(memory.data + 2097152, 2097152, 0));
    assert(wasm_rt_grow_memory(&memory, 1) == bad);
    wasm_rt_free_memory(&memory);
    wasm_rt_free();
    printf("PASS fresh demand-zero initial_resident_pages=%u grown_resident_pages=%u expected_eager=%u\n",
           first, grown, eager);
    puts("PASS fresh zero contents, growth prefix, failed and partial grow retries, limits, lifetime");
    /* The inherited suite deliberately dirties cache-recommit failures and
     * checks that the runtime still clears the restored cache on retry. */
    return reclaim_main();
}
