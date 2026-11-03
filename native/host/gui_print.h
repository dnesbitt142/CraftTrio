#ifndef WC_GUI_PRINT_H
#define WC_GUI_PRINT_H

#include <stdint.h>

/* These routines run on the thread that owns the Presentation Manager queue. */
int32_t wc_print_queues(uint8_t *output, uint32_t capacity);
int32_t wc_print_start(const char *name, uint32_t length, uint32_t orientation,
                       uint32_t paper, uint32_t duplex, uint32_t grayscale);
int32_t wc_print_page(const uint8_t *rgba, uint32_t width, uint32_t height,
                      uint32_t width_millipoints, uint32_t height_millipoints);
int32_t wc_print_finish(int cancel);

#endif
