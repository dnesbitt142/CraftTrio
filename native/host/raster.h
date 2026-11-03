/* Software renderer for WordCraft's Open Watcom host. MIT OR Apache-2.0. */
#ifndef WC_RASTER_H
#define WC_RASTER_H

#include <stddef.h>
#include <stdint.h>

typedef struct WcRaster WcRaster;

WcRaster *wc_raster_create(void);
void wc_raster_destroy(WcRaster *raster);

/* Render a WCG1 packet at one pixel per point. A failed packet leaves the
 * previous image and texture state available. */
int wc_raster_frame(WcRaster *raster, uint32_t width, uint32_t height,
                    const void *data, size_t len);

/* BGRA32, opaque, with the top row first. Valid until the next frame or destroy. */
const uint8_t *wc_raster_pixels(const WcRaster *raster, uint32_t *width,
                                uint32_t *height, size_t *stride);
const char *wc_raster_error(const WcRaster *raster);

#endif
