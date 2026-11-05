/* Compare exact OpenWatcom i386 renderers; no OS/2 loader/GPI assertion. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "raster.h"
#ifdef QA_ALLOC_TEST
#include "alloc-shim.h"
/* This managed host has no /proc task inventory for LeakSanitizer.
 * Requested allocations are accounted explicitly; ASan/UBSan remain active. */
int __lsan_is_turned_off(void) { return 1; }
#endif

typedef struct BaseRaster BaseRaster;
BaseRaster *base_wc_raster_create(void);
void base_wc_raster_destroy(BaseRaster *raster);
int base_wc_raster_frame(BaseRaster *, uint32_t, uint32_t, const void *, size_t);
const uint8_t *base_wc_raster_pixels(const BaseRaster *, uint32_t *, uint32_t *, size_t *);

static uint8_t packet[65536];
static size_t length;
static uint32_t seed = 0x43524146UL;
static uint32_t digest = 2166136261UL;
static unsigned frames;

static void word(uint32_t value)
{
    unsigned i;
    if (length + 4 > sizeof(packet)) abort();
    for (i=0; i<4; ++i) packet[length++] = (uint8_t)(value >> (8*i));
}

static void real(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, 4);
    word(bits);
}

static uint32_t random_word(void)
{
    seed = seed * 1664525UL + 1013904223UL;
    return seed;
}

static void header(unsigned uploads, unsigned meshes)
{
    length = 0;
    memcpy(packet,"WCG1",4); length=4;
    word(uploads);word(meshes);word(0);
}

static void texture(void)
{
    word(1);word(0xffffffffUL);word(0xffffffffUL);word(2);word(2);
    word(0xff0000ffUL);word(0xff00ff00UL);
    word(0xffff0000UL);word(0xffffffffUL);
}

static void vertex(float x, float y, float u, float v, uint32_t color)
{
    real(x);real(y);real(u);real(v);word(color);
}

static void quad(uint32_t width, uint32_t height)
{
    word(1);real(0);real(0);real((float)width);real((float)height);word(4);word(6);
    vertex(0,0,0,0,0xffffffffUL);
    vertex((float)width,0,1,0,0xffffffffUL);
    vertex((float)width,(float)height,1,1,0xffffffffUL);
    vertex(0,(float)height,0,1,0xffffffffUL);
    word(0);word(1);word(2);word(0);word(2);word(3);
}

static int equal_pixels(BaseRaster *base, WcRaster *next, int golden)
{
    uint32_t bw,bh,nw,nh,x,y;
    size_t bs,ns;
    const uint8_t *b = base_wc_raster_pixels(base,&bw,&bh,&bs);
    const uint8_t *n = wc_raster_pixels(next,&nw,&nh,&ns);
    static const uint8_t expected[] = {0,0,255, 0,255,0, 255,0,0, 255,255,255};
    if (!b || !n || bw!=nw || bh!=nh || bs!=(size_t)bw*4 || ns!=(((size_t)nw*3+3)&~(size_t)3)) return 0;
    for(y=0;y<nh;++y) {
        const uint8_t *row = n+(size_t)(nh-1-y)*ns;
        for(x=0;x<nw;++x) {
            unsigned c;
            for(c=0;c<3;++c) {
                uint8_t value=row[(size_t)x*3+c];
                if(value!=b[(size_t)y*bs+(size_t)x*4+c]) return 0;
                if(golden && value!=expected[((size_t)y*nw+x)*3+c]) return 0;
                digest=(digest^value)*16777619UL;
            }
        }
        for(x=nw*3;x<ns;++x) if(row[x]) return 0;
    }
    ++frames;
    return 1;
}

