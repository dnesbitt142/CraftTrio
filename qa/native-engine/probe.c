/* Test-only Linux ABI host for the exact GCC i386/OMF engine objects.
 * This does not execute Presentation Manager or the OS/2 kernel. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "craftpm.h"
#include "raster.h"

static uint32_t seed=0x12345678U, random_calls, save_calls;
static uint8_t *span(uint32_t offset,uint32_t length) {
    size_t size; uint8_t *memory=wc_reactor_memory(&size);
    if(!memory || offset>size || length>size-offset) { fputs("invalid host memory span\n",stderr); exit(2); }
    return memory+offset;
}
static void store32(uint32_t offset,uint32_t value) { memcpy(span(offset,4),&value,4); }
static uint32_t empty(uint32_t count,uint32_t size) { store32(count,0);store32(size,0);return 0; }
static uint32_t random_get(uint32_t offset,uint32_t length) {
    uint8_t *p=span(offset,length); uint32_t i; random_calls++;
    /* Deliberately deterministic, test-only, never suitable for production. */
    for(i=0;i<length;i++) { seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;p[i]=(uint8_t)seed; }
    return 0;
}
static uint32_t clock_time(uint32_t id,uint32_t offset) {
    uint64_t value=id?UINT64_C(1000000000):UINT64_C(1700000000000000000);
    memcpy(span(offset,8),&value,8);return 0;
}
static uint32_t clock_res(uint32_t id,uint32_t offset) {
    uint64_t value=1000000; (void)id;memcpy(span(offset,8),&value,8);return 0;
}
static uint32_t fd_write(uint32_t fd,uint32_t vectors,uint32_t count,uint32_t output) {
    uint32_t i,total=0; (void)fd;
    for(i=0;i<count;i++) { uint32_t pair[2];memcpy(pair,span(vectors+i*8,8),8);fwrite(span(pair[0],pair[1]),1,pair[1],stderr);total+=pair[1]; }
    store32(output,total);return 0;
}
static uint32_t fd_read(uint32_t fd,uint32_t output) { (void)fd;store32(output,0);return 0; }
static uint32_t fdstat(uint32_t fd,uint32_t output) { uint8_t *p=span(output,24);(void)fd;memset(p,0,24);p[0]=2;return 0; }
static uint32_t badfd(uint32_t fd) { (void)fd;return 8; }
static uint32_t yield(void) { return 0; }
static int32_t save(uint32_t name,uint32_t namesize,uint32_t data,uint32_t length,uint32_t as,uint32_t output,uint32_t capacity) {
    (void)name;(void)namesize;(void)as;(void)output;(void)capacity;
    (void)span(data,length); save_calls++;return 0;
}
static int32_t net(uint32_t op,uint32_t address,uint32_t length) { (void)op;(void)address;(void)length;return -1; }
static void url(uint32_t address,uint32_t length) { (void)address;(void)length; }
static int32_t queues(uint8_t *p,uint32_t size) { (void)p;(void)size;return -1; }
static int32_t print_start(const char *p,uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e) { (void)p;(void)a;(void)b;(void)c;(void)d;(void)e;return -1; }
static int32_t print_page(const uint8_t *p,uint32_t a,uint32_t b,uint32_t c,uint32_t d) { (void)p;(void)a;(void)b;(void)c;(void)d;return -1; }
static int32_t print_finish(int cancel) { (void)cancel;return -1; }
static uint32_t clipboard_read(uint32_t a,uint32_t b,uint32_t c) { (void)a;(void)b;(void)c;return 0; }
static uint32_t clipboard_write(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e,uint32_t f) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;return 0; }
static const WcPmApi api={NULL,empty,random_get,clock_time,clock_res,fd_write,fd_read,fdstat,badfd,badfd,yield,save,net,url,net,queues,print_start,print_page,print_finish,clipboard_read,clipboard_write,clipboard_read};
const WcPmApi *wc_pm=&api;

/* Same x87 mode used at every PM engine entry. */
static void float_mode(void);
#pragma aux float_mode = "sub esp,4" "fnclex" "fnstcw word ptr [esp]" \
    "and word ptr [esp],0f0ffh" "or word ptr [esp],023fh" \
    "fldcw word ptr [esp]" "add esp,4" modify exact [];
