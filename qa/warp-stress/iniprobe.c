/* Read-only diagnostic of existing OS/2 profile entries; no PrfWrite calls. */
#define INCL_DOS
#define INCL_DOSERRORS
#define INCL_WIN
#include <os2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIMIT 65536UL
static HAB anchor;

static void quoted(const char *text) {
    const unsigned char *p=(const unsigned char *)text;
    putchar('"');
    while(*p) {
        if(*p=='"'||*p=='\\')putchar('\\');
        if(*p>=32&&*p<127)putchar(*p);else printf("\\x%02x",*p);
        p++;
    }
    putchar('"');
}

static void entry(HINI profile,const char *app,const char *key) {
    ULONG size=0,capacity,i,value;BOOL ok;unsigned char *data;
    ok=PrfQueryProfileSize(profile,app,key,&size);
    fputs("key=",stdout);quoted(key);
    printf(" size_ok=%u bytes=%lu pm_error=0x%08lx\n",(unsigned)ok,size,(unsigned long)WinGetLastError(anchor));
    if(!ok||!size)return;
    if(size>LIMIT) { printf("data skipped: above %lu-byte diagnostic limit\n",LIMIT);return; }
    data=malloc(size);if(!data) { fputs("allocation failed\n",stdout);return; }
    capacity=size;ok=PrfQueryProfileData(profile,app,key,data,&capacity);
    printf("data_ok=%u returned_bytes=%lu pm_error=0x%08lx\n",(unsigned)ok,capacity,(unsigned long)WinGetLastError(anchor));
    if(ok&&capacity<=size) {
        for(i=0;i<capacity;i++) {
            if(!(i&15))printf("hex[%04lx] ",(unsigned long)i);
            printf("%02x",data[i]);
            if((i&15)==15||i+1==capacity)putchar('\n');else putchar(' ');
        }
        for(i=0;i+4<=capacity;i+=4) {
            value=(ULONG)data[i]|((ULONG)data[i+1]<<8)|((ULONG)data[i+2]<<16)|((ULONG)data[i+3]<<24);
            printf("ulong_le[%lu]=%lu (0x%08lx)\n",i/4,value,value);
        }
        if(capacity&3)printf("trailing_bytes=%lu\n",capacity&3);
    }
    free(data);fflush(stdout);
}

static int inspect(const char *path,const char *app) {
    HINI profile;FILESTATUS3 info;APIRET rc;ULONG size=0,count,at,end;char *keys;BOOL ok;
    fputs("profile=",stdout);quoted(path);fputs(" application=",stdout);quoted(app);putchar('\n');fflush(stdout);
    /* PrfOpenProfile can create a nonexistent profile, so reject missing paths. */
    rc=DosQueryPathInfo((PSZ)path,FIL_STANDARD,&info,sizeof(info));
    printf("existing_file_rc=%lu file_bytes=%lu\n",(unsigned long)rc,rc?0UL:info.cbFile);
    if(rc)return 0;
    profile=PrfOpenProfile(anchor,path);
    printf("PrfOpenProfile=0x%08lx pm_error=0x%08lx\n",(unsigned long)profile,(unsigned long)WinGetLastError(anchor));fflush(stdout);
    if(!profile)return 0;
    entry(profile,app,"DEFAULTSYSTEMRESOLUTION");
    ok=PrfQueryProfileSize(profile,app,NULL,&size);
    printf("keys_size_ok=%u keys_bytes=%lu pm_error=0x%08lx\n",(unsigned)ok,size,(unsigned long)WinGetLastError(anchor));
    if(ok&&size&&size<LIMIT-2) {
        keys=calloc(1,size+2);
        if(keys) {
            count=PrfQueryProfileString(profile,app,NULL,"",keys,size+2);
            printf("keys_string_bytes=%lu pm_error=0x%08lx\n",count,(unsigned long)WinGetLastError(anchor));
            if(count>size+1)count=size+1;
            for(at=0;at<count&&keys[at];at=end+1) {
                for(end=at;end<count&&keys[end];end++);
                if(end==count) { fputs("truncated key list\n",stdout);break; }
                if(strcmp(keys+at,"DEFAULTSYSTEMRESOLUTION"))entry(profile,app,keys+at);
            }
            free(keys);
        }
    }
    ok=PrfCloseProfile(profile);
    printf("PrfCloseProfile=%u pm_error=0x%08lx\n",(unsigned)ok,(unsigned long)WinGetLastError(anchor));fflush(stdout);
    return ok!=0;
}

int main(int argc,char **argv) {
    PTIB tib;PPIB pib;APIRET rc;ULONG saved_type;int ok=1;const char *app="PM_DISPLAYDRIVERS";
    if(argc>4) { fputs("INIPROBE [USER_INI_PATH [SYSTEM_INI_PATH [APPLICATION]]]\n",stderr);return 2; }
    if(argc==4)app=argv[3];
    rc=DosGetInfoBlocks(&tib,&pib);
    printf("INIPROBE read-only; DosGetInfoBlocks=%lu\n",(unsigned long)rc);fflush(stdout);
    if(rc)return 1;
    saved_type=pib->pib_ultype;
    printf("original_process_type=%lu morph_to_PM=3\n",saved_type);fflush(stdout);
    pib->pib_ultype=3;
    anchor=WinInitialize(0);
    printf("WinInitialize HAB=0x%08lx pm_error=0x%08lx HMQ=not-created\n",(unsigned long)anchor,(unsigned long)WinGetLastError(anchor));fflush(stdout);
    if(!anchor) { pib->pib_ultype=saved_type;fputs("No PM anchor; start PM services and retry. No profiles opened.\n",stdout);return 1; }
    if(!inspect(argc>1?argv[1]:"C:\\OS2\\OS2.INI",app))ok=0;
    if(!inspect(argc>2?argv[2]:"C:\\OS2\\OS2SYS.INI",app))ok=0;
    printf("WinTerminate=%u\n",(unsigned)WinTerminate(anchor));pib->pib_ultype=saved_type;
    printf("process_type_restored=%lu result=%s\n",saved_type,ok?"OK":"API_FAILURE");return ok?0:1;
}
