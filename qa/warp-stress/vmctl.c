/* Test-only native OS/2 PM controller. Only bounded listbox commands send
 * synchronous messages; ordinary keyboard/text/close commands remain posted. */
#define INCL_DOS
#define INCL_WIN
#include <os2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

typedef struct Target { HWND frame,client;HSWITCH task;PID pid; } Target;
static HAB anchor;

static void quoted(const char *text) {
    const unsigned char *p=(const unsigned char *)text;putchar('"');
    while(*p) { if(*p=='"'||*p=='\\')putchar('\\');if(*p>=32)putchar(*p);else printf("\\x%02x",*p);p++; }
    putchar('"');
}
static void title(HWND window,char *text,size_t capacity) {
    text[0]=0;WinQueryWindowText(window,(LONG)capacity,text);text[capacity-1]=0;
}
static Target target(HWND frame,HSWITCH task,PID pid) {
    Target t;t.frame=frame;t.task=task;t.pid=pid;t.client=WinWindowFromID(frame,FID_CLIENT);
    if(!t.client)t.client=frame;return t;
}
static void report(Target t,const char *switch_title) {
    SWP frame,client;POINTL point;char text[256];TID tid;HWND focused=WinQueryFocus(HWND_DESKTOP);
    memset(&frame,0,sizeof(frame));memset(&client,0,sizeof(client));
    WinQueryWindowPos(t.frame,&frame);WinQueryWindowPos(t.client,&client);
    point.x=0;point.y=0;WinMapWindowPoints(t.client,HWND_DESKTOP,&point,1);
    if(!t.pid)WinQueryWindowProcess(t.frame,&t.pid,&tid);
    title(t.frame,text,sizeof(text));
    printf("pid=%lu frame=0x%08lx client=0x%08lx switch=0x%08lx frame_xywh=%ld,%ld,%ld,%ld client_xywh=%ld,%ld,%ld,%ld focus=0x%08lx title=",
      (unsigned long)t.pid,(unsigned long)t.frame,(unsigned long)t.client,(unsigned long)t.task,
      frame.x,frame.y,frame.cx,frame.cy,point.x,point.y,client.cx,client.cy,(unsigned long)focused);
    quoted(text);if(switch_title) { fputs(" switch_title=",stdout);quoted(switch_title); }putchar('\n');
}
static PSWBLOCK switches(void) {
    ULONG count=WinQuerySwitchList(anchor,NULL,0),bytes;PSWBLOCK list;
    if(count>4096) { fputs("too many switch entries\n",stderr);return NULL; }
    count+=32;bytes=sizeof(SWBLOCK)+count*sizeof(SWENTRY);
    list=calloc(1,bytes);if(!list)return NULL;
    WinQuerySwitchList(anchor,list,bytes);
    if(list->cswentry>count) { free(list);fputs("switch list grew beyond snapshot\n",stderr);return NULL; }
    return list;
}
static int find(const char *wanted,Target *found) {
    PSWBLOCK list=switches();ULONG i;unsigned matches=0;HWND w;char text[256];TID tid;PID pid;
    if(!list)return 0;
    for(i=0;i<list->cswentry;i++) {
        SWENTRY *entry=&list->aswentry[i];title(entry->swctl.hwnd,text,sizeof(text));
        if(!strcmp(wanted,text)||!strcmp(wanted,entry->swctl.szSwtitle)) {
            if(matches && found->frame==entry->swctl.hwnd)continue;
            *found=target(entry->swctl.hwnd,entry->hswitch,entry->swctl.idProcess);matches++;
        }
    }
    free(list);
    /* Dialogs need not have switch-list entries. Search exact desktop titles. */
    for(w=WinQueryWindow(HWND_DESKTOP,QW_TOP);w;w=WinQueryWindow(w,QW_NEXT)) {
        title(w,text,sizeof(text));if(strcmp(wanted,text))continue;
        if(matches && found->frame==w)continue;
        pid=0;WinQueryWindowProcess(w,&pid,&tid);*found=target(w,0,pid);matches++;
    }
    if(matches!=1) { fprintf(stderr,"exact title matched %u windows: %s\n",matches,wanted);return 0; }
    return 1;
}
static int post_key(HWND window,USHORT flags,USHORT ch,USHORT vk) {
    if(!WinPostMsg(window,WM_CHAR,MPFROMSH2CH(flags,1,0),MPFROM2SHORT(ch,vk)))return 0;
    return WinPostMsg(window,WM_CHAR,MPFROMSH2CH(flags|KC_KEYUP,1,0),MPFROM2SHORT(ch,vk))!=0;
}
static int key(HWND window,const char *description) {
    char value[128],*part,*base=NULL;USHORT flags=0,vk=0,ch=0;unsigned n;
    if(strlen(description)>=sizeof(value))return 0;
    strcpy(value,description);
    for(part=strtok(value,"-");part;part=strtok(NULL,"-")) {
        if(!stricmp(part,"ctrl"))flags|=KC_CTRL;
        else if(!stricmp(part,"shift"))flags|=KC_SHIFT;
        else if(!stricmp(part,"alt"))flags|=KC_ALT;
        else if(base)return 0;else base=part;
    }
    if(!base)return 0;
    if(strlen(base)==1) { ch=(unsigned char)base[0];flags|=KC_CHAR; }
    else if(!stricmp(base,"enter"))vk=VK_ENTER;
    else if(!stricmp(base,"tab"))vk=VK_TAB;
    else if(!stricmp(base,"esc")||!stricmp(base,"escape"))vk=VK_ESC;
    else if(!stricmp(base,"space")) { ch=' ';flags|=KC_CHAR; }
    else if(!stricmp(base,"backspace"))vk=VK_BACKSPACE;
    else if(!stricmp(base,"delete"))vk=VK_DELETE;
    else if(!stricmp(base,"insert"))vk=VK_INSERT;
    else if(!stricmp(base,"left"))vk=VK_LEFT;
    else if(!stricmp(base,"right"))vk=VK_RIGHT;
    else if(!stricmp(base,"up"))vk=VK_UP;
    else if(!stricmp(base,"down"))vk=VK_DOWN;
    else if(!stricmp(base,"home"))vk=VK_HOME;
    else if(!stricmp(base,"end"))vk=VK_END;
    else if(!stricmp(base,"pageup"))vk=VK_PAGEUP;
    else if(!stricmp(base,"pagedown"))vk=VK_PAGEDOWN;
    else if((base[0]=='f'||base[0]=='F') && (n=(unsigned)atoi(base+1))>=1 && n<=12) {
        char canonical[8];sprintf(canonical,"f%u",n);if(stricmp(base,canonical))return 0;vk=VK_F1+n-1;
    } else return 0;
    if(vk)flags|=KC_VIRTUALKEY;
    return post_key(window,flags,ch,vk);
}
static int text(HWND window,const char *value) {
    const unsigned char *p=(const unsigned char *)value;size_t n=strlen(value);
    if(n>4096) { fputs("text is limited to4096 codepage bytes\n",stderr);return 0; }
    while(*p) {
        int ok;if(*p=='\n'||*p=='\r')ok=post_key(window,KC_VIRTUALKEY,0,VK_ENTER);
        else if(*p=='\t')ok=post_key(window,KC_VIRTUALKEY,0,VK_TAB);
        else if(*p>=32)ok=post_key(window,KC_CHAR,*p,0);else return 0;
        if(!ok)return 0;
        p++;DosSleep(2);
    }
    return 1;
}
static int number(const char *value,LONG *result) {
    char *end;long n;errno=0;n=strtol(value,&end,10);if(errno||!*value||*end)return 0;*result=n;return 1;
}
static HWND focused(Target t) {
    HWND window=WinQueryFocus(HWND_DESKTOP);PID pid=0;TID tid;
    if(!window||!WinQueryWindowProcess(window,&pid,&tid)||pid!=t.pid) {
        fputs("focused control does not belong to selected process\n",stderr);return 0;
    }
    return window;
}
static void children(HWND parent,unsigned depth) {
    HWND w;char caption[256],class_name[128];PID pid;TID tid;SWP pos;
    if(depth>8)return;
    for(w=WinQueryWindow(parent,QW_TOP);w;w=WinQueryWindow(w,QW_NEXT)) {
        title(w,caption,sizeof(caption));class_name[0]=0;WinQueryClassName(w,sizeof(class_name),class_name);
        pid=0;WinQueryWindowProcess(w,&pid,&tid);memset(&pos,0,sizeof(pos));WinQueryWindowPos(w,&pos);
        printf("depth=%u hwnd=0x%08lx id=%lu pid=%lu xywh=%ld,%ld,%ld,%ld class=",depth,(unsigned long)w,(unsigned long)WinQueryWindowUShort(w,QWS_ID),(unsigned long)pid,pos.x,pos.y,pos.cx,pos.cy);
        quoted(class_name);fputs(" title=",stdout);quoted(caption);putchar('\n');children(w,depth+1);
    }
}
static HWND list_control(Target t,LONG id) {
    HWND frame_control,client_control,window;PID pid=0;TID tid;char name[64];
    if(id<1||id>65535) { fputs("control ID must be1..65535\n",stderr);return 0; }
    frame_control=WinWindowFromID(t.frame,(ULONG)id);
    client_control=t.frame==t.client?frame_control:WinWindowFromID(t.client,(ULONG)id);
    if(frame_control&&client_control&&frame_control!=client_control) {
        fputs("control ID is ambiguous between frame and client\n",stderr);return 0;
    }
    window=frame_control?frame_control:client_control;
    if(!window||!WinQueryWindowProcess(window,&pid,&tid)||pid!=t.pid||
       WinQueryWindowUShort(window,QWS_ID)!=(USHORT)id) {
        fputs("matching control not found in selected process\n",stderr);return 0;
    }
    memset(name,0,sizeof(name));
    if(!WinQueryClassName(window,sizeof(name),name)||
       (strcmp(name,"#7")&&stricmp(name,"LISTBOX"))) {
        fputs("selected control is not a native listbox\n",stderr);return 0;
    }
    return window;
}
static LONG list_count(HWND window) {
    LONG count=(LONG)WinSendMsg(window,LM_QUERYITEMCOUNT,0,0);
    if(count<0||count>7000) { fprintf(stderr,"listbox count outside0..7000: %ld\n",count);return -1; }
    return count;
}
static char *list_buffer(HWND window) {
    PVOID memory=NULL;PID pid=0;TID tid;APIRET rc;
    if(!WinQueryWindowProcess(window,&pid,&tid)||!pid)return NULL;
    /* A native control writes into this pointer in ITS process. Ordinary
     * malloc/stack memory is not automatically marshalled by WinSendMsg. */
    rc=DosAllocSharedMem(&memory,NULL,4096,OBJ_GIVEABLE|PAG_READ|PAG_WRITE|PAG_COMMIT);
    if(rc) { fprintf(stderr,"listbox shared allocation failed: %lu\n",(unsigned long)rc);return NULL; }
    rc=DosGiveSharedMem(memory,pid,PAG_READ|PAG_WRITE);
    if(rc) {
        fprintf(stderr,"listbox shared grant failed: %lu\n",(unsigned long)rc);
        DosFreeMem(memory);return NULL;
    }
    return (char *)memory;
}
static int list_free(char *buffer) {
    APIRET rc;
    /* The synchronous text query has returned, so the receiver has finished
     * writing. Release our reference. Its given mapping lasts until exit. */
    rc=DosFreeMem(buffer);
    if(rc) { fprintf(stderr,"listbox shared release failed: %lu\n",(unsigned long)rc);return 0; }
    return 1;
}
static int list_text(HWND window,LONG index,char *buffer,LONG *length) {
    LONG wanted=(SHORT)WinSendMsg(window,LM_QUERYITEMTEXTLENGTH,MPFROMSHORT((SHORT)index),0),got;
    if(wanted<0||wanted>2048) { fprintf(stderr,"item%ld text outside0..2048 bytes: %ld\n",index,wanted);return 0; }
    memset(buffer,0xff,(size_t)wanted+1);
    got=(LONG)WinSendMsg(window,LM_QUERYITEMTEXT,MPFROM2SHORT((SHORT)index,(USHORT)(wanted+1)),MPFROMP(buffer));
    if(got!=wanted||memchr(buffer,0,(size_t)wanted+1)!=buffer+wanted) {
        fprintf(stderr,"item%ld text query failed or changed (expected%ld got%ld)\n",index,wanted,got);return 0;
    }
    *length=wanted;return 1;
}
static int list_items(HWND window) {
    LONG count=list_count(window),i,length;char *buffer;int ok=1;
    if(count<0)return 0;
    printf("listbox=0x%08lx count=%ld\n",(unsigned long)window,count);
    if(!count)return 1;
    buffer=list_buffer(window);if(!buffer)return 0;
    for(i=0;i<count;i++) {
        if(!list_text(window,i,buffer,&length)) { ok=0;break; }
        printf("item=%ld bytes=%ld text=",i,length);quoted(buffer);putchar('\n');
    }
    if(!list_free(buffer))ok=0;
    return ok;
}
static int list_select(HWND window,LONG index) {
    LONG count=list_count(window),selected,length;char *buffer;int ok=0;
    if(count<0||index<0||index>=count) { fputs("selection index is outside the listbox\n",stderr);return 0; }
    buffer=list_buffer(window);if(!buffer)return 0;
    if(!list_text(window,index,buffer,&length))goto done;
    if(!WinSendMsg(window,LM_SELECTITEM,MPFROMSHORT((SHORT)index),MPFROMSHORT(TRUE))) {
        fputs("listbox selection failed\n",stderr);goto done;
    }
    selected=(LONG)WinSendMsg(window,LM_QUERYSELECTION,MPFROMLONG(LIT_FIRST),0);
    if(selected!=index) { fprintf(stderr,"selection verification failed: expected%ld got%ld\n",index,selected);goto done; }
    if(!WinSetFocus(HWND_DESKTOP,window)) { fputs("listbox focus failed\n",stderr);goto done; }
    printf("listbox=0x%08lx selected=%ld bytes=%ld text=",(unsigned long)window,index,length);quoted(buffer);putchar('\n');ok=1;
done:
    if(!list_free(buffer))ok=0;
    return ok;
}
static void usage(void) {
    fputs("VMCTL wait MILLISECONDS (0..60000)\nVMCTL list | windows\nVMCTL query|focus|children|close TITLE\nVMCTL resize TITLE X Y W H\nVMCTL move TITLE X Y\nVMCTL text|textfocus TITLE STRING\nVMCTL key|keyfocus TITLE ctrl-o|enter|tab|ctrl-shift-f12|...\nVMCTL focusid TITLE CONTROL_ID\nVMCTL items TITLE CONTROL_ID\nVMCTL select TITLE CONTROL_ID INDEX\nExact title required; coordinates use OS/2 bottom-left origin.\n",stderr);
}
int main(int argc,char **argv) {
    HMQ queue;int status=1,ok=0;Target selected;PSWBLOCK list;ULONG i,saved_type;HWND w;LONG x,y,width,height;PTIB tib;PPIB pib;
    if(argc<2) { usage();return 2; }
    if(!strcmp(argv[1],"wait")) {
        if(argc!=3||!number(argv[2],&x)||x<0||x>60000) { usage();return 2; }
        return DosSleep((ULONG)x)!=0;
    }
    if(DosGetInfoBlocks(&tib,&pib))return 1;
    saved_type=pib->pib_ultype;pib->pib_ultype=3;
    anchor=WinInitialize(0);if(!anchor) { fputs("WinInitialize failed\n",stderr);pib->pib_ultype=saved_type;return 1; }
    queue=WinCreateMsgQueue(anchor,0);if(!queue) { fprintf(stderr,"WinCreateMsgQueue failed:0x%08lx\n",(unsigned long)WinGetLastError(anchor));WinTerminate(anchor);pib->pib_ultype=saved_type;return 1; }
    if(!strcmp(argv[1],"shutdown")&&argc==2) {
        ok=WinShutdownSystem(anchor,queue)!=0;
    } else if(!strcmp(argv[1],"list")&&argc==2) {
        list=switches();if(list) { for(i=0;i<list->cswentry;i++)report(target(list->aswentry[i].swctl.hwnd,list->aswentry[i].hswitch,list->aswentry[i].swctl.idProcess),list->aswentry[i].swctl.szSwtitle);free(list);ok=1; }
    } else if(!strcmp(argv[1],"windows")&&argc==2) {
        for(w=WinQueryWindow(HWND_DESKTOP,QW_TOP);w;w=WinQueryWindow(w,QW_NEXT))report(target(w,0,0),NULL);ok=1;
    } else if(argc>=3 && find(argv[2],&selected)) {
        if(!strcmp(argv[1],"query")&&argc==3) { report(selected,NULL);ok=1; }
        else if(!strcmp(argv[1],"children")&&argc==3) { children(selected.frame,0);ok=1; }
        else if(!strcmp(argv[1],"focus")&&argc==3) {
            if(selected.task)WinSwitchToProgram(selected.task);
            ok=WinSetWindowPos(selected.frame,HWND_TOP,0,0,0,0,SWP_RESTORE|SWP_SHOW|SWP_ACTIVATE|SWP_ZORDER)!=0;
            if(ok)ok=WinSetFocus(HWND_DESKTOP,selected.client)!=0;report(selected,NULL);
        } else if(!strcmp(argv[1],"resize")&&argc==7 && number(argv[3],&x)&&number(argv[4],&y)&&number(argv[5],&width)&&number(argv[6],&height)&&width>0&&height>0) {
            ok=WinSetWindowPos(selected.frame,HWND_TOP,x,y,width,height,SWP_RESTORE|SWP_MOVE|SWP_SIZE|SWP_SHOW)!=0;report(selected,NULL);
        } else if(!strcmp(argv[1],"move")&&argc==5 && number(argv[3],&x)&&number(argv[4],&y)) {
            ok=WinSetWindowPos(selected.frame,HWND_TOP,x,y,0,0,SWP_RESTORE|SWP_MOVE|SWP_SHOW)!=0;report(selected,NULL);
        } else if(!strcmp(argv[1],"close")&&argc==3)ok=WinPostMsg(selected.frame,WM_CLOSE,0,0)!=0;
        else if(!strcmp(argv[1],"text")&&argc==4)ok=text(selected.client,argv[3]);
        else if(!strcmp(argv[1],"key")&&argc==4)ok=key(selected.client,argv[3]);
        else if(!strcmp(argv[1],"textfocus")&&argc==4) { w=focused(selected);if(w)ok=text(w,argv[3]); }
        else if(!strcmp(argv[1],"keyfocus")&&argc==4) { w=focused(selected);if(w)ok=key(w,argv[3]); }
        else if(!strcmp(argv[1],"focusid")&&argc==4 && number(argv[3],&x)&&x>=0) {
            w=WinWindowFromID(selected.frame,(ULONG)x);if(!w)w=WinWindowFromID(selected.client,(ULONG)x);
            if(w)ok=WinSetFocus(HWND_DESKTOP,w)!=0;
        } else if(!strcmp(argv[1],"items")&&argc==4 && number(argv[3],&x)&&x>=1&&x<=65535) {
            w=list_control(selected,x);if(w)ok=list_items(w);
        } else if(!strcmp(argv[1],"select")&&argc==5 && number(argv[3],&x)&&x>=1&&x<=65535&&number(argv[4],&y)&&y>=0&&y<7000) {
            w=list_control(selected,x);if(w)ok=list_select(w,y);
        } else usage();
    } else if(argc<3)usage();
    if(ok)status=0;else fprintf(stderr,"VMCTL failed; PM error=0x%08lx\n",(unsigned long)WinGetLastError(anchor));
    WinDestroyMsgQueue(queue);WinTerminate(anchor);pib->pib_ultype=saved_type;return status;
}
