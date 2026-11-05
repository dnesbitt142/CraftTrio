#ifndef WC_GUI_FILES_H
#define WC_GUI_FILES_H

#include <stdint.h>
#include "input.h"

#define WC_GUI_PATH_CAP 260

/* 1: selected/opened, 0: cancelled, -1: error shown. Caller owns the handle.
 * Read into validated engine storage using wc_input_read_close(), which also
 * closes the handle. Discard it on every earlier cancellation/error path. */
int wc_gui_pick_input(uint32_t kind, char path[WC_GUI_PATH_CAP],
                      WcInput **input, uint32_t *size);
void wc_gui_input_accepted(uint32_t kind, const char *path);

/* The caller checks engine memory ranges before passing host pointers. */
int32_t wc_gui_save(const char *name, uint32_t name_size, const void *data,
                    uint32_t data_size, uint32_t save_as, char *output,
                    uint32_t output_size);

#endif
