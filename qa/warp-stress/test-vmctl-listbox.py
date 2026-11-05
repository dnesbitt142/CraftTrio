#!/usr/bin/env python3
"""Exercise actual listbox command functions against controlled PM API results."""
from pathlib import Path
import subprocess
import sys

suite = Path(__file__).resolve().parents[2]
build = Path(sys.argv[1] if len(sys.argv) > 1 else suite / "build/warp-stress").resolve()
build.mkdir(parents=True, exist_ok=True)
source = (suite / "qa/warp-stress/vmctl.c").read_text()
functions = source[source.index("static HWND list_control("):source.index("static void usage(")]
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
typedef uintptr_t HWND;
typedef uintptr_t MPARAM;
typedef intptr_t MRESULT;
typedef uint16_t USHORT;
typedef int16_t SHORT;
typedef long LONG;
typedef unsigned long ULONG;
typedef unsigned long PID;
typedef unsigned long TID;
typedef void *PVOID;
typedef unsigned long APIRET;
typedef struct Target { HWND frame,client;uintptr_t task;PID pid; } Target;
#define stricmp strcasecmp
#define HWND_DESKTOP 1
#define QWS_ID 0
#define TRUE 1
#define LIT_FIRST (-1)
#define PAG_READ 0x1
#define PAG_WRITE 0x2
#define PAG_COMMIT 0x10
#define OBJ_GIVEABLE 0x200
#define MPFROMSHORT(a) ((MPARAM)(USHORT)(a))
#define MPFROMLONG(a) ((MPARAM)(ULONG)(a))
#define MPFROM2SHORT(a,b) ((MPARAM)(USHORT)(a)|((MPARAM)(USHORT)(b)<<16))
#define MPFROMP(a) ((MPARAM)(uintptr_t)(a))
#define LM_QUERYITEMCOUNT 0x0160
#define LM_SELECTITEM 0x0164
#define LM_QUERYSELECTION 0x0165
#define LM_QUERYITEMTEXTLENGTH 0x0167
#define LM_QUERYITEMTEXT 0x0168
static HWND frame_child,client_child=20;
static PID process=42;
static USHORT control_id=600;
static const char *class_name="#7";
static LONG item_count=2,length_override=-99,selected=-1;
static int text_failure,bad_nul,bad_count,selection_failure,bad_selected,focus_failure,select_calls;
static int allocation_failure,grant_failure,free_failure,granted;
static unsigned allocations,frees,query_writes;
static void *shared;
static char text_data[2050]="Save";
static HWND WinWindowFromID(HWND parent,ULONG id) { (void)id;return parent==11?frame_child:client_child; }
static int WinQueryWindowProcess(HWND window,PID *pid,TID *tid) { (void)window;*pid=process;*tid=1;return 1; }
static USHORT WinQueryWindowUShort(HWND w,int ignored) { (void)w;(void)ignored;return control_id; }
static LONG WinQueryClassName(HWND w,LONG size,char *name) { (void)w;snprintf(name,(size_t)size,"%s",class_name);return (LONG)strlen(name); }
static int WinSetFocus(HWND desktop,HWND window) { assert(desktop==HWND_DESKTOP&&window==20);return !focus_failure; }
static void quoted(const char *text) { (void)text; }
static APIRET DosAllocSharedMem(PVOID *out,const char *name,ULONG size,ULONG flags) {
 assert(!name&&size==4096&&flags==(OBJ_GIVEABLE|PAG_READ|PAG_WRITE|PAG_COMMIT));
 if(allocation_failure)return 8;
 assert(!shared);shared=malloc((size_t)size);assert(shared);*out=shared;allocations++;return 0;
}
static APIRET DosGiveSharedMem(PVOID memory,PID pid,ULONG flags) {
 assert(memory==shared&&pid==process&&flags==(PAG_READ|PAG_WRITE));
 if(grant_failure)return 5;
 granted=1;return 0;
}
static APIRET DosFreeMem(PVOID memory) {
 assert(memory==shared);free(shared);shared=NULL;granted=0;frees++;return free_failure?5:0;
}
static MRESULT WinSendMsg(HWND window,ULONG message,MPARAM first,MPARAM second) {
 LONG index=(SHORT)(first&65535),length=(LONG)strlen(text_data);assert(window==20);
 switch(message) {
 case LM_QUERYITEMCOUNT:return item_count;
 case LM_QUERYITEMTEXTLENGTH:assert(index>=0&&index<item_count);return length_override==-99?length:length_override;
 case LM_QUERYITEMTEXT: {
  ULONG capacity=(first>>16)&65535;char *out=(char *)(uintptr_t)second;
  /* Reject the exact cross-process private-buffer defect seen in the VM. */
  assert(shared&&granted&&out==shared&&capacity<=4096);query_writes++;
  if(text_failure)return -3;
  assert(capacity>=(ULONG)length+1);memcpy(out,text_data,(size_t)length);
  if(!bad_nul)out[length]=0;
  return bad_count?length+1:length;
 }
 case LM_SELECTITEM:assert(second==TRUE);select_calls++;if(selection_failure)return 0;selected=index;return 1;
 case LM_QUERYSELECTION:return bad_selected?selected+1:selected;
 }
 assert(!"unexpected PM message");return 0;
}
'''
checks = r'''
int main(void) {
 Target t;char *buffer;LONG length;unsigned before;t.frame=11;t.client=12;t.pid=42;
 assert(list_control(t,600)==20);assert(!list_control(t,0));assert(!list_control(t,65536));
 frame_child=21;assert(!list_control(t,600));frame_child=0;
 process=43;assert(!list_control(t,600));process=42;
 control_id=601;assert(!list_control(t,600));control_id=600;
 class_name="#6";assert(!list_control(t,600));class_name="#7";
 item_count=-3;assert(list_count(20)==-1);item_count=7001;assert(list_count(20)==-1);
 item_count=7000;assert(list_count(20)==7000);item_count=2;
 buffer=list_buffer(20);assert(buffer&&granted);
 assert(list_text(20,0,buffer,&length)&&length==4&&!strcmp(buffer,"Save"));
 length_override=-3;assert(!list_text(20,0,buffer,&length));
 length_override=2049;assert(!list_text(20,0,buffer,&length));length_override=-99;
 text_failure=1;assert(!list_text(20,0,buffer,&length));text_failure=0;
 bad_nul=1;assert(!list_text(20,0,buffer,&length));bad_nul=0;
 bad_count=1;assert(!list_text(20,0,buffer,&length));bad_count=0;
 memset(text_data,'A',2048);text_data[2048]=0;assert(list_text(20,0,buffer,&length)&&length==2048);
 assert(list_free(buffer)&&!shared);strcpy(text_data,"Save");
 assert(!list_select(20,-1));assert(!list_select(20,2));assert(!select_calls);
 before=query_writes;allocation_failure=1;assert(!list_items(20));assert(!list_select(20,1));allocation_failure=0;
 grant_failure=1;assert(!list_items(20));assert(!list_select(20,1));grant_failure=0;
 assert(!shared&&query_writes==before);
 text_failure=1;assert(!list_items(20));assert(!list_select(20,1));assert(!shared);text_failure=0;
 selection_failure=1;assert(!list_select(20,1));selection_failure=0;
 bad_selected=1;assert(!list_select(20,1));bad_selected=0;
 focus_failure=1;assert(!list_select(20,1));focus_failure=0;
 assert(list_select(20,1)&&selected==1);assert(list_items(20));
 free_failure=1;assert(!list_items(20));free_failure=0;
 item_count=0;before=allocations;assert(list_items(20));assert(allocations==before);
 assert(!shared&&allocations==frees);
 puts("PASS actual VMCTL listbox lookup/bounds/errors, shared allocation+PID grant before EVERY text write, cleanup and selection");return 0;
}
'''
unit = build / "vmctl-listbox.c"
unit.write_text(program + functions + checks)
binary = build / "vmctl-listbox"
subprocess.run(["gcc", "-std=c99", "-Wall", "-Wextra", "-Werror", str(unit), "-o", str(binary)], check=True)
subprocess.run([str(binary)], check=True)
