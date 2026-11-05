/* QA-only OS/2 memory API shim for the exact i386/OMF engine in a Linux ELF.
 * Protected pages exercise the production runtime's __OS2__ reclaim branch.
 * This is not execution of an OS/2 kernel or Presentation Manager. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os2.h"

#define NATIVE_PAGE 4096U
#define MAX_PAGES (128U * 1024U * 1024U / NATIVE_PAGE)
static struct {
    unsigned char *base;
    ULONG size;
    unsigned char committed[MAX_PAGES];
} mappings[4];
static unsigned commits, decommits, queries;
static ULONG decommitted_bytes;

/* Linux i386 syscall numbers from asm/unistd_32.h; no foreign libc ABI. */
static long linux_mmap(ULONG *args);
#pragma aux linux_mmap = "mov eax,90" "int 80h" parm [ebx] value [eax] modify exact [eax];
static long linux_munmap(void *base, ULONG size);
#pragma aux linux_munmap = "mov eax,91" "int 80h" parm [ebx] [ecx] value [eax] modify exact [eax];
static long linux_protect(void *base, ULONG size, ULONG flags);
#pragma aux linux_protect = "mov eax,125" "int 80h" parm [ebx] [ecx] [edx] value [eax] modify exact [eax];
static long linux_discard(void *base, ULONG size, ULONG advice);
#pragma aux linux_discard = "mov eax,219" "int 80h" parm [ebx] [ecx] [edx] value [eax] modify exact [eax];

static unsigned find(void *address, ULONG length) {
    unsigned i;
    for(i=0;i<4;++i) if(mappings[i].base &&
        (ULONG)address >= (ULONG)mappings[i].base &&
        (ULONG)address - (ULONG)mappings[i].base <= mappings[i].size &&
        length <= mappings[i].size - ((ULONG)address - (ULONG)mappings[i].base)) return i;
    return 4;
}
APIRET DosAllocMem(PVOID *out, ULONG size, ULONG flags) {
    unsigned i;
    ULONG args[6] = {0, 0, 0, 0x22, 0xffffffffUL, 0};
    long result;
    (void)flags;
    if(!size || size > MAX_PAGES * NATIVE_PAGE) return 87;
    for(i=0;i<4;++i) if(!mappings[i].base) break;
    if(i==4) return 8;
    args[1]=size; result=linux_mmap(args);
    if((ULONG)result >= 0xfffff001UL) return 8;
    mappings[i].base=(unsigned char*)result; mappings[i].size=size;
    memset(mappings[i].committed,0,sizeof(mappings[i].committed));
    *out=mappings[i].base;
    printf("protected_arena base=%p reserved=%lu\n",*out,(unsigned long)size);fflush(stdout);
    return 0;
}
APIRET DosSetMem(PVOID address, ULONG length, ULONG flags) {
    unsigned i=find(address,length), page, count;
    long result;
    if(i==4 || !length || ((ULONG)address & 4095U) || (length & 4095U)) return 87;
    page=((ULONG)address-(ULONG)mappings[i].base)/NATIVE_PAGE; count=length/NATIVE_PAGE;
    if(flags&PAG_DECOMMIT) {
        if(linux_discard(address,length,4)<0) return 8;
        result=linux_protect(address,length,0);
        if(result<0) return 8;
        memset(mappings[i].committed+page,0,count);++decommits;decommitted_bytes+=length;
    } else {
        if(linux_protect(address,length,3)<0) return 8;
        memset(mappings[i].committed+page,1,count);++commits;
    }
    return 0;
}
APIRET DosQueryMem(PVOID address, ULONG *length, ULONG *flags) {
    unsigned i=find(address,1), page, count=1;
    ULONG available;
    if(i==4 || !*length || ((ULONG)address&4095U)) return 87;
    ++queries;page=((ULONG)address-(ULONG)mappings[i].base)/NATIVE_PAGE;
    available=(mappings[i].size-((ULONG)address-(ULONG)mappings[i].base))/NATIVE_PAGE;
    while(count<available && count*NATIVE_PAGE<*length &&
          mappings[i].committed[page+count]==mappings[i].committed[page]) ++count;
    *length=count*NATIVE_PAGE;
    *flags=mappings[i].committed[page] ? PAG_COMMIT|PAG_READ|PAG_WRITE : 0;
    return 0;
}
APIRET DosFreeMem(PVOID address) {
    unsigned i=find(address,1);
    if(i==4 || address!=mappings[i].base) return 87;
    if(linux_munmap(address,mappings[i].size)<0) return 87;
    mappings[i].base=NULL;
    printf("protected_reclaim commits=%u decommits=%u queries=%u cumulative_decommit_bytes=%lu\n",
           commits,decommits,queries,(unsigned long)decommitted_bytes);fflush(stdout);
    if(!decommits || !queries) { fputs("FAIL protected reclamation path was not exercised\n",stderr);exit(2); }
    return 0;
}
