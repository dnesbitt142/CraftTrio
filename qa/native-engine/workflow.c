/* QA-only editing/serialization/PDF path using the unchanged native host.
 * Default probe.c benchmark behavior is preserved in its separate executable. */
#define main eight_frame_probe_main
#include "probe.c"
#undef main

static const char *wf_output, *wf_app;
static WcPmApi wf_api;
static WcRaster *wf_raster;
static unsigned wf_frame_count, wf_docs, wf_pdfs;
static uint32_t wf_clock;

static void wf_fail(const char *message) { fprintf(stderr,"FAIL workflow: %s\n",message);exit(1); }
static int32_t wf_save(uint32_t name,uint32_t namesize,uint32_t offset,uint32_t length,uint32_t as,uint32_t output,uint32_t capacity) {
    const uint8_t *bytes=span(offset,length);char path[1024];const char *extension;
    (void)name;(void)namesize;(void)as;(void)output;(void)capacity;
    if(length>=5 && memcmp(bytes,"%PDF-",5)==0) { extension="pdf";wf_pdfs++; }
    else if(length>=2 && bytes[0]=='P' && bytes[1]=='K') {
        extension=strcmp(wf_app,"word")==0?"docx":strcmp(wf_app,"grid")==0?"xlsx":"pptx";wf_docs++;
    } else wf_fail("save payload is neither Office ZIP nor PDF");
    sprintf(path,"%s/%s-%s.%s",wf_output,wf_app,strcmp(extension,"pdf")==0?"export":"saved",extension);
    dump(path,bytes,length);save_calls++;
    printf("save=%s bytes=%lu hash=%08lx\n",extension,(unsigned long)length,(unsigned long)hash(bytes,length));fflush(stdout);
    return 0;
}
static int32_t wf_queues(uint8_t *p,uint32_t size) { (void)p;(void)size;return 0; }
static int32_t wf_print_start(const char *p,uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e) {
    (void)p;(void)a;(void)b;(void)c;(void)d;(void)e;return -2; /* Request the engine's PDF fallback, as in Node QA. */
}
static void wf_frame(void) {
    int32_t args[3];uint32_t pointer,length,w,h;size_t stride,size;WcRasterMemory stats;const uint8_t *pixels;
    args[0]=1144;args[1]=688;args[2]=(int32_t)(wf_clock+=200);
    if(call(WCR_FRAME,args))wf_fail("frame returned failure");
    pointer=call(WCR_DRAW_PTR,NULL);length=call(WCR_DRAW_LEN,NULL);
    if(!wc_raster_frame(wf_raster,1144,688,span(pointer,length),length))wf_fail(wc_raster_error(wf_raster));
    pixels=wc_raster_pixels(wf_raster,&w,&h,&stride);wc_raster_memory(wf_raster,&stats);wc_reactor_memory(&size);
    printf("workflow_frame=%u packet=%lu memory=%lu surface=%lu textures=%lu hash=%08lx\n",++wf_frame_count,(unsigned long)length,(unsigned long)size,(unsigned long)stats.surface_bytes,(unsigned long)stats.texture_bytes,(unsigned long)hash(pixels,stride*h));fflush(stdout);
    call(WCR_DRAW_CONSUMED,NULL);
    if(call(WCR_DRAW_LEN,NULL))wf_fail("draw packet retained after consumption");
}
static void wf_key(int code,int modifiers) { event(2,code,1,0,modifiers);event(2,code,0,0,0);wf_frame(); }
static void wf_type(const char *text) {
    int32_t args[1];while(*text) { args[0]=(uint8_t)*text++;if(call(WCR_TEXT,args))wf_fail("text input failed"); }wf_frame();
}
static int wf_find(const char *label,int prefix,int document,uint32_t *low,uint32_t *high) {
    uint32_t size=call(WCR_A11Y_LEN,NULL),at=16,pointer=call(WCR_A11Y_PTR,NULL),header[6];const uint8_t *data=span(pointer,size);int found=0;char name[1025];
    if(size<16 || memcmp(data,"WCA1",4))wf_fail("invalid accessibility packet");
    while(at<size) {
        if(size-at<24)wf_fail("truncated accessibility row");memcpy(header,data+at,24);at+=24;
        if(header[4]>1024 || header[5]>32768 || header[4]+header[5]>size-at)wf_fail("invalid accessibility row length");
        memcpy(name,data+at,header[4]);name[header[4]]=0;
        if((header[2]&(document?3:1)) && !(header[2]&4) &&
            ((document && (header[2]&16)) || (!document && (prefix?strncmp(name,label,strlen(label))==0:strcmp(name,label)==0)))) {
            *low=header[0];*high=header[1];found=1;
        }
        at+=header[4]+header[5];
    }
    return found;
}
static void wf_activate(const char *label,int prefix) {
    uint32_t low,high;
    if(!wf_find(label,prefix,0,&low,&high)) { fprintf(stderr,"Missing accessible command: %s\n",label);wf_fail("command not found"); }
    event(21,(int32_t)low,(int32_t)high,0,0);wf_frame();wf_frame();
}
int main(int argc,char **argv) {
    const char *error;int32_t args[3]={1144,688,0};unsigned i;uint32_t low,high;char path[1024];size_t stride;uint32_t w,h;const uint8_t *pixels;
    if(argc!=4) { fputs("Usage: workflow APP FIXTURE OUTPUT_DIRECTORY\n",stderr);return 2; }
    wf_app=argv[1];wf_output=argv[3];wf_api=api;wf_api.wc_host_save=wf_save;wf_api.wc_print_queues=wf_queues;wf_api.wc_print_start=wf_print_start;wc_pm=&wf_api;
    float_mode();error=wc_reactor_start();if(error)wf_fail(error);
    if(call(WCR_INIT,args))wf_fail("init failed");event(20,1,0,0,0);
    wf_raster=wc_raster_create();if(!wf_raster)wf_fail("raster allocation failed");
    for(i=0;i<8;i++)wf_frame();
    wf_key(79,2);if(call(WCR_OPEN_REQUESTED,NULL)!=1)wf_fail("open shortcut not requested");
    args[0]=3<<20;if(!call(WCR_RESERVE_INPUT,args))wf_fail("large cancellation reserve failed");args[0]=args[1]=0;if(call(WCR_ACCEPT_INPUT,args))wf_fail("large input cancellation failed");wf_frame();
    wf_key(79,2);if(call(WCR_OPEN_REQUESTED,NULL)!=1)wf_fail("fixture open not requested");input(argv[2]);
    for(i=0;i<4;i++)wf_frame();
    if(strcmp(wf_app,"word")==0) {
        if(wf_find("",0,1,&low,&high)) { event(21,(int32_t)low,(int32_t)high,0,0);wf_frame(); }
        wf_key(260,2);wf_type("MEMORY_QA_WORD ");
    } else if(strcmp(wf_app,"grid")==0) { wf_key(260,2);wf_type("=20+22");wf_key(13,0);wf_key(258,0); }
    else if(strcmp(wf_app,"deck")==0)wf_key(78,3);
    else wf_fail("unknown application");
    wf_key(83,2);for(i=0;i<3 && !wf_docs;i++)wf_frame();if(!wf_docs)wf_fail("Ctrl+S produced no Office document");
    if(strcmp(wf_app,"word")==0) { wf_activate("Button - File",1);wf_activate("Button - Export",0);wf_activate("Button - PDF document (*.pdf)",0); }
    else if(strcmp(wf_app,"grid")==0) { wf_activate("Button - File",0);wf_activate("Button - Export",1);wf_activate("Button - Create PDF",0); }
    else { wf_key(80,2);wf_activate("Button - Print",0); }
    for(i=0;i<4 && !wf_pdfs;i++)wf_frame();if(!wf_pdfs)wf_fail("PDF export produced no save callback");
    wf_key(27,0);for(i=0;i<4;i++)wf_frame();
    sprintf(path,"%s/%s-final.a11y",wf_output,wf_app);low=call(WCR_A11Y_PTR,NULL);high=call(WCR_A11Y_LEN,NULL);dump(path,span(low,high),high);
    pixels=wc_raster_pixels(wf_raster,&w,&h,&stride);sprintf(path,"%s/%s-final.ppm",wf_output,wf_app);ppm(path,pixels,w,h,stride);
    printf("PASS native workflow: app=%s frames=%u document_saves=%u pdf_saves=%u\n",wf_app,wf_frame_count,wf_docs,wf_pdfs);
    wc_raster_destroy(wf_raster);wc_reactor_shutdown();return 0;
}
