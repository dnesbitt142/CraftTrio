#ifndef WC_TEST_OS2_H
#define WC_TEST_OS2_H
#include <stdint.h>
typedef uint32_t ULONG;
typedef uint32_t APIRET;
typedef void *PVOID;
#define PAG_READ 1U
#define PAG_WRITE 2U
#define PAG_GUARD 8U
#define PAG_COMMIT 16U
#define PAG_DECOMMIT 32U
#define PAG_DEFAULT 1024U
#define PAG_FREE 16384U
#define OBJ_ANY 1024U
#define ERROR_INVALID_PARAMETER 87U
APIRET DosAllocMem(PVOID *, ULONG, ULONG);
APIRET DosSetMem(PVOID, ULONG, ULONG);
APIRET DosFreeMem(PVOID);
APIRET DosQueryMem(PVOID, ULONG *, ULONG *);
#endif
