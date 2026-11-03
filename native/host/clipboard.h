#ifndef WC_CLIPBOARD_H
#define WC_CLIPBOARD_H

#include <stddef.h>
#include <stdint.h>

/* Convert between UTF-8 and the active PM code page. The caller owns termination.
 * Return the number of bytes written, or SIZE_MAX if the buffer was too small. */
size_t wc_pm_encode(const unsigned char *utf8, size_t length,
                    unsigned char *output, size_t capacity);
size_t wc_pm_decode(const unsigned char *text, size_t length,
                    unsigned char *output, size_t capacity);

uint32_t wc_clipboard_read(uint32_t kind, uint32_t offset, uint32_t capacity);
uint32_t wc_clipboard_write(uint32_t text, uint32_t text_len, uint32_t html,
                            uint32_t html_len, uint32_t math, uint32_t math_len);
uint32_t wc_clipboard_image(uint32_t offset, uint32_t width, uint32_t height);

#endif
