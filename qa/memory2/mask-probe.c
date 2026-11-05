/* Exact compiled-code regression for mask textures. MIT OR Apache-2.0.
 * No OS/2 loader/GPI or complete-application claim is made by this probe.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "raster.h"

#ifdef QA_ALLOC_TEST
#include "../memory/alloc-shim.h"
#undef malloc
#undef calloc
#undef realloc
#undef free
int __lsan_is_turned_off(void) { return 1; }
#endif

typedef struct BaseRaster BaseRaster;
BaseRaster *base_wc_raster_create(void);
void base_wc_raster_destroy(BaseRaster *);
int base_wc_raster_frame(BaseRaster *, uint32_t, uint32_t, const void *, size_t);
const uint8_t *base_wc_raster_pixels(const BaseRaster *, uint32_t *, uint32_t *, size_t *);
void base_wc_raster_memory(const BaseRaster *, WcRasterMemory *);

static uint8_t *packet;
static size_t length;
static const size_t capacity = 4UL * 1024UL * 1024UL + 65536UL;
static unsigned comparisons;
static uint32_t seed = 0x4d41534bUL;

static uint32_t random_word(void)
{
    seed = seed * 1664525UL + 1013904223UL;
    return seed;
}

static void word(uint32_t value)
{
    unsigned i;
    if (length + 4 > capacity) abort();
    for (i = 0; i < 4; ++i) packet[length++] = (uint8_t)(value >> (8 * i));
}

static void real(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, 4);
    word(bits);
}

static void header(unsigned uploads, unsigned meshes)
{
    memcpy(packet, "WCG1", 4);
    length = 4;
    word(uploads); word(meshes); word(0);
}

static void texture(uint32_t width, uint32_t height, unsigned format,
                    uint32_t x, uint32_t y)
{
    uint32_t i;
    word(1); word(x); word(y); word(width); word(height);
    for (i = 0; i < width * height; ++i) {
        const uint32_t coverage = (i * 37U + 19U) & 255U;
        word(format == 1 ? coverage * 0x01010101UL :
             format == 2 ? 0x81808080UL : 0xff204080UL);
    }
}

static void vertex(float x, float y, float u, float v, uint32_t color)
{
    real(x); real(y); real(u); real(v); word(color);
}

static void quad(uint32_t width, uint32_t height)
{
    word(1); real(0); real(0); real((float)width); real((float)height);
    word(4); word(6);
    vertex(0, 0, 0, 0, 0xc8785010UL);
    vertex((float)width, 0, 1, 0, 0xc8785010UL);
    vertex((float)width, (float)height, 1, 1, 0xc8785010UL);
    vertex(0, (float)height, 0, 1, 0xc8785010UL);
    word(0); word(1); word(2); word(0); word(2); word(3);
}

static int equal_pixels(BaseRaster *base, WcRaster *next)
{
    uint32_t bw, bh, nw, nh;
    size_t bs, ns;
    const uint8_t *a = base_wc_raster_pixels(base, &bw, &bh, &bs);
    const uint8_t *b = wc_raster_pixels(next, &nw, &nh, &ns);
    if (!a || !b || bw != nw || bh != nh || bs != ns) return 0;
    return memcmp(a, b, bs * bh) == 0;
}

static int compare(BaseRaster *base, WcRaster *next, uint32_t width,
                    uint32_t height, size_t used, int expected)
{
    const int a = base_wc_raster_frame(base, width, height, packet, used);
    const int b = wc_raster_frame(next, width, height, packet, used);
    if (a != expected || b != expected || !equal_pixels(base, next)) return 0;
    ++comparisons;
    return 1;
}

#ifdef QA_ALLOC_TEST
static int allocation_faults(void)
{
    unsigned scenario, rejected = 0;
    long position;
    for (scenario = 0; scenario < 3; ++scenario) {
        unsigned scenario_rejected = 0;
        for (position = 0; position < 10; ++position) {
            WcRaster *raster = wc_raster_create();
            WcRasterMemory memory;
            const uint8_t *pixels;
            uint8_t previous[1024];
            uint32_t width, height, i;
            size_t stride;
            int ok;
            if (!raster) return 0;
            header(1, 1); texture(2, 2, 1, UINT32_MAX, UINT32_MAX); quad(23, 11);
            if (!wc_raster_frame(raster, 23, 11, packet, length)) return 0;
            pixels = wc_raster_pixels(raster, &width, &height, &stride);
            if (stride * height > sizeof(previous)) return 0;
            memcpy(previous, pixels, stride * height);
            header(scenario == 2 ? 4 : 1, 1);
            if (scenario == 2) texture(2, 2, 1, UINT32_MAX, UINT32_MAX);
            texture(1, 1, 4, 1, 1);
            if (scenario == 2) {
                texture(3, 1, 1, UINT32_MAX, UINT32_MAX);
                texture(1, 1, 4, 2, 0);
            }
            if (scenario == 1) {
                word(1); real(0); real(0); real(1025); real(769); word(630); word(630);
                for (i = 0; i < 630; ++i)
                    vertex((float)(i % 21), (float)(i / 21), 0, 0, UINT32_MAX);
                for (i = 0; i < 630; ++i) word(i);
            } else quad(23, 11);
            qa_memory_fail_after(position);
            ok = wc_raster_frame(raster, scenario == 1 ? 1025 : 23,
                                  scenario == 1 ? 769 : 11, packet, length);
            qa_memory_fail_after(-1);
            if (!ok) {
                ++rejected; ++scenario_rejected;
                pixels = wc_raster_pixels(raster, &width, &height, &stride);
                wc_raster_memory(raster, &memory);
                if (width != 23 || height != 11 || memory.texture_bytes != 4 ||
                    memcmp(previous, pixels, stride * height)) return 0;
                header(0, 1); quad(23, 11);
                if (!wc_raster_frame(raster, 23, 11, packet, length)) return 0;
                pixels = wc_raster_pixels(raster, &width, &height, &stride);
                if (memcmp(previous, pixels, stride * height)) return 0;
            }
            wc_raster_destroy(raster);
            if (qa_memory_live()) return 0;
        }
        if (!scenario_rejected) return 0;
    }
    printf("PASS required allocation failures=%u; original mask/cache/image retained\n", rejected);
    return rejected >= 7;
}
#endif

int main(void)
{
    BaseRaster *base;
    WcRaster *next;
    WcRasterMemory old_memory, new_memory;
    unsigned i, j;
    packet = malloc(capacity);
    base = base_wc_raster_create();
    next = wc_raster_create();
    if (!packet || !base || !next) return 1;

    header(1, 1); texture(8, 8, 1, UINT32_MAX, UINT32_MAX); quad(31, 17);
    if (!compare(base, next, 31, 17, length, 1)) return 2;
    wc_raster_memory(next, &new_memory);
    if (new_memory.texture_bytes != 64) return 3;
    header(1, 1); texture(2, 2, 1, 2, 3); quad(31, 17);
    if (!compare(base, next, 31, 17, length, 1)) return 4;
    header(1, 1); texture(1, 1, 4, 4, 4); quad(31, 17);
    if (!compare(base, next, 31, 17, length, 1)) return 5;
    wc_raster_memory(next, &new_memory);
    if (new_memory.texture_bytes != 256) return 6;
    header(1, 1); texture(8, 8, 1, UINT32_MAX, UINT32_MAX); quad(31, 17);
    if (!compare(base, next, 31, 17, length, 1)) return 7;
    wc_raster_memory(next, &new_memory);
    if (new_memory.texture_bytes != 64) return 8;
    puts("PASS exact mask pixels, partial update, color promotion and recompaction");

    header(4, 1);
    texture(2, 2, 1, UINT32_MAX, UINT32_MAX);
    texture(1, 1, 4, 1, 1);
    texture(3, 1, 1, UINT32_MAX, UINT32_MAX);
    texture(1, 1, 4, 2, 0);
    quad(31, 17);
    if (!compare(base, next, 31, 17, length, 1)) return 9;
    puts("PASS ordered format and dimension transitions in one packet");

    for (i = 0; i < 80; ++i) {
        const uint32_t width = 1 + random_word() % 63;
        const uint32_t height = 1 + random_word() % 45;
        header(2, 1);
        texture(8, 8, 1, UINT32_MAX, UINT32_MAX);
        texture(2, 2, i % 3 ? 1 : 4, random_word() % 7, random_word() % 7);
        word(1); real(-2); real(0); real((float)width); real((float)height); word(24); word(24);
        for (j = 0; j < 24; ++j) {
            const uint32_t a = random_word() & 255U;
            const uint32_t color = (a << 24) | ((random_word() % (a + 1)) << 16) |
                                   ((random_word() % (a + 1)) << 8) | random_word() % (a + 1);
            vertex((float)(int)(random_word() % (width + 16)) - 8,
                   (float)(int)(random_word() % (height + 16)) - 8,
                   (float)(random_word() % 500) / 256.0f - .5f,
                   (float)(random_word() % 500) / 256.0f - .5f, color);
        }
        for (j = 0; j < 24; ++j) word(j);
        if (!compare(base, next, width, height, length, 1)) return 10;
        if (!compare(base, next, width, height, length - 1, 0)) return 11;
    }
    puts("PASS 80 deterministic mixed-mask/color bilinear frames and truncation rollback");

    header(1, 1); texture(1024, 1024, 1, UINT32_MAX, UINT32_MAX); quad(17, 13);
    if (!compare(base, next, 17, 13, length, 1)) return 12;
    base_wc_raster_memory(base, &old_memory);
    wc_raster_memory(next, &new_memory);
    if (old_memory.texture_bytes != 4UL * 1024UL * 1024UL ||
        new_memory.texture_bytes != 1024UL * 1024UL) return 13;
    printf("PASS 1024-square cache old=%lu new=%lu saved=%lu bytes\n",
           (unsigned long)old_memory.texture_bytes, (unsigned long)new_memory.texture_bytes,
           (unsigned long)(old_memory.texture_bytes - new_memory.texture_bytes));

    base_wc_raster_destroy(base);
    wc_raster_destroy(next);
#ifdef QA_ALLOC_TEST
    if (qa_memory_live() || !allocation_faults()) return 14;
#endif
    free(packet);
    printf("PASS %u exact BGR24 comparisons; no OS/2/GPI runtime claim\n", comparisons);
    return 0;
}
