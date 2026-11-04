#ifndef WC_RNG_H
#define WC_RNG_H
#include <stddef.h>

/* All calls run on the application's PM thread; state is process-private.
 * Successful reads fill the whole buffer. Failed reads zero the buffer.
 */
const char *wc_rng_prepare(void);
int wc_rng_fill(void *data, size_t size);
const char *wc_rng_error(void);
void wc_rng_shutdown(void);
#endif
