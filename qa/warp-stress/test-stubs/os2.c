#include "os2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
static ULONG now, owned, attempts;
static PVOID allocations[300];
static ULONG sizes[300];
static int setup;
/* This managed host has no /proc for LeakSanitizer's thread inspection. */
int __lsan_is_turned_off(void) { return 1; }
static ULONG env_number(const char *key, ULONG fallback) {
    const char *s = getenv(key);
    return s ? (ULONG)strtoul(s, NULL, 10) : fallback;
}
static void finish(void) {
    unsigned n;
    for (n = 0; n < 300; ++n) if (allocations[n]) {
        fprintf(stderr, "TEST ERROR leaked allocation %u\n", n);
        abort();
    }
    fprintf(stderr, "TEST owned bytes at exit: %lu\n", owned);
}
static APIRET process_state(ULONG flags,ULONG level,PID pid,TID tid,PVOID out,ULONG size) {
    unsigned char *base=out;
    QSPTRREC *root=out;
    QSPREC *first,*second;
    QSLREC *module;
    const char *mode=getenv("WARP_TEST_TASKS");
    (void)flags; (void)level; (void)pid; (void)tid;
    if (!mode) return ERROR_INVALID_FUNCTION;
    if (!strcmp(mode,"overflow") && size==65536) return ERROR_BUFFER_OVERFLOW;
    first=(QSPREC*)(base+64);
    root->pProcRec=first;
    first->RecType=QS_PROCESS; first->pid=14; first->ppid=2;
    if (!strcmp(mode,"invalid")) { first->pThrdRec=NULL; return 0; }
    first->pThrdRec=(QSTREC*)(first+1); first->cTCB=2; first->hMte=7;
    first->pThrdRec[0].systime=5; first->pThrdRec[0].usertime=9;
    first->pThrdRec[1].systime=7; first->pThrdRec[1].usertime=11;
    second=(QSPREC*)(first->pThrdRec+2);
    second->RecType=QS_PROCESS; second->pid=15; second->ppid=2;
    second->pThrdRec=(QSTREC*)(second+1); second->cTCB=1; second->hMte=7;
    module=(QSLREC*)(base+8192); root->pLibRec=module;
    module->hmte=7; module->pName=base+16384;
    strcpy((char*)module->pName,"C:\\CraftTrio\\Word\"Craft.EXE");
    return 0;
}
APIRET DosQuerySysInfo(ULONG first,ULONG last,PVOID out,ULONG bytes) {
    ULONG *p = out, i;
    if ((last-first+1)*sizeof(ULONG) > bytes) abort();
    for (i=first;i<=last;++i) {
        ULONG n=0;
        if (i==QSV_MS_COUNT) n=now++;
        if (i==QSV_PAGE_SIZE) n=4096;
        if (i==17) n=128UL*1048576;
        if (i==18) n=32UL*1048576;
        if (i==19) n=env_number("WARP_TEST_AVAILABLE_MIB",256)*1048576-owned;
        if (i==20 || i==21) n=128UL*1048576;
        *p++=n;
    }
    return 0;
}
APIRET DosQueryPathInfo(const char *path,ULONG level,PVOID out,ULONG bytes) {
    struct stat s; (void)level; (void)bytes;
    if (stat(path,&s)) return 2;
    ((FILESTATUS3*)out)->cbFile=(ULONG)s.st_size;
    return 0;
}
APIRET DosSleep(ULONG ms) {
    now+=ms;
    if (env_number("WARP_TEST_INTERRUPT_MS",0) && now>=env_number("WARP_TEST_INTERRUPT_MS",0))
        raise(SIGINT);
    return 0;
}
APIRET DosGetDateTime(DATETIME *d) {
    memset(d,0,sizeof(*d)); d->year=2026; d->month=11; d->day=4; return 0;
}
APIRET DosSetPriority(ULONG s,ULONG c,int d,ULONG p) {
    (void)s;(void)c;(void)d;(void)p; return 0;
}
APIRET DosAllocMem(PPVOID p,ULONG bytes,ULONG flags) {
    unsigned i; (void)flags;
    if (!setup) { atexit(finish); setup=1; }
    ++attempts;
    if (attempts==env_number("WARP_TEST_FAIL_AT",0)) return ERROR_NOT_ENOUGH_MEMORY;
    *p=calloc(1,bytes);
    if (!*p) return ERROR_NOT_ENOUGH_MEMORY;
    for (i=0;i<300;++i) if (!allocations[i]) {
        allocations[i]=*p; sizes[i]=bytes; owned+=bytes; return 0;
    }
    abort(); return 8;
}
APIRET DosFreeMem(PVOID p) {
    unsigned i;
    for (i=0;i<300;++i) if (allocations[i]==p) {
        owned-=sizes[i]; free(p); allocations[i]=NULL; return 0;
    }
    abort(); return 87;
}
APIRET16 Dos16MemAvail(PULONG n) { *n=64UL*1048576; return 0; }
APIRET DosQueryFSInfo(ULONG drive,ULONG level,PVOID out,ULONG size) {
    FSALLOCATE *d=out; (void)drive;(void)level;(void)size;
    d->cUnit=1048576;d->cSectorUnit=8;d->cbSector=512;d->cUnitAvail=524288;return 0;
}
APIRET DosQueryModuleHandle(const char *name,HMODULE *h) { (void)name; *h=1; return 0; }
APIRET DosQueryProcAddr(HMODULE h,ULONG ord,const char *name,PFN *p) {
    (void)h;(void)ord;(void)name; *p=(PFN)process_state; return 0;
}
