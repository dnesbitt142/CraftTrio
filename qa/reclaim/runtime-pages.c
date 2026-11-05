/* Execute the actual OS/2 runtime branch with protected native test pages.
 * These Linux test shims are not evidence of executing an OS/2 kernel. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "os2.h"
#include "wasm-rt.h"
#include "wasm-rt-impl.h"
#include "wasm-rt-exceptions.h"

static struct { void *address; uint32_t size; unsigned char committed[512]; } objects[4];
static unsigned fail_setmem, commit_calls, decommit_calls;
/* Managed build host has no /proc task inventory; ASan/UBSan stay enabled. */
int __lsan_is_turned_off(void) { return 1; }
APIRET DosAllocMem(PVOID *address, ULONG size, ULONG flags) {
    unsigned i;
    (void)flags;
    for(i=0;i<4;i++) if(!objects[i].address) {
        void *p=mmap(NULL,size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if(p==MAP_FAILED)return 8;
        objects[i].address=p;objects[i].size=size;*address=p;return 0;
    }
    return 8;
}
APIRET DosSetMem(PVOID address, ULONG size, ULONG flags) {
    unsigned i,first=0;
    if(fail_setmem==1)return 8;
    assert(((uintptr_t)address&4095U)==0 && size && !(size&4095U));
    for(i=0;i<4;i++)if(objects[i].address && (uintptr_t)address >= (uintptr_t)objects[i].address &&
        (uintptr_t)address + size <= (uintptr_t)objects[i].address + objects[i].size) break;
    assert(i<4); first=(unsigned)(((uintptr_t)address-(uintptr_t)objects[i].address)/4096U);
    if(flags&PAG_COMMIT) {
        if(fail_setmem==3)return 8;
        if(fail_setmem==4) {
            objects[i].committed[first]=1;
            assert(!mprotect(address,4096,PROT_READ|PROT_WRITE));
            memset(address,0x9a,4096);
            return 8;
        }
        commit_calls++;
        memset(objects[i].committed+first,1,size/4096U);
        return mprotect(address,size,PROT_READ|PROT_WRITE)==0?0:87;
    }
    if(flags&PAG_DECOMMIT) {
        decommit_calls++;
        if(fail_setmem>=2) {
            objects[i].committed[first]=0;
            assert(!madvise(address,4096,MADV_DONTNEED));
            assert(!mprotect(address,4096,PROT_NONE));
            return 8;
        }
        memset(objects[i].committed+first,0,size/4096U);
        if(madvise(address,size,MADV_DONTNEED))return 87;
        return mprotect(address,size,PROT_NONE)==0?0:87;
    }
    return 87;
}
APIRET DosQueryMem(PVOID address, ULONG *size, ULONG *flags) {
    unsigned i,first,count,committed;
    for(i=0;i<4;i++)if(objects[i].address && (uintptr_t)address >= (uintptr_t)objects[i].address &&
        (uintptr_t)address < (uintptr_t)objects[i].address + objects[i].size) break;
    if(i==4)return 87;
    first=(unsigned)(((uintptr_t)address-(uintptr_t)objects[i].address)/4096U);
    committed=objects[i].committed[first];count=1;
    while(count*4096U<*size && first+count<objects[i].size/4096U &&
        objects[i].committed[first+count]==committed)count++;
    *size=count*4096U;*flags=PAG_READ|PAG_WRITE|(committed?PAG_COMMIT:0);return 0;
}
APIRET DosFreeMem(PVOID address) {
    unsigned i;
    for(i=0;i<4;i++)if(objects[i].address==address) {
        assert(!munmap(address,objects[i].size));
        objects[i].address=NULL;return 0;
    }
    return 87;
}
static int all_bytes(const uint8_t *p,size_t n,uint8_t byte) {
    size_t i;for(i=0;i<n;i++)if(p[i]!=byte)return 0;return 1;
}
int main(void) {
    wasm_rt_memory_t first={0},second={0};
    uint32_t base=65536U,n=3U*4096U,i;
    wasm_rt_init();
    wasm_rt_allocate_memory(&first,16,32,false);
    wasm_rt_allocate_memory(&second,2,4,false);
    assert(wasm_rt_memory_pages(&first,0,base,n)==0);
    memset(first.data+base,0x5a,n);
    assert(wasm_rt_memory_pages(&first,0,base+4096,4096)!=0);
    assert(wasm_rt_memory_pages(&first,2,base+4096,8192)==0);
    assert(wasm_rt_reclaimed_memory_bytes(&first)==8192);
    assert(wasm_rt_committed_memory_bytes(&first)==first.size-8192);
    assert(all_bytes(first.data+base,4096,0x5a));
    assert(wasm_rt_memory_pages(&first,3,base,n)!=0);
    assert(wasm_rt_memory_pages(&first,1,base+8192,4096)!=0);
    fail_setmem=1;
    assert(wasm_rt_memory_pages(&first,1,base+4096,8192)!=0);
    assert(wasm_rt_reclaimed_memory_bytes(&first)==8192);
    fail_setmem=4;
    assert(wasm_rt_memory_pages(&first,1,base+4096,8192)!=0);
    assert(wasm_rt_reclaimed_memory_bytes(&first)==4096);
    assert(all_bytes(first.data+base,4096,0x5a));
    fail_setmem=0;
    assert(wasm_rt_memory_pages(&first,1,base+4096,8192)==0);
    assert(all_bytes(first.data+base,4096,0x5a));
    assert(all_bytes(first.data+base+4096,8192,0));
    assert(wasm_rt_reclaimed_memory_bytes(&first)==0);
    fail_setmem=1;
    assert(wasm_rt_memory_pages(&first,2,base,n)!=0);
    assert(wasm_rt_reclaimed_memory_bytes(&first)==0);
    assert(all_bytes(first.data+base,4096,0x5a));
    fail_setmem=0;
    /* A failing decommit may have already removed its first page. */
    fail_setmem=2;
    assert(wasm_rt_memory_pages(&first,2,base,n)!=0);
    assert(wasm_rt_reclaimed_memory_bytes(&first)==0);
    memset(first.data+base,0x7b,n); /* rollback restored all access */
    fail_setmem=0;
    assert(wasm_rt_memory_pages(&first,3,base,n)==0);
    assert(wasm_rt_memory_pages(&first,2,base,n)!=0);
    assert(wasm_rt_memory_pages(&first,0,base+1,n)!=0);
    assert(wasm_rt_memory_pages(&first,0,base,0)!=0);
    assert(wasm_rt_memory_pages(&first,0,UINT32_MAX-4095U,8192)!=0);
    assert(wasm_rt_memory_pages(&first,0,(uint32_t)first.size,4096)!=0);
    assert(wasm_rt_memory_pages(&first,99,base,n)!=0);
    assert(wasm_rt_memory_pages(&first,0,base,n)==0);
    fail_setmem=3;
    {
        wasm_rt_trap_t trap=wasm_rt_impl_try();
        if(!trap) {
            (void)wasm_rt_memory_pages(&first,2,base,n);
            assert(!"unrestorable partial failure must stop the engine");
        }
        assert(trap==WASM_RT_TRAP_EXHAUSTION);
    }
    fail_setmem=0;
    /* This engine would now exit; the test restores mappings for cleanup. */
    assert(DosSetMem(first.data+base,n,PAG_COMMIT|PAG_READ|PAG_WRITE)==0);
    assert(wasm_rt_memory_pages(&first,3,base,n)==0);
    puts("PASS checked ownership, prefix/suffix transitions, bounds, zeroed recommit, live-prefix preservation and OS errors");
    for(i=0;i<128;i++)assert(wasm_rt_memory_pages(&first,0,i*4096U,4096)==0);
    assert(wasm_rt_memory_pages(&first,0,128*4096U,4096)!=0);
    assert(wasm_rt_memory_pages(&first,3,127*4096U,4096)==0);
    assert(wasm_rt_memory_pages(&first,0,128*4096U,4096)==0);
    /* A second memory has no aliases with the first; capacity is global and
       checked, so release a slot before registering the second owner. */
    assert(wasm_rt_memory_pages(&first,3,126*4096U,4096)==0);
    assert(wasm_rt_memory_pages(&second,0,4096,4096)==0);
    assert(wasm_rt_memory_pages(&second,2,4096,4096)==0);
    assert(wasm_rt_reclaimed_memory_bytes(&first)==0);
    assert(wasm_rt_reclaimed_memory_bytes(&second)==4096);
    wasm_rt_free_memory(&first);
    assert(wasm_rt_reclaimed_memory_bytes(&first)==0);
    assert(wasm_rt_reclaimed_memory_bytes(&second)==4096);
    assert(wasm_rt_memory_pages(&second,1,4096,4096)==0);
    assert(all_bytes(second.data+4096,4096,0));
    wasm_rt_free_memory(&second);
    wasm_rt_free();
    assert(commit_calls>=4 && decommit_calls>=2);
    puts("PASS ledger exhaustion/reuse, memory isolation, lifetime cleanup and native protected-page operations");
    return 0;
}