#ifdef QA_ALLOC_TEST
static int allocation_faults(void)
{
    long failure;
    unsigned rejected=0;
    for(failure=0;failure<8;++failure) {
        WcRaster *raster=wc_raster_create();
        const uint8_t *pixels;
        uint32_t width,height,i;
        size_t stride;
        uint8_t previous[23*3*11+33];
        int ok;
        if(!raster)return 0;
        header(1,1);texture();quad(23,11);
        if(!wc_raster_frame(raster,23,11,packet,length))return 0;
        pixels=wc_raster_pixels(raster,&width,&height,&stride);
        if(stride*height>sizeof(previous))return 0;
        memcpy(previous,pixels,stride*height);
        header(1,1);
        word(1);word(0xffffffffUL);word(0xffffffffUL);word(8);word(8);
        for(i=0;i<64;++i)word(0xff010203UL);
        word(1);real(0);real(0);real(1025);real(769);word(630);word(630);
        for(i=0;i<630;++i)vertex((float)(i%21),(float)(i/21),0,0,0xffffffffUL);
        for(i=0;i<630;++i)word(i);
        qa_memory_fail_after(failure);
        ok=wc_raster_frame(raster,1025,769,packet,length);
        qa_memory_fail_after(-1);
        if(!ok) {
            ++rejected;
            pixels=wc_raster_pixels(raster,&width,&height,&stride);
            if(width!=23||height!=11||memcmp(previous,pixels,stride*height))return 0;
            header(0,1);quad(23,11);
            if(!wc_raster_frame(raster,23,11,packet,length))return 0;
            pixels=wc_raster_pixels(raster,&width,&height,&stride);
            if(memcmp(previous,pixels,stride*height))return 0;
        }
        wc_raster_destroy(raster);
        if(qa_memory_live())return 0;
    }
    printf("PASS allocation failures rejected=%u, rollback and zero live allocations\n",rejected);
    return rejected>0;
}
#endif

int main(void)
{
    BaseRaster *base=base_wc_raster_create();
    WcRaster *next=wc_raster_create();
    WcRasterMemory memory;
    unsigned i,j;
    if(!base||!next) return 1;
    header(1,1);texture();quad(2,2);
    if(!base_wc_raster_frame(base,2,2,packet,length) || !wc_raster_frame(next,2,2,packet,length) || !equal_pixels(base,next,1)) return 2;
    puts("PASS golden BGR colors, orientation and padded rows");
    for(i=0;i<60;++i) {
        uint32_t width=1+(random_word()%63),height=1+(random_word()%45);
        header(0,1);
        word(1);real(-2);real(0);real((float)width);real((float)height);word(24);word(24);
        for(j=0;j<24;++j) {
            float x=(float)(int)(random_word()%(width+16))-8;
            float y=(float)(int)(random_word()%(height+16))-8;
            uint32_t alpha=32+(random_word()%224);
            uint32_t color=(alpha<<24) | ((random_word()%(alpha+1))<<16) |
                           ((random_word()%(alpha+1))<<8) | (random_word()%(alpha+1));
            vertex(x,y,(float)(random_word()%500)/256.0f-.5f,
                   (float)(random_word()%500)/256.0f-.5f,color);
        }
        for(j=0;j<24;++j)word(j);
        if(!base_wc_raster_frame(base,width,height,packet,length)||!wc_raster_frame(next,width,height,packet,length)||!equal_pixels(base,next,0)) return 3;
        /* A truncated packet must not alter the last image. */
        if(base_wc_raster_frame(base,width,height,packet,length-1)||wc_raster_frame(next,width,height,packet,length-1)||!equal_pixels(base,next,0)) return 4;
    }
    puts("PASS textured/blended/clipped triangles and invalid-frame rollback");
    header(0,0);
    if(!wc_raster_frame(next,1280,751,packet,length))return 5;
    wc_raster_memory(next,&memory);
    if(memory.surface_bytes!=(size_t)1280*751*3)return 6;
    printf("PASS surface bytes at1280x751=%lu\n",(unsigned long)memory.surface_bytes);
    if(!wc_raster_frame(next,2560,1600,packet,length)||!wc_raster_frame(next,640,480,packet,length))return 7;
    wc_raster_memory(next,&memory);
    if(memory.surface_bytes!=(size_t)640*480*3)return 8;
    printf("PASS shrink surface bytes=%lu\n",(unsigned long)memory.surface_bytes);
    base_wc_raster_destroy(base);wc_raster_destroy(next);
#ifdef QA_ALLOC_TEST
    if(qa_memory_live()||!allocation_faults())return 9;
#endif
    printf("PASS %u canonical comparisons digest=%08lx\n",frames,(unsigned long)digest);
    return 0;
}
