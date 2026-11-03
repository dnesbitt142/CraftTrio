#ifndef CRAFTPM_H
#define CRAFTPM_H

#include "reactor.h"

#define WC_PM_ABI 1
#define WC_APP_ICON 1

typedef struct WcPmConfig {
    const char *title;
    const char *open_titles[4];
    const char *document_filter;
    const char *document_extension;
    const char *gui_log;
    const char *engine_log;
    const char *release_url;
    uint32_t icon_module;
    int cancel_input;
} WcPmConfig;

typedef struct WcPmEngine {
    const char *(*start)(void);
    void (*shutdown)(void);
    const char *(*call)(WcOperation, const int32_t *, uint32_t *);
    uint8_t *(*memory)(size_t *);
} WcPmEngine;

/* All calls run on the app's PM thread. Buffers stay with the caller. */
typedef struct WcPmApi {
    int (*run)(const WcPmConfig *, const WcPmEngine *);
    uint32_t (*wc_wasi_empty_sizes)(uint32_t, uint32_t);
    uint32_t (*wc_wasi_random_get)(uint32_t, uint32_t);
    uint32_t (*wc_wasi_clock_time)(uint32_t, uint32_t);
    uint32_t (*wc_wasi_clock_res)(uint32_t, uint32_t);
    uint32_t (*wc_wasi_fd_write)(uint32_t, uint32_t, uint32_t, uint32_t);
    uint32_t (*wc_wasi_fd_read)(uint32_t, uint32_t);
    uint32_t (*wc_wasi_fdstat)(uint32_t, uint32_t);
    uint32_t (*wc_wasi_close)(uint32_t);
    uint32_t (*wc_wasi_seek)(uint32_t);
    uint32_t (*wc_wasi_yield)(void);
    int32_t (*wc_host_save)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    int32_t (*wc_net_socket)(uint32_t, uint32_t, uint32_t);
    void (*wc_net_open_url)(uint32_t, uint32_t);
    int32_t (*wc_net_release)(uint32_t, uint32_t, uint32_t);
    int32_t (*wc_print_queues)(uint8_t *, uint32_t);
    int32_t (*wc_print_start)(const char *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    int32_t (*wc_print_page)(const uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t);
    int32_t (*wc_print_finish)(int);
    uint32_t (*wc_clipboard_read)(uint32_t, uint32_t, uint32_t);
    uint32_t (*wc_clipboard_write)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    uint32_t (*wc_clipboard_image)(uint32_t, uint32_t, uint32_t);
} WcPmApi;

typedef const WcPmApi *(*WcPmGetApi)(uint32_t);

/* This is the shared module's only export. */
const WcPmApi *wc_pm_get_api(uint32_t version);

/* App-side engine adapters use this pointer after the module is loaded. */
extern const WcPmApi *wc_pm;

/* Internal to the shared PM host. */
const WcPmConfig *wc_pm_config(void);
uint8_t *wc_pm_memory(size_t *size);

#endif
