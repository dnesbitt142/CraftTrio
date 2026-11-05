/* Presentation Manager compatible software-rendering surface. */
#include "surface.h"

#include <stdlib.h>
#include <string.h>

#define MAX_BYTES (64UL * 1024UL * 1024UL)
#define TRIM_EXCESS (1024UL * 1024UL)

int wc_surface_resize(WcSurface *surface, uint32_t width, uint32_t height)
{
    size_t stride, bytes;
    uint8_t *replacement;
    if (!surface || !width || !height || width > (MAX_BYTES - 3U) / 3U)
        return 0;
    stride = ((size_t)width * 3U + 3U) & ~(size_t)3;
    if (height > MAX_BYTES / stride)
        return 0;
    bytes = stride * height;
    if (bytes > surface->capacity) {
        replacement = realloc(surface->pixels, bytes);
        if (!replacement)
            return 0;
        surface->pixels = replacement;
        surface->capacity = bytes;
    }
    surface->stride = stride;
    surface->width = width;
    surface->height = height;
    return 1;
}

void wc_surface_clear(WcSurface *surface, uint8_t gray)
{
    const size_t row = (size_t)surface->width * 3U;
    const size_t padding = surface->stride - row;
    uint32_t y;
    memset(surface->pixels, gray, surface->stride * surface->height);
    if (padding)
        for (y = 0; y < surface->height; ++y)
            memset(surface->pixels + (size_t)y * surface->stride + row, 0, padding);
}

void wc_surface_trim(WcSurface *surface)
{
    const size_t bytes = surface->stride * surface->height;
    uint8_t *replacement;
    if (!bytes || surface->capacity / 2U <= bytes ||
        surface->capacity - bytes < TRIM_EXCESS)
        return;
    replacement = realloc(surface->pixels, bytes);
    if (replacement) {
        surface->pixels = replacement;
        surface->capacity = bytes;
    }
}

void wc_surface_destroy(WcSurface *surface)
{
    free(surface->pixels);
    memset(surface, 0, sizeof(*surface));
}
