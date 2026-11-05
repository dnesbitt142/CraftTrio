#ifndef WARP_TEST_OS2_H
#define WARP_TEST_OS2_H
#include <stdint.h>
/* Host ABI uses C unsigned long for portable printf tests, not a Warp ABI emulator. */
typedef unsigned long ULONG, APIRET, PID, TID, HMODULE;
typedef uint16_t USHORT, APIRET16;
typedef void *PVOID;
typedef void **PPVOID;
typedef ULONG *PULONG;
typedef PID *PPID;
typedef ULONG HFILE;
typedef char *PSZ;
typedef void (*PFN)(void);
#define APIENTRY
#define QSV_MS_COUNT 14
#define QSV_PAGE_SIZE 10
#define QSV_TOTPHYSMEM 17
#define QSV_TOTRESMEM 18
#define QSV_TOTAVAILMEM 19
#define QSV_MAXSHMEM 21
#define FIL_STANDARD 1
#define FSIL_ALLOC 1
#define PAG_READ 1
#define PAG_WRITE 2
#define PAG_COMMIT 16
#define PRTYS_PROCESS 0
#define PRTYC_IDLETIME 1
#define ERROR_NOT_ENOUGH_MEMORY 8
#define ERROR_INVALID_FUNCTION 1
#define ERROR_INVALID_DATA 13
#define ERROR_INVALID_PARAMETER 87
#define ERROR_BUFFER_OVERFLOW 111
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_SMG_START_IN_BACKGROUND 457
#define FILE_DIRECTORY 16
#define FILE_NORMAL 0
#define OPEN_ACTION_FAIL_IF_NEW 0
#define OPEN_ACTION_OPEN_IF_EXISTS 1
#define OPEN_ACCESS_READONLY 0
#define OPEN_SHARE_DENYNONE 64
#define OPEN_FLAGS_NOINHERIT 128
#define SSF_RELATED_CHILD 1
#define SSF_FGBG_BACK 1
#define SSF_INHERTOPT_PARENT 1
#define SSF_TYPE_PM 3
#define SSF_CONTROL_VISIBLE 0
#define QS_PROCESS 1
#define QS_MTE 4
typedef struct { ULONG cbFile, attrFile; } FILESTATUS3;
typedef struct {
 USHORT Length,Related,FgBg; PSZ PgmTitle,PgmName;
 USHORT InheritOpt,SessionType,PgmControl; PSZ ObjectBuffer; ULONG ObjectBuffLen;
} STARTDATA;
typedef struct { ULONG cUnit, cSectorUnit, cbSector, cUnitAvail; } FSALLOCATE;
typedef struct { USHORT year; unsigned char month,day,hours,minutes,seconds,hundredths; } DATETIME;
typedef struct { ULONG systime, usertime; } QSTREC;
typedef struct {
 ULONG RecType; QSTREC *pThrdRec; USHORT pid,ppid; ULONG type,stat,sgid;
 USHORT hMte,cTCB;
} QSPREC;
typedef struct QSLREC { void *pNextRec; USHORT hmte; unsigned char *pName; } QSLREC;
typedef struct { void *unused; QSPREC *pProcRec; QSLREC *pLibRec; } QSPTRREC;
APIRET DosQuerySysInfo(ULONG,ULONG,PVOID,ULONG);
APIRET DosQueryPathInfo(const char *,ULONG,PVOID,ULONG);
APIRET DosSleep(ULONG);
APIRET DosGetDateTime(DATETIME *);
APIRET DosSetPriority(ULONG,ULONG,int,ULONG);
APIRET DosAllocMem(PPVOID,ULONG,ULONG);
APIRET DosFreeMem(PVOID);
APIRET16 Dos16MemAvail(PULONG);
APIRET DosQueryFSInfo(ULONG,ULONG,PVOID,ULONG);
APIRET DosQueryModuleHandle(const char *,HMODULE *);
APIRET DosQueryProcAddr(HMODULE,ULONG,const char *,PFN *);
APIRET DosStartSession(STARTDATA *,PULONG,PPID);
APIRET DosSetDefaultDisk(ULONG);
APIRET DosSetCurrentDir(const char *);
APIRET DosOpen(const char *,HFILE *,PULONG,ULONG,ULONG,ULONG,ULONG,PVOID);
APIRET DosRead(HFILE,PVOID,ULONG,PULONG);
APIRET DosClose(HFILE);
APIRET DosQueryFHState(HFILE,PULONG);
APIRET DosSetFHState(HFILE,ULONG);
#endif
