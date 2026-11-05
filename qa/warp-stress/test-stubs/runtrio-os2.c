#include "os2.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
static ULONG milliseconds;
static unsigned launches, writes[3];
static FILE *files[3];
static int directory_set;
static int csv_noinherit;
static const char *logs[3]={"WCGUI.LOG","DCGUI.LOG","GCGUI.LOG"};
int __lsan_is_turned_off(void) { return 1; }
static const char *mode(void) { const char *m=getenv("RUNTRIO_TEST_MODE"); return m?m:"ready"; }
static const char *base(const char *path) {
    const char *p=path,*result=path;
    while (*p) { if (*p=='\\' || *p=='/') result=p+1; ++p; }
    return result;
}
static void finish(void) {
    unsigned i;
    for (i=0;i<3;++i) assert(!files[i]);
    fprintf(stderr,"TEST launched sessions: %u\n",launches);
}
APIRET DosQuerySysInfo(ULONG first,ULONG last,PVOID out,ULONG bytes) {
    static int registered;
    if (!registered) { atexit(finish); registered=1; }
    assert(first==QSV_MS_COUNT && last==QSV_MS_COUNT && bytes==sizeof(ULONG));
    if (!strcmp(mode(),"clock")) return 87;
    *(PULONG)out=milliseconds++; return 0;
}
APIRET DosQueryPathInfo(const char *path,ULONG level,PVOID out,ULONG bytes) {
    FILESTATUS3 *st=out;
    struct stat s;
    const char *name=base(path);
    (void)level;(void)bytes;
    memset(st,0,sizeof(*st));
    if (strstr(name,".EXE")) return 0;
    if (stat(name,&s)) return ERROR_FILE_NOT_FOUND;
    st->cbFile=(ULONG)s.st_size;return 0;
}
APIRET DosSetDefaultDisk(ULONG disk) { assert(disk==3); return 0; }
APIRET DosSetCurrentDir(const char *path) { assert(!strcmp(path,"C:\\APP")); directory_set=1; return 0; }
APIRET DosStartSession(STARTDATA *s,PULONG session,PPID pid) {
    unsigned app=launches++;
    assert(directory_set && csv_noinherit && app<3 && milliseconds<100);
    assert(s->Related==SSF_RELATED_CHILD && s->SessionType==SSF_TYPE_PM);
    assert(s->FgBg==SSF_FGBG_BACK && s->InheritOpt==SSF_INHERTOPT_PARENT);
    *session=20+app;*pid=100+app;
    if (!strcmp(mode(),"launchfail") && app==1) {
        strcpy(s->ObjectBuffer,"missing CRAFTPM.DLL"); return 2;
    }
    return app==1 ? ERROR_SMG_START_IN_BACKGROUND : 0;
}
APIRET DosSleep(ULONG delay) {
    unsigned i;
    assert(delay<=250);
    milliseconds+=delay;
    for (i=0;i<3;++i) {
        FILE *f;
        if ((!strcmp(mode(),"timeout") && i==2) || (!strcmp(mode(),"launchfail") && i==1)) continue;
        if (!writes[i]) {
            f=fopen(logs[i],"wb");assert(f);fputs("Starting linked engine\nStartup frame: ",f);fclose(f);writes[i]=1;
        }
        if (writes[i]==1 && milliseconds>=500+i*250) {
            f=fopen(logs[i],"ab");assert(f);
            if (!strcmp(mode(),"logfail") && i==1) fputs("1, first\nCould not initialize the application engine: test\n",f);
            else fputs("3, 1280 x 720, 8192 bytes\n",f);
            fclose(f);writes[i]=2;
        }
    }
    return 0;
}
APIRET DosOpen(const char *path,HFILE *handle,PULONG action,ULONG size,ULONG attrs,ULONG create,ULONG access,PVOID ea) {
    unsigned i;
    (void)action;(void)size;(void)attrs;(void)create;(void)access;(void)ea;
    for (i=0;i<3;++i) if (!strcmp(base(path),logs[i])) {
        files[i]=fopen(logs[i],"rb");if(!files[i]) return 2;*handle=i;return 0;
    }
    abort();return 87;
}
APIRET DosRead(HFILE handle,PVOID out,ULONG bytes,PULONG got) {
    assert(handle<3 && files[handle]);clearerr(files[handle]);
    *got=fread(out,1,bytes,files[handle]);return 0;
}
APIRET DosClose(HFILE handle) {
    assert(handle<3 && files[handle]);fclose(files[handle]);files[handle]=NULL;return 0;
}
APIRET DosQueryFHState(HFILE handle,PULONG flags) { (void)handle;*flags=0x21;return 0; }
APIRET DosSetFHState(HFILE handle,ULONG flags) {
    (void)handle;
    /* Warp rejects access/share bits returned by QueryFHState when passed to SetFHState. */
    if(flags!=OPEN_FLAGS_NOINHERIT || !strcmp(mode(),"setfhfail")) return 87;
    csv_noinherit=1;return 0;
}
