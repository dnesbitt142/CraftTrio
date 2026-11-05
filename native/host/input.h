/* Bounded, direct file input. No Presentation Manager or engine dependency. */
#ifndef WC_INPUT_H
#define WC_INPUT_H
#include <stdint.h>

typedef struct WcInput WcInput;

/* NULL on success; otherwise a stable, human-readable error. The size comes
 * from the caller's validated file metadata. No file-sized staging buffer is
 * allocated. The caller owns the handle until read_close or discard. */
const char *wc_input_open(const char *path, uint32_t size, WcInput **input);

/* Consume and close input, including on failure. The destination capacity
 * must exactly match the validated file size. Detect short reads, file growth,
 * stream errors and close errors before the caller accepts the new document. */
const char *wc_input_read_close(WcInput *input, void *destination, uint32_t capacity);
void wc_input_discard(WcInput *input);
#endif
