#ifndef WC_REACTOR_H
#define WC_REACTOR_H

#include <stddef.h>
#include <stdint.h>

typedef enum WcOperation {
    WCR_INIT, WCR_FRAME, WCR_EVENT, WCR_TEXT, WCR_DRAW_PTR, WCR_DRAW_LEN,
    WCR_OPEN_REQUESTED, WCR_RESERVE_INPUT, WCR_ACCEPT_INPUT, WCR_CLOSE_REQUEST,
    WCR_SHOULD_CLOSE, WCR_A11Y_PTR, WCR_A11Y_LEN, WCR_REPAINT_DELAY
} WcOperation;

/* The engine is linked into the executable. Calls run on the PM thread. */
const char *wc_reactor_start(void);
void wc_reactor_shutdown(void);
const char *wc_reactor_call(WcOperation op, const int32_t *args, uint32_t *result);
uint8_t *wc_reactor_memory(size_t *size);
void wc_reactor_exit(uint32_t code);

#endif
