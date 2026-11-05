/* QA-only startup coverage with the exact native i386/OMF engine objects.
 * The RNG is deliberately deterministic and is unsuitable for production. */
#define main original_eight_frame_main
#include "probe.c"
#undef main
#include <errno.h>

static int number(const char *text, uint32_t maximum, uint32_t *out) {
    char *end; unsigned long value;
    errno=0;value=strtoul(text,&end,0);
    if(errno || !*text || *end || text[0]=='-' || value>maximum)return 0;
    *out=(uint32_t)value;return 1;
}

int main(int argc,char **argv) {
    uint32_t initial_seed,width,height,a11y,frames,frame,offset,length,w,h;
    int32_t args[3];const char *error;WcRaster *raster;
    size_t size,stride;const uint8_t *pixels;
    if(argc!=6 || !number(argv[1],UINT32_MAX,&initial_seed) ||
       !number(argv[2],4096,&width) || width<64 ||
       !number(argv[3],4096,&height) || height<64 ||
       !number(argv[4],1,&a11y) || !number(argv[5],32,&frames) || frames<3) {
        fputs("Usage: seed-probe SEED WIDTH HEIGHT A11Y(0|1) FRAMES(3..32)\n",stderr);return 2;
    }
    seed=initial_seed;args[0]=(int32_t)width;args[1]=(int32_t)height;args[2]=0;
    printf("case seed=%08lx geometry=%lux%lu a11y=%lu frames=%lu\n",
           (unsigned long)seed,(unsigned long)width,(unsigned long)height,
           (unsigned long)a11y,(unsigned long)frames);fflush(stdout);
    float_mode();error=wc_reactor_start();
    if(error) { fprintf(stderr,"FAIL instantiate: %s\n",error);return 1; }
    if(call(WCR_INIT,args)) { fputs("FAIL init status\n",stderr);return 1; }
    if(a11y)event(20,1,0,0,0);
    event(5,1,0,0,0);
    raster=wc_raster_create();if(!raster)return 1;
    for(frame=0;frame<frames;++frame) {
        args[2]=(int32_t)(frame*17000UL);
        if(call(WCR_FRAME,args)) { fputs("FAIL frame status\n",stderr);return 1; }
        offset=call(WCR_DRAW_PTR,NULL);length=call(WCR_DRAW_LEN,NULL);
        if(!wc_raster_frame(raster,width,height,span(offset,length),length)) {
            fprintf(stderr,"FAIL raster: %s\n",wc_raster_error(raster));return 1;
        }
        pixels=wc_raster_pixels(raster,&w,&h,&stride);wc_reactor_memory(&size);
        printf("frame=%lu packet=%lu memory=%lu hash=%08lx random_calls=%lu\n",
               (unsigned long)frame,(unsigned long)length,(unsigned long)size,
               (unsigned long)hash(pixels,stride*h),(unsigned long)random_calls);fflush(stdout);
        call(WCR_DRAW_CONSUMED,NULL);
        if(call(WCR_DRAW_LEN,NULL)) { fputs("FAIL draw packet retained\n",stderr);return 1; }
    }
    wc_raster_destroy(raster);wc_reactor_shutdown();
    puts("PASS seeded native startup, raster and packet release");return 0;
}
