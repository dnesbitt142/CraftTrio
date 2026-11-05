/* Test-only first-thread exception observer; never resumes or repairs faults. */
#define INCL_DOS
#define INCL_DOSERRORS
#include <os2.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "craftpm.h"

extern int craft_app_main(void);
static HFILE log_handle;
static int in_handler;
static char line[1024];

static void note(const char *format, ...) {
    va_list args;
    int length;
    ULONG written, offset = 0;
    va_start(args,format);length=vsnprintf(line,sizeof(line),format,args);va_end(args);
    if(length<0)return;
    if(length>=(int)sizeof(line))length=sizeof(line)-1;
    while(offset<(ULONG)length) {
        written=0;
        if(DosWrite(log_handle,line+offset,(ULONG)length-offset,&written)||!written)break;
        offset+=written;
    }
}
static int readable(ULONG address,ULONG length) {
    ULONG extent=length,flags=0;
    return length && address<=0xffffffffUL-length &&
        !DosQueryMem((PVOID)address,&extent,&flags) && extent>=length &&
        (flags&(PAG_READ|PAG_COMMIT|PAG_GUARD))==(PAG_READ|PAG_COMMIT);
}
static void bytes(ULONG address) {
    ULONG start=address>=16?address-16:address,extent=48,flags=0,i;
    unsigned char saved[48];
    APIRET rc=DosQueryMem((PVOID)start,&extent,&flags);
    if(rc || !extent || (flags&(PAG_READ|PAG_COMMIT|PAG_GUARD))!=(PAG_READ|PAG_COMMIT)) {
        note("bytes address=%08lx unavailable query_rc=%lu flags=%08lx\r\n",address,(ULONG)rc,flags);return;
    }
    if(extent>sizeof(saved))extent=sizeof(saved);
    if(start>0xffffffffUL-extent)return;
    memcpy(saved,(PVOID)start,extent);
    note("bytes start=%08lx count=%lu hex=",start,extent);
    for(i=0;i<extent;++i)note("%02x",saved[i]);
    note("\r\n");
}
static int module(ULONG address,const char *source) {
    HMODULE handle=0;
    ULONG object=0,offset=0;
    char name[260];
    APIRET rc;
    name[0]=0;
    rc=DosQueryModFromEIP(&handle,&object,sizeof(name),name,&offset,address);
    name[sizeof(name)-1]=0;
    note("module source=%s address=%08lx rc=%lu handle=%08lx object=%lu offset=%08lx name=%s\r\n",
         source,address,(ULONG)rc,(ULONG)handle,object,offset,name);
    return rc==0;
}
static ULONG APIENTRY observe(PEXCEPTIONREPORTRECORD report,
    PEXCEPTIONREGISTRATIONRECORD registration,PCONTEXTRECORD context,PVOID reserved) {
    ULONG i,value,stack,flags,extent;
    size_t size=0;
    unsigned char *memory;
    (void)registration;(void)reserved;
    if(in_handler || (report->fHandlerFlags&(EH_UNWINDING|EH_EXIT_UNWIND)) ||
       (report->ExceptionNum&0xc0000000UL)!=0xc0000000UL) return XCPT_CONTINUE_SEARCH;
    in_handler=1;
    note("FAULT exception=%08lx handler_flags=%08lx address=%08lx context_flags=%08lx\r\n",
         report->ExceptionNum,report->fHandlerFlags,(ULONG)report->ExceptionAddress,context->ContextFlags);
    for(i=0;i<report->cParameters && i<EXCEPTION_MAXIMUM_PARAMETERS;++i)
        note("parameter[%lu]=%08lx\r\n",i,report->ExceptionInfo[i]);
    note("EIP=%08lx ESP=%08lx EBP=%08lx EFLAGS=%08lx CS=%08lx SS=%08lx\r\n",
         context->ctx_RegEip,context->ctx_RegEsp,context->ctx_RegEbp,context->ctx_EFlags,context->ctx_SegCs,context->ctx_SegSs);
    note("EAX=%08lx EBX=%08lx ECX=%08lx EDX=%08lx ESI=%08lx EDI=%08lx DS=%08lx ES=%08lx FS=%08lx GS=%08lx\r\n",
         context->ctx_RegEax,context->ctx_RegEbx,context->ctx_RegEcx,context->ctx_RegEdx,
         context->ctx_RegEsi,context->ctx_RegEdi,context->ctx_SegDs,context->ctx_SegEs,context->ctx_SegFs,context->ctx_SegGs);
    DosResetBuffer(log_handle);
    module(context->ctx_RegEip,"EIP");bytes(context->ctx_RegEip);
    stack=context->ctx_RegEsp;
    for(i=0;i<256;++i) {
        if(!readable(stack,4)) { note("stack_end index=%lu address=%08lx unreadable\r\n",i,stack);break; }
        memcpy(&value,(PVOID)stack,4);
        note("stack[%03lu] address=%08lx value=%08lx\r\n",i,stack,value);
        if(module(value,"stack_candidate"))bytes(value);
        if(stack>0xfffffffbUL)break;
        stack+=4;
    }
    /* Preserve the stack before calling an optional engine getter: that getter
     * might itself be on the failing code path. */
    DosResetBuffer(log_handle);
    memory=wc_reactor_memory(&size);
    note("linear_memory base=%08lx live_size=%lu fault_within_live=%u\r\n",(ULONG)memory,(ULONG)size,
         memory && context->ctx_RegEip>=(ULONG)memory && context->ctx_RegEip-(ULONG)memory<size);
    if(memory) {
        extent=4096;flags=0;
        value=DosQueryMem((PVOID)(context->ctx_RegEip&~4095UL),&extent,&flags);
        note("EIP_mapping rc=%lu extent=%lu flags=%08lx\r\n",value,extent,flags);
    }
    note("END_FAULT returning_CONTINUE_SEARCH\r\n");
    DosResetBuffer(log_handle);
    in_handler=0;
    return XCPT_CONTINUE_SEARCH;
}
int main(void) {
    EXCEPTIONREGISTRATIONRECORD registration;
    ULONG action;
    APIRET rc;
    int result;
    rc=DosOpen("GRIDFAULT.LOG",&log_handle,&action,0,FILE_NORMAL,
        OPEN_ACTION_CREATE_IF_NEW|OPEN_ACTION_REPLACE_IF_EXISTS,
        OPEN_ACCESS_WRITEONLY|OPEN_SHARE_DENYNONE|OPEN_FLAGS_NOINHERIT|OPEN_FLAGS_FAIL_ON_ERROR,NULL);
    if(rc)return 2;
    note("GRIDFAULT test-only exception observer; no recovery; stack_limit=256\r\n");
    module((ULONG)main,"diagnostic_main");module((ULONG)craft_app_main,"application_main");
    module((ULONG)wc_reactor_call,"reactor_call");
    memset(&registration,0,sizeof(registration));
    registration.prev_structure=END_OF_CHAIN;
    registration.ExceptionHandler=observe;
    rc=DosSetExceptionHandler(&registration);
    note("DosSetExceptionHandler rc=%lu\r\n",(ULONG)rc);DosResetBuffer(log_handle);
    if(rc) { DosClose(log_handle);return 2; }
    result=craft_app_main();
    rc=DosUnsetExceptionHandler(&registration);
    note("normal_return result=%d unset_handler_rc=%lu\r\n",result,(ULONG)rc);
    DosClose(log_handle);return result;
}
