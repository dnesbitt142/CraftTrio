#include "serial-os2.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
static int opened,closed,configured;
static int reserved_handle = -1;
static FILE *held[4];
static char held_paths[4][32];
static unsigned held_count;
static ULONG ticks;
int __lsan_is_turned_off(void) { return 1; }
static void finish(void) {
    unsigned i;
    assert(!opened || closed); assert(reserved_handle < 0);
    for (i=0;i<held_count;++i) fclose(held[i]);
}
APIRET DosOpen(const char *name,HFILE *port,PULONG action,ULONG size,ULONG attr,ULONG create,ULONG flags,PVOID ea) {
    (void)action;(void)size;(void)attr;(void)create;(void)ea;
    if (strcmp(name,"COM1")) {
        int fd;
        assert(create==(OPEN_ACTION_CREATE_IF_NEW|OPEN_ACTION_FAIL_IF_EXISTS));
        assert(flags==(OPEN_ACCESS_WRITEONLY|OPEN_SHARE_DENYREADWRITE|OPEN_FLAGS_NOINHERIT));
        assert(reserved_handle<0);
        fd=open(name,O_WRONLY|O_CREAT|O_EXCL,0600);
        if(fd<0) return errno==EEXIST ? ERROR_FILE_EXISTS : 5;
        *port=(ULONG)fd;reserved_handle=fd;return 0;
    }
    assert((flags&(OPEN_ACCESS_READWRITE|OPEN_SHARE_DENYNONE|OPEN_FLAGS_NOINHERIT))==
           (OPEN_ACCESS_READWRITE|OPEN_SHARE_DENYNONE|OPEN_FLAGS_NOINHERIT));
    *port=99;opened=1;atexit(finish);return 0;
}
APIRET DosDevIOCtl(HFILE port,ULONG category,ULONG function,PVOID params,ULONG size,PULONG used,PVOID data,ULONG cap,PULONG got) {
    assert(port==99 && category==1);
    if (getenv("SERIAL_TEST_CONFIG_FAIL")) return 87;
    if (function==ASYNC_SETBAUDRATE) assert(size==2 && *(USHORT*)params==38400);
    if (function==ASYNC_EXTSETBAUDRATE) {
        unsigned char *p=params;
        assert(size==5 && p[0]==0 && p[1]==0xc2 && p[2]==1 && p[3]==0 && p[4]==0);
    }
    if (function==ASYNC_SETLINECTRL) {
        unsigned char *p=params;assert(size==3 && p[0]==8 && !p[1] && !p[2]);
    }
    if (function==ASYNC_GETDCBINFO) { assert(cap==11);memset(data,0,11);*got=11; }
    if (function==ASYNC_SETDCBINFO) {
        DCBINFO *d=params;
        assert(size==11 && d->usReadTimeout==99 && d->usWriteTimeout==999);
        assert(d->fbCtlHndShake==MODE_DTR_CONTROL && d->fbFlowReplace==MODE_RTS_CONTROL);
        assert(d->fbTimeout==MODE_WAIT_READ_TIMEOUT);configured=1;
    }
    if (used) *used=size;
    return 0;
}
APIRET DosRead(HFILE port,PVOID out,ULONG cap,PULONG got) {
    ssize_t n;
    assert(port==99 && configured);
    n=read(0,out,cap>7?7:(size_t)cap);
    if(n<=0) return 6;
    *got=(ULONG)n;return 0;
}
APIRET DosWrite(HFILE port,PVOID data,ULONG size,PULONG wrote) {
    ssize_t n;
    assert(port==99 && configured);
    if(getenv("SERIAL_TEST_WRITE_FAIL")) return 6;
    n=write(1,data,size>5?5:(size_t)size);
    if(n<0) return 6;
    *wrote=(ULONG)n;return 0;
}
APIRET DosClose(HFILE port) {
    if(port!=99) { assert((int)port==reserved_handle);close(reserved_handle);reserved_handle=-1;return 0; }
    assert(!closed);closed=1;return 0;
}
APIRET DosSleep(ULONG ms) { ticks+=ms;return 0; }
APIRET DosQuerySysInfo(ULONG first,ULONG last,PVOID out,ULONG size) {
    assert(first==14 && last==14 && size==sizeof(ULONG));*(PULONG)out=ticks++;return 0;
}
int seragent_test_system(const char *shell) {
    FILE *f;
    char output[32];
    const char *path=strstr(shell,") >C:\\CRAFT\\A");
    const char *suffix;
    unsigned i;
    size_t size=strlen(shell);
    assert(shell[0]=='(' && size>30);
    assert(path && reserved_handle<0);
    path+=3;suffix=strstr(path," 2>&1");assert(suffix && (size_t)(suffix-path)<sizeof(output));
    memcpy(output,path,(size_t)(suffix-path));output[suffix-path]=0;
    for(i=0;i<held_count;++i) assert(strcmp(output,held_paths[i]));
    f=fopen(output,"wb");assert(f);
    fputs("executed stdout\nexecuted stderr\n",f);fflush(f);
    if(strstr(shell,"start ")) {
        assert(held_count<4);strcpy(held_paths[held_count],output);held[held_count++]=f;
    } else fclose(f);
    return strstr(shell,"fail_owned_command")?7:0;
}
APIRET DosQueryPathInfo(const char *path,ULONG level,PVOID data,ULONG size) {
    struct stat st;(void)level;(void)size;
    if(stat(path,&st)) return 2;
    ((FILESTATUS3*)data)->cbFile=(ULONG)st.st_size;return 0;
}
