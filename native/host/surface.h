/* Presentation Manager compatible software-rendering surface. */
#ifndef WC_SURFACE_H
#define WC_SURFACE_H

#include <stddef.h>
#include <stdint.h>

/* Zero-initialize before first use. */
typedef struct WcSurface {
    uint8_t *pixels;
    size_t capacity, stride;
    uint32_t width, height;
} WcSurface;

/* Bottom-up BGR24, with zeroed padding to a four-byte row boundary.
 * A failed resize leaves the previous surface and dimensions intact. */
int wc_surface_resize(WcSurface *surface, uint32_t width, uint32_t height);
void wc_surface_clear(WcSurface *surface, uint8_t gray);

/* Best effort: retain some slack to avoid allocation churn during resizing.
 * Call only after a completed frame; the pixels pointer may change. */
void wc_surface_trim(WcSurface *surface);
void wc_surface_destroy(WcSurface *surface);

#endif
