/* Exercise extracted production startup/dispatch code under reentrant PM shims.
 * This is a host-state test, not OS/2 ABI, document I/O, or ArcaOS FOC testing. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "reactor.h"

typedef uintptr_t HAB, HMQ, HWND, MPARAM, MRESULT;
typedef uint32_t ULONG;
typedef int BOOL;
typedef struct WcRaster { int unused; } WcRaster;
typedef struct {
    const char *(*start)(void);
    const char *(*call)(WcOperation, const int32_t *, uint32_t *);
} WcPmEngine;
#define EXPENTRY
#define WM_USER 0x1000
#define WM_QUIT 0x002a
#define MPFROMP(p) ((MPARAM)(uintptr_t)(p))
#define MPFROMLONG(n) ((MPARAM)(uint32_t)(n))
#define POINTER_TIMER 1
#define HWND_DESKTOP ((HWND)1)

#include "app.inc"
static const WcPmEngine *app_engine;
typedef struct { HWND window; ULONG msg; MPARAM first, second; } Packet;
static Packet posted, startup;
static int starts, inits, frames, failures, pickers, last_kind, requests;
static int hook_rng, hook_start, hook_fail, start_error, init_error, rng_error;
static int in_hook;
static const char *last_stage, *last_error;
static WcRaster raster;
static MRESULT client_proc(HWND, ULONG, MPARAM, MPARAM);

int __lsan_is_turned_off(void) { return 1; }
static void check(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "FAIL %s: starts=%d inits=%d frames=%d failures=%d\n%s\n%s\n",
                what, starts, inits, frames, failures,
                last_stage ? last_stage : "", last_error ? last_error : "");
        exit(1);
    }
}
static void replay(void) {
    if (in_hook) return;
    in_hook = 1;
    client_proc(startup.window, startup.msg, startup.first, startup.second);
    in_hook = 0;
}
static int dimensions(HWND window, int32_t *size) {
    (void)window; size[0] = 913; size[1] = 659; return 1;
}
static void log_line(const char *a, const char *b) { (void)a; (void)b; }
static void fail(const char *stage, const char *error) {
    app.failed = 1; ++failures; last_stage = stage; last_error = error;
    if (hook_fail) { hook_fail = 0; replay(); }
}
static const char *wc_rng_prepare(void) {
    if (hook_rng) { hook_rng = 0; replay(); }
    return rng_error ? "test random initialization failure" : NULL;
}
static const char *wc_rng_error(void) { return NULL; }
static void wasm_float_mode(void) { }
static WcRaster *wc_raster_create(void) { return &raster; }
static uint32_t ticks(void) { return 100; }
static int WinStartTimer(HAB a, HWND w, int id, int delay) {
    (void)a; (void)w; (void)delay; return id;
}
static HWND WinQueryFocus(HWND desktop) { (void)desktop; return app.client; }
static void send_event(int a, int b, int c, int d, int e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
}
static void request_frame(void) { }
static void input_error(const char *error) { fail("input error", error); }
static void cancel_input(void) { }
static BOOL WinPostMsg(HWND w, ULONG msg, MPARAM a, MPARAM b) {
    posted = (Packet){w, msg, a, b}; return 1;
}
static MRESULT WinDefWindowProc(HWND w, ULONG m, MPARAM a, MPARAM b) {
    (void)w; (void)m; (void)a; (void)b; return 77;
}
static void pick_input(uint32_t kind) {
    if (!app.ready || app.failed || app.busy) return;
    check(kind >= 1 && kind <= 4, "valid picker kind");
    ++pickers; last_kind = (int)kind;
}

#include "invoke.inc"
#include "start.inc"
#include "after-frame.inc"
#include "dispatch.inc"

static const char *engine_start(void) {
    ++starts;
    if (starts > 1) return "application engine is already running";
    if (hook_start) { hook_start = 0; replay(); }
    return start_error ? "test engine startup failure" : NULL;
}
/* Called during real production invoke(WCR_FRAME), which owns app.busy. */
static void modal_save(void) {
    uint32_t ignored = 0;
    const char *error;
    check(app.busy, "modal save is inside engine invocation");
    client_proc(app.client, WM_USER + 1, 0, 0);
    client_proc(app.client, startup.msg, 0, 0);
    replay();
    client_proc(app.client, WM_USER + 3, MPFROMLONG(1), 0);
    check(starts == 1 && !app.failed, "modal startup replay inert");
    error = invoke(WCR_FRAME, NULL, &ignored);
    check(error && !strcmp(error, "Recursive engine invocation"), "recursive frame blocked");
    check(app.busy, "nested messages did not release engine ownership");
}
static const char *engine_call(WcOperation op, const int32_t *args, uint32_t *result) {
    (void)args; *result = 0;
    switch (op) {
    case WCR_INIT: ++inits; return init_error ? "test application initialization failure" : NULL;
    case WCR_FRAME: ++frames; modal_save(); break;
    case WCR_CAPABILITIES: return "original ABI has no capabilities operation";
    case WCR_SHOULD_CLOSE: break;
    case WCR_OPEN_REQUESTED: *result = (uint32_t)requests; break;
    default: return "unexpected test operation";
    }
    return NULL;
}
static const WcPmEngine engine = {engine_start, engine_call};

static void boot(void) {
    /* The runner emits the actual WinPostMsg expression from production run_app. */
#include "post-start.inc"
    startup = posted;
    client_proc(startup.window, startup.msg, startup.first, startup.second);
}

int main(int argc, char **argv) {
    uint32_t result = 0;
    const char *name;
    check(argc == 2, "scenario argument"); name = argv[1];
    app.client = (HWND)0x101; app_engine = &engine;
    hook_rng = !strcmp(name, "nested-rng");
    hook_start = !strcmp(name, "nested-start");
    start_error = !strcmp(name, "start-failure");
    init_error = !strcmp(name, "init-failure");
    rng_error = !strcmp(name, "rng-failure");
    hook_fail = start_error || init_error || rng_error;
    if (!strcmp(name, "pre-busy")) app.busy = 1;
    if (!strcmp(name, "pre-failed")) app.failed = 1;
    if (!strcmp(name, "pre-ready")) app.ready = 1;
    boot();
    if (!strncmp(name, "pre-", 4)) {
        check(!starts && !inits, "startup refused in existing state");
    } else if (start_error || init_error || rng_error) {
        check(failures == 1 && app.failed, "one initialization failure only");
        check(starts == (rng_error ? 0 : 1), "failure dialog did not restart engine");
        check(inits == (init_error ? 1 : 0), "failure initialization count");
    } else {
        check(starts == 1 && inits == 1 && app.ready && !app.failed, "one successful startup");
        if (!strcmp(name, "modal-save")) {
            check(!invoke(WCR_FRAME, NULL, &result), "save callback completed");
            check(starts == 1 && frames == 1 && !app.failed && !app.busy, "modal startup replay inert");
        }
        replay(); replay();
        check(starts == 1 && inits == 1 && !app.failed, "idle startup replay inert");
        requests = 2; after_frame(app.client);
        check(posted.msg != startup.msg, "separate picker message");
        client_proc(app.client + 1, posted.msg, posted.first, posted.second);
        client_proc(app.client, posted.msg, posted.first, 0);
        check(pickers == 0, "unrelated picker messages ignored");
        client_proc(posted.window, posted.msg, posted.first, posted.second);
        check(pickers == 1 && last_kind == 2, "legitimate picker delivered");
    }
    printf("PASS %s starts=%d inits=%d frames=%d failures=%d\n", name, starts, inits, frames, failures);
    return 0;
}
