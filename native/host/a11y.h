#ifndef WC_A11Y_H
#define WC_A11Y_H

#include <stddef.h>
#include <stdint.h>

typedef void (*WcA11yEvent)(int kind, int a, int b, int c, int modifiers);

/* The host calls these on the Presentation Manager thread. */
BOOL wc_a11y_key(HAB anchor, HWND owner, HWND editor, MPARAM mp1, MPARAM mp2, WcA11yEvent dispatch);
BOOL wc_a11y_enabled(void);
BOOL wc_a11y_update(const uint8_t *packet, size_t length);
void wc_a11y_focus(BOOL focused);
void wc_a11y_shutdown(void);

#endif