static uint32_t call(WcOperation op,const int32_t *args) {
    const char *error; uint32_t result=0;float_mode();error=wc_reactor_call(op,args,&result);
    if(error) { fprintf(stderr,"FAIL operation %u: %s\n",(unsigned)op,error);exit(1); }
    return result;
}
static void event(int a,int b,int c,int d,int e) { int32_t args[5];args[0]=a;args[1]=b;args[2]=c;args[3]=d;args[4]=e;if(call(WCR_EVENT,args))exit(1); }
static uint32_t hash(const uint8_t *p,size_t size) { uint32_t h=2166136261U;while(size--)h=(h^*p++)*16777619U;return h; }
static void dump(const char *path,const uint8_t *p,size_t size) {
    FILE *f=fopen(path,"wb");if(!f || fwrite(p,1,size,f)!=size || fclose(f)) { fprintf(stderr,"cannot write %s\n",path);exit(2); }
}
static void ppm(const char *path,const uint8_t *pixels,uint32_t w,uint32_t h,size_t stride) {
    FILE *f=fopen(path,"wb");uint32_t x,y;uint8_t rgb[3];if(!f)exit(2);
    fprintf(f,"P6\n%lu %lu\n255\n",(unsigned long)w,(unsigned long)h);
    for(y=0;y<h;y++)for(x=0;x<w;x++) { const uint8_t *p=pixels+(h-1-y)*stride+x*3;rgb[0]=p[2];rgb[1]=p[1];rgb[2]=p[0];if(fwrite(rgb,1,3,f)!=3)exit(2); }
    if(fclose(f))exit(2);
}
static void input(const char *path) {
    FILE *f=fopen(path,"rb"); long length;uint32_t offset;int32_t args[2];size_t namesize=strlen(path);
    if(!f || fseek(f,0,SEEK_END) || (length=ftell(f))<0 || fseek(f,0,SEEK_SET)) { fputs("cannot read fixture\n",stderr);exit(2); }
    args[0]=(int32_t)(namesize+length);offset=call(WCR_RESERVE_INPUT,args);
    if(!offset) { fputs("input reservation failed\n",stderr);exit(1); }
    memcpy(span(offset,(uint32_t)(namesize+length)),path,namesize);
    if(fread(span(offset+(uint32_t)namesize,(uint32_t)length),1,(size_t)length,f)!=(size_t)length)exit(2);
    fclose(f);args[0]=(int32_t)namesize;args[1]=(int32_t)length;
    if(call(WCR_ACCEPT_INPUT,args)) { fputs("fixture rejected\n",stderr);exit(1); }
}
int main(int argc,char **argv) {
    const char *error;WcRaster *raster;int32_t args[3]={800,600,0};unsigned frame;size_t size,stride;uint32_t offset,length,w,h;WcRasterMemory stats;const uint8_t *pixels;char path[512];
    float_mode();error=wc_reactor_start();if(error) { fprintf(stderr,"FAIL instantiate: %s\n",error);return 1; }
    if(call(WCR_INIT,args)) { fputs("FAIL init status\n",stderr);return 1; }
    event(20,1,0,0,0); /* Enable the same accessible packet used by Node QA. */
    raster=wc_raster_create();if(!raster)return 1;
    for(frame=0;frame<8;frame++) {
        if(frame==3 && argc>2) { event(2,79,1,0,2);event(2,79,0,0,0); }
        args[2]=(int32_t)(frame*16);
        if(call(WCR_FRAME,args)) { fputs("FAIL frame status\n",stderr);return 1; }
        offset=call(WCR_DRAW_PTR,NULL);length=call(WCR_DRAW_LEN,NULL);
        if(!wc_raster_frame(raster,800,600,span(offset,length),length)) { fprintf(stderr,"FAIL raster: %s\n",wc_raster_error(raster));return 1; }
        pixels=wc_raster_pixels(raster,&w,&h,&stride);wc_raster_memory(raster,&stats);wc_reactor_memory(&size);
        printf("frame=%u packet=%lu memory=%lu surface=%lu vertices=%lu textures=%lu hash=%08lx random_calls=%lu\n",frame,(unsigned long)length,(unsigned long)size,(unsigned long)stats.surface_bytes,(unsigned long)stats.vertex_bytes,(unsigned long)stats.texture_bytes,(unsigned long)hash(pixels,stride*h),(unsigned long)random_calls);fflush(stdout);
        if(argc>1) { sprintf(path,"%s-%u.wcg",argv[1],frame);dump(path,span(offset,length),length); }
        if(argc>1 && (frame==2 || frame==7)) { sprintf(path,"%s-%u.ppm",argv[1],frame);ppm(path,pixels,w,h,stride); }
        offset=call(WCR_A11Y_PTR,NULL);length=call(WCR_A11Y_LEN,NULL);
        if(argc>1) { sprintf(path,"%s-%u.a11y",argv[1],frame);dump(path,span(offset,length),length); }
        call(WCR_DRAW_CONSUMED,NULL);
        if(call(WCR_DRAW_LEN,NULL)!=0) { fputs("draw consumption retained packet\n",stderr);return 1; }
        if(frame==3 && argc>2) { if(call(WCR_OPEN_REQUESTED,NULL)!=1) { fputs("open shortcut did not request input\n",stderr);return 1; }input(argv[2]); }
    }
    printf("PASS native i386 engine init, eight frames, WCG1 raster and packet release; saves=%lu\n",(unsigned long)save_calls);
    wc_raster_destroy(raster);wc_reactor_shutdown();return 0;
}
