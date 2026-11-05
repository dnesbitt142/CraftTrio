/* Shared Presentation Manager host for the native Craft Apps. */
#define INCL_WIN
#define INCL_DOSMISC
#define INCL_GPI
#include <os2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "craftpm.h"
#include "gui_files.h"
#include "gui_print.h"
#include "guest_services.h"
#include "raster.h"
#include "a11y.h"
#include "network.h"
#include "clipboard.h"
#include "rng.h"

#define WM_WCG_START (WM_USER + 1)
#define WM_WCG_PICK_INPUT (WM_USER + 3)
#define POINTER_TIMER 1
#define MAX_VIEW_WIDTH 2560
#define MAX_VIEW_HEIGHT 1600
#define MAX_DRAW_PACKET (48UL * 1024 * 1024)

static struct App {
    HAB anchor;
    HMQ queue;
    HWND frame, client;
    FILE *log;
    WcRaster *raster;
    uint32_t bitmap_width, bitmap_height;
    uint32_t started_at;
    uint32_t repaint_at;
    unsigned int codepage;
    uint32_t buttons;
    uint16_t keys[256];
    int inside, busy, ready, failed, needs_frame, repaint_scheduled;
    int pointer_x, pointer_y, pointer_modifier_mask;
    int reported_draw_error;
    unsigned long frames;
    uint32_t engine_capabilities;
    uint32_t committed_bytes, reclaimed_bytes;
    uint32_t next_memory_report;
} app;

static const WcPmConfig *app_config;
static const WcPmEngine *app_engine;
static int app_started;

const WcPmConfig *wc_pm_config(void)
{
    return app_config;
}

uint8_t *wc_pm_memory(size_t *size)
{
    if (app_engine) return app_engine->memory(size);
    *size = 0;
    return NULL;
}

HWND wc_gui_frame(void)
{
    return app.frame;
}

static void log_line(const char *stage, const char *detail)
{
    if (app.log) {
        fprintf(app.log, "%s%s%s\n", stage, detail ? ": " : "", detail ? detail : "");
        fflush(app.log);
    }
}

static void fail(const char *stage, const char *detail)
{
    char message[512];
    if (app.failed) return;
    app.failed = 1;
    (void)wc_print_finish(1);
    log_line(stage, detail);
    if (app.client) WinStopTimer(app.anchor, app.client, POINTER_TIMER);
    snprintf(message, sizeof(message), "%s\n\n%s", stage,
             detail ? detail : "The application could not continue.");
    message[sizeof(message) - 1] = 0;
    WinMessageBox(HWND_DESKTOP, app.frame, message, (PSZ)app_config->title, 0,
                  MB_OK | MB_ERROR | MB_MOVEABLE);
}

static uint32_t ticks(void)
{
    ULONG value = 0;
    DosQuerySysInfo(QSV_MS_COUNT, QSV_MS_COUNT, &value, sizeof(value));
    return value;
}

/* PM and the CRT can restore their own exception masks. Set the actual
 * x87 control word at each guest entry, including when CRT detection is off. */
static void wasm_float_mode(void);
#pragma aux wasm_float_mode = \
    "sub esp,4" \
    "fnclex" \
    "fnstcw word ptr [esp]" \
    "and word ptr [esp],0f0ffh" \
    "or word ptr [esp],023fh" \
    "fldcw word ptr [esp]" \
    "add esp,4" \
    modify exact [];

static const char *invoke(WcOperation op, const int32_t *values, uint32_t *result)
{
    const char *error;
    if (app.busy) return "Recursive engine invocation";
    app.busy = 1;
    wasm_float_mode();
    error = app_engine->call(op, values, result);
    app.busy = 0;
    if (error && wc_rng_error()) return wc_rng_error();
    return error;
}

static int dimensions(HWND window, int32_t *size)
{
    RECTL rect;
    if (!WinQueryWindowRect(window, &rect) || rect.xRight < 1 || rect.yTop < 1) return 0;
    size[0] = rect.xRight > MAX_VIEW_WIDTH ? MAX_VIEW_WIDTH : rect.xRight;
    size[1] = rect.yTop > MAX_VIEW_HEIGHT ? MAX_VIEW_HEIGHT : rect.yTop;
    return 1;
}

static void request_frame(void)
{
    /* A low-priority PM timer drains queued keyboard and mouse input first. */
    app.needs_frame = 1;
}

static void input_error(const char *message)
{
    log_line("Native input", message);
    app.busy = 1;
    WinMessageBox(HWND_DESKTOP, app.frame, message, (PSZ)app_config->title, 0,
                  MB_OK | MB_ERROR | MB_MOVEABLE);
    app.busy = 0;
}

static void cancel_input(void)
{
    int32_t args[2] = {0, 0};
    uint32_t result;
    const char *error;
    if (!app_config->cancel_input) return;
    error = invoke(WCR_ACCEPT_INPUT, args, &result);
    if (error) fail("Could not cancel the file request", error);
}

static void pick_input(uint32_t kind)
{
    char name[WC_GUI_PATH_CAP];
    unsigned char *memory;
    WcInput *input;
    uint32_t length, offset, result;
    int32_t args[2];
    size_t name_len, total, memory_size;
    const char *error;
    int picked;
    if (!app.ready || app.failed || app.busy) return;
    app.busy = 1;
    picked = wc_gui_pick_input(kind, name, &input, &length);
    app.busy = 0;
    if (picked <= 0) {
        cancel_input();
        request_frame();
        return;
    }
    name_len = strlen(name);
    total = name_len + length;
    args[0] = (int32_t)total;
    error = invoke(WCR_RESERVE_INPUT, args, &offset);
    if (error) {
        wc_input_discard(input);
        fail("Could not allocate input storage", error);
        return;
    }
    /* reserve_input can grow the guest memory. Reacquire its base and size. */
    memory = wc_pm_memory(&memory_size);
    if (!offset || !memory || offset > memory_size || total > memory_size - offset) {
        wc_input_discard(input);
        input_error("Could not allocate storage for the selected file.");
        cancel_input();
        request_frame();
        return;
    }
    memcpy(memory + offset, name, name_len);
    /* No PM calls or engine calls while a pointer into the engine is held.
     * Reading directly avoids a second, potentially 16 MiB native copy. */
    error = wc_input_read_close(input, memory + offset + name_len, length);
    if (error) {
        input_error(error);
        cancel_input();
        request_frame();
        return;
    }
    args[0] = (int32_t)name_len;
    args[1] = (int32_t)length;
    error = invoke(WCR_ACCEPT_INPUT, args, &result);
    if (error) { fail("Could not open the selected file", error); return; }
    if (result) {
        input_error("Could not accept the selected file.");
        cancel_input();
    } else {
        wc_gui_input_accepted(kind, name);
    }
    request_frame();
}

static void after_frame(HWND window)
{
    const char *error;
    uint32_t result;
    error = invoke(WCR_SHOULD_CLOSE, NULL, &result);
    if (error) { fail("Could not check the close request", error); return; }
    if (result) { WinPostMsg(window, WM_QUIT, 0, 0); return; }
    error = invoke(WCR_OPEN_REQUESTED, NULL, &result);
    if (error) { fail("Could not check the file request", error); return; }
    if (result) {
        if (result > 4) { fail("Could not open the file picker", "Unknown file request"); return; }
        if (!WinPostMsg(window, WM_WCG_PICK_INPUT, MPFROMLONG(result), 0)) {
            input_error("Could not open the file picker.");
            cancel_input();
        }
    }
}

static void close_window(HWND window)
{
    uint32_t result;
    const char *error;
    if (app.busy) return;
    if (!app.ready || app.failed) {
        WinPostMsg(window, WM_QUIT, 0, 0);
        return;
    }
    error = invoke(WCR_CLOSE_REQUEST, NULL, &result);
    if (error) { fail("Could not close the application", error); return; }
    if (result) WinPostMsg(window, WM_QUIT, 0, 0);
    else request_frame();
}

static int update_bitmap_geometry(void)
{
    const uint8_t *pixels;
    uint32_t width, height;
    size_t stride, row;
    pixels = wc_raster_pixels(app.raster, &width, &height, &stride);
    if (!pixels || !width || !height || width > MAX_VIEW_WIDTH ||
        height > MAX_VIEW_HEIGHT) return 0;
    row = ((size_t)width * 3 + 3) & ~(size_t)3;
    if (stride != row) return 0;
    app.bitmap_width = width;
    app.bitmap_height = height;
    return 1;
}

static void render_frame(HWND window)
{
    int32_t args[3];
    const char *error;
    uint32_t status, offset, length, frame_start, guest_ms, repaint_delay;
    size_t size;
    uint8_t *memory;
    int resized;
    char report[128];
    if (!app.ready || app.failed || app.busy || !WinIsWindowShowing(window)) return;
    if (!dimensions(window, args)) return;
    resized = app.bitmap_width != (uint32_t)args[0] || app.bitmap_height != (uint32_t)args[1];
    frame_start = ticks();
    if (!app.needs_frame && !resized &&
        (!app.repaint_scheduled || (int32_t)(frame_start - app.repaint_at) < 0)) return;
    app.needs_frame = 0;
    args[2] = (int32_t)(frame_start - app.started_at);
    error = invoke(WCR_FRAME, args, &status);
    guest_ms = ticks() - frame_start;
    if (error || status) {
        fail("The engine frame failed", error ? error : "The Rust UI rejected the frame");
        return;
    }
    error = invoke(WCR_DRAW_PTR, NULL, &offset);
    if (!error) error = invoke(WCR_DRAW_LEN, NULL, &length);
    if (error) { fail("Could not obtain draw data", error); return; }
    memory = wc_pm_memory(&size);
    if (!memory || length < 16 || length > MAX_DRAW_PACKET || offset > size || length > size - offset) {
        fail("Could not obtain draw data", "The draw packet is outside the engine memory");
        return;
    }
    if (!wc_raster_frame(app.raster, (uint32_t)args[0], (uint32_t)args[1],
                         memory + offset, length)) {
        fail("Could not rasterize the application", wc_raster_error(app.raster));
        return;
    }
    if (app.engine_capabilities & WC_ENGINE_DRAW_CONSUMED) {
        error = invoke(WCR_DRAW_CONSUMED, NULL, &status);
        if (error || status) {
            fail("Could not release the consumed draw packet", error ? error : "The engine rejected the release");
            return;
        }
    }
    if (!update_bitmap_geometry()) { fail("Could not display the application", "Invalid native bitmap"); return; }
    error = invoke(WCR_REPAINT_DELAY, NULL, &repaint_delay);
    if (error) { fail("Could not schedule the next frame", error); return; }
    WinInvalidateRect(window, NULL, FALSE);
    WinUpdateWindow(window);
    app.repaint_scheduled = repaint_delay < INT32_MAX;
    if (app.repaint_scheduled) app.repaint_at = ticks() + repaint_delay;
    if (++app.frames <= 3 || resized || guest_ms >= 2000) {
        snprintf(report, sizeof(report), "%lu, %ld x %ld, %lu bytes, engine %lu ms, total %lu ms",
                 app.frames, (long)args[0], (long)args[1], (unsigned long)length,
                 (unsigned long)guest_ms, (unsigned long)(ticks() - frame_start));
        log_line(app.frames <= 3 ? "Startup frame" : resized ? "Resize frame" : "Slow frame", report);
    }
    if (app.frames == 3) {
        snprintf(report, sizeof(report), "%lu bytes after startup", (unsigned long)size);
        log_line("Engine linear memory", report);
    }
    if (app.frames == 3 || resized) {
        WcRasterMemory allocations;
        wc_raster_memory(app.raster, &allocations);
        snprintf(report, sizeof(report), "surface %lu, vertices %lu, textures %lu bytes; no display copy",
                 (unsigned long)allocations.surface_bytes,
                 (unsigned long)allocations.vertex_bytes,
                 (unsigned long)allocations.texture_bytes);
        log_line("Host render allocations", report);
    }
    /* Query native page flags at most once per second during interaction;
     * keep the first startup frames and resize transitions observable. */
    if ((app.engine_capabilities & WC_ENGINE_MEMORY_STATS) &&
        (app.frames <= 3 || resized || (int32_t)(ticks() - app.next_memory_report) >= 0)) {
        uint32_t committed, reclaimed;
        error = invoke(WCR_MEMORY_COMMITTED, NULL, &committed);
        if (!error) error = invoke(WCR_MEMORY_RECLAIMED, NULL, &reclaimed);
        if (error) { fail("Could not query engine memory", error); return; }
        app.next_memory_report = ticks() + 1000;
        if (app.frames == 3 || committed != app.committed_bytes || reclaimed != app.reclaimed_bytes) {
            snprintf(report, sizeof(report), "%lu committed bytes, %lu decommitted unused bytes",
                     (unsigned long)committed, (unsigned long)reclaimed);
            log_line("Engine page accounting", report);
            app.committed_bytes = committed;
            app.reclaimed_bytes = reclaimed;
        }
    }
    /* Allow the engine to finish loading fonts over its first passes. */
    if (app.frames < 3) app.needs_frame = 1;
    if (wc_a11y_enabled()) {
        error = invoke(WCR_A11Y_PTR, NULL, &offset);
        if (!error) error = invoke(WCR_A11Y_LEN, NULL, &length);
        memory = wc_pm_memory(&size);
        if (!error && memory && length && offset <= size && length <= size - offset)
            wc_a11y_update(memory + offset, length);
    }
    after_frame(window);
}

static void paint(HWND window)
{
    RECTL invalid, area;
    HPS ps;
    BITMAPINFO2 info;
    const uint8_t *bitmap;
    uint32_t width, height;
    size_t stride;
    POINTL points[4];
    char report[64];
    ps = WinBeginPaint(window, NULLHANDLE, &invalid);
    if (!ps) return;
    bitmap = wc_raster_pixels(app.raster, &width, &height, &stride);
    if (!bitmap || !width || !height || !WinQueryWindowRect(window, &area)) {
        WinFillRect(ps, &invalid, SYSCLR_WINDOW);
    } else {
        memset(&info, 0, sizeof(info));
        info.cbFix = sizeof(BITMAPINFOHEADER2);
        info.cx = width;
        info.cy = height;
        info.cPlanes = 1;
        info.cBitCount = 24;
        /* Fill the resized client with the last frame while the application reflows
         * its controls and document at the new size. */
        points[0].x = area.xLeft;
        points[0].y = area.yBottom;
        points[1].x = area.xRight - 1;
        points[1].y = area.yTop - 1;
        points[2].x = 0;
        points[2].y = 0;
        points[3].x = width;
        points[3].y = height;
        if (GpiDrawBits(ps, (PBYTE)bitmap, &info, 4, points, ROP_SRCCOPY, BBO_IGNORE) == GPI_ERROR &&
            !app.reported_draw_error) {
            snprintf(report, sizeof(report), "PM error 0x%08lx", (unsigned long)WinGetLastError(app.anchor));
            log_line("GpiDrawBits", report);
            app.reported_draw_error = 1;
        }
    }
    WinEndPaint(ps);
}

static int mods_from_flags(USHORT flags)
{
    return ((flags & KC_SHIFT) ? 1 : 0) | ((flags & KC_CTRL) ? 2 : 0) |
           ((flags & KC_ALT) ? 4 : 0);
}

static int pointer_mods(void)
{
    return ((WinGetKeyState(HWND_DESKTOP, VK_SHIFT) & 0x8000) ? 1 : 0) |
           ((WinGetKeyState(HWND_DESKTOP, VK_CTRL) & 0x8000) ? 2 : 0) |
           ((WinGetKeyState(HWND_DESKTOP, VK_ALT) & 0x8000) ? 4 : 0);
}

static void send_event(int kind, int a, int b, int c, int mods)
{
    int32_t args[5];
    uint32_t result;
    const char *error;
    if (!app.ready || app.failed || app.busy) return;
    args[0] = kind;
    args[1] = a;
    args[2] = b;
    args[3] = c;
    args[4] = mods;
    error = invoke(WCR_EVENT, args, &result);
    if (error || result) fail("The application rejected the input", error ? error : "The Rust UI rejected the event");
    else request_frame();
}

static int32_t character(USHORT value)
{
    /* The Warp guest uses IBM CP850. ASCII remains available with any CP. */
    static const uint16_t cp850[128] = {
        0x00c7,0x00fc,0x00e9,0x00e2,0x00e4,0x00e0,0x00e5,0x00e7,
        0x00ea,0x00eb,0x00e8,0x00ef,0x00ee,0x00ec,0x00c4,0x00c5,
        0x00c9,0x00e6,0x00c6,0x00f4,0x00f6,0x00f2,0x00fb,0x00f9,
        0x00ff,0x00d6,0x00dc,0x00f8,0x00a3,0x00d8,0x00d7,0x0192,
        0x00e1,0x00ed,0x00f3,0x00fa,0x00f1,0x00d1,0x00aa,0x00ba,
        0x00bf,0x00ae,0x00ac,0x00bd,0x00bc,0x00a1,0x00ab,0x00bb,
        0x2591,0x2592,0x2593,0x2502,0x2524,0x00c1,0x00c2,0x00c0,
        0x00a9,0x2563,0x2551,0x2557,0x255d,0x00a2,0x00a5,0x2510,
        0x2514,0x2534,0x252c,0x251c,0x2500,0x253c,0x00e3,0x00c3,
        0x255a,0x2554,0x2569,0x2566,0x2560,0x2550,0x256c,0x00a4,
        0x00f0,0x00d0,0x00ca,0x00cb,0x00c8,0x0131,0x00cd,0x00ce,
        0x00cf,0x2518,0x250c,0x2588,0x2584,0x00a6,0x00cc,0x2580,
        0x00d3,0x00df,0x00d4,0x00d2,0x00f5,0x00d5,0x00b5,0x00fe,
        0x00de,0x00da,0x00db,0x00d9,0x00fd,0x00dd,0x00af,0x00b4,
        0x00ad,0x00b1,0x2017,0x00be,0x00b6,0x00a7,0x00f7,0x00b8,
        0x00b0,0x00a8,0x00b7,0x00b9,0x00b3,0x00b2,0x25a0,0x00a0
    };
    if (value >= 32 && value < 127) return value;
    if (value >= 128 && value < 256 && app.codepage == 850) return cp850[value - 128];
    return 0;
}

static int virtual_key(USHORT vk)
{
    if (vk >= VK_F1 && vk <= VK_F12) return 288 + vk - VK_F1;
    switch (vk) {
    case VK_BACKSPACE: return 8;
    case VK_TAB: case VK_BACKTAB: return 9;
    case VK_ENTER: case VK_NEWLINE: return 13;
    case VK_ESC: return 27;
    case VK_SPACE: return 32;
    case VK_DELETE: return 127;
    case VK_LEFT: return 256;
    case VK_RIGHT: return 257;
    case VK_UP: return 258;
    case VK_DOWN: return 259;
    case VK_HOME: return 260;
    case VK_END: return 261;
    case VK_PAGEUP: return 262;
    case VK_PAGEDOWN: return 263;
    case VK_INSERT: return 264;
    }
    return 0;
}

static int control_letter(unsigned int scan)
{
    /* PM may suppress KC_CHAR for control chords. These are PC scan codes. */
    static const char top[] = "QWERTYUIOP";
    static const char middle[] = "ASDFGHJKL";
    static const char bottom[] = "ZXCVBNM";
    if (scan >= 0x10 && scan <= 0x19) return top[scan - 0x10];
    if (scan >= 0x1e && scan <= 0x26) return middle[scan - 0x1e];
    if (scan >= 0x2c && scan <= 0x32) return bottom[scan - 0x2c];
    return 0;
}

static void keyboard(MPARAM mp1, MPARAM mp2)
{
    USHORT flags = SHORT1FROMMP(mp1), ch = SHORT1FROMMP(mp2);
    unsigned int scan = CHAR4FROMMP(mp1), count = CHAR3FROMMP(mp1);
    int pressed = !(flags & KC_KEYUP), mods = mods_from_flags(flags);
    int key = (flags & KC_VIRTUALKEY) ? virtual_key(SHORT2FROMMP(mp2)) : 0;
    int32_t codepoint;
    const char *error;
    uint32_t result;
    if (!app.ready || app.failed || app.busy) return;
    if (flags & (KC_DEADKEY | KC_INVALIDCOMP | KC_INVALIDCHAR)) return;
    if (!key && (flags & KC_CHAR)) {
        if ((mods & 2) && ch >= 1 && ch <= 26) key = 'A' + ch - 1;
        else if (ch >= 'a' && ch <= 'z') key = ch - 'a' + 'A';
        else if (ch >= 32 && ch < 127) key = ch;
    }
    if (!key && (mods & 2) && (flags & KC_SCANCODE)) key = control_letter(scan);
    if (!pressed && scan) {
        if (app.keys[scan]) key = app.keys[scan];
        app.keys[scan] = 0;
    } else if (pressed && scan && key) app.keys[scan] = (uint16_t)key;
    if (key) send_event(2, key, pressed, (flags & KC_PREVDOWN) != 0, mods);
    if (!pressed || (mods & 6) || !(flags & KC_CHAR) || app.failed) return;
    codepoint = character(ch);
    if (!codepoint) return;
    if (!count) count = 1;
    if (count > 32) count = 32;
    while (count--) {
        error = invoke(WCR_TEXT, &codepoint, &result);
        if (error || result) {
            fail("The application rejected the text input", error ? error : "The Rust UI rejected the character");
            return;
        }
    }
    request_frame();
}

static void pointer(HWND window, MPARAM mp1, uint32_t button, int pressed)
{
    RECTL area;
    int32_t size[2];
    int x, y, mods;
    if (!app.ready || app.failed || app.busy || !WinQueryWindowRect(window, &area)) return;
    /* The scaled preview has no hit targets at the new size until reflow.
     * Still deliver button releases so a resize cannot leave a drag active. */
    if (button && pressed &&
        (!dimensions(window, size) || app.bitmap_width != (uint32_t)size[0] ||
         app.bitmap_height != (uint32_t)size[1])) return;
    if (button) {
        if (pressed) {
            app.buttons |= button;
            WinSetFocus(HWND_DESKTOP, window);
            WinSetCapture(HWND_DESKTOP, window);
        } else {
            app.buttons &= ~button;
            if (!app.buttons) WinSetCapture(HWND_DESKTOP, NULLHANDLE);
        }
    }
    x = (SHORT)SHORT1FROMMP(mp1);
    y = area.yTop - 1 - (SHORT)SHORT2FROMMP(mp1);
    if (area.xRight > MAX_VIEW_WIDTH) x = x * MAX_VIEW_WIDTH / area.xRight;
    if (area.yTop > MAX_VIEW_HEIGHT) y = y * MAX_VIEW_HEIGHT / area.yTop;
    mods = pointer_mods();
    if (!button && app.inside && x == app.pointer_x && y == app.pointer_y &&
        mods == app.pointer_modifier_mask) return;
    app.pointer_x = x;
    app.pointer_y = y;
    app.pointer_modifier_mask = mods;
    app.inside = 1;
    send_event(1, x, y, (int)app.buttons, mods);
}

static void check_pointer(HWND window)
{
    POINTL p;
    RECTL area;
    if (!app.ready || !app.inside || app.buttons || app.busy) return;
    if (WinQueryPointerPos(HWND_DESKTOP, &p) &&
        WinMapWindowPoints(HWND_DESKTOP, window, &p, 1) &&
        WinQueryWindowRect(window, &area) &&
        (p.x < 0 || p.y < 0 || p.x >= area.xRight || p.y >= area.yTop)) {
        app.inside = 0;
        send_event(4, 0, 0, 0, pointer_mods());
    }
}

static void start(HWND window)
{
    int32_t size[2];
    uint32_t status;
    const char *error;
    if (!dimensions(window, size)) { fail("Could not start the application", "The client window has no size"); return; }
    log_line("Initializing native random source", "jitterentropy 3.7.0, OSR 3, health tests enabled");
    error = wc_rng_prepare();
    if (error) { fail("Could not initialize the native random source", error); return; }
    log_line("Native random source", "ready; ENTROPY.BIN is not required");
    log_line("Starting linked engine", NULL);
    app.busy = 1;
    wasm_float_mode();
    error = app_engine->start();
    app.busy = 0;
    if (error) { fail("Could not load the application engine", error); return; }
    app.raster = wc_raster_create();
    if (!app.raster) { fail("Could not start the application", "Raster allocation failed"); return; }
    log_line("Initializing upstream application", NULL);
    error = invoke(WCR_INIT, size, &status);
    if (error || status) {
        fail("Could not initialize the application engine", error ? error : "The Rust UI returned an error");
        return;
    }
    /* Original ABI1 EXEs report an unsupported operation here. They remain
     * compatible and simply keep their old draw-packet lifetime policy. */
    status = 0;
    error = invoke(WCR_CAPABILITIES, NULL, &status);
    app.engine_capabilities = error ? 0 : status;
    log_line("Draw packet lifetime", app.engine_capabilities & WC_ENGINE_DRAW_CONSUMED ?
             "released after native rendering" : "legacy engine");
    app.started_at = ticks();
    app.ready = 1;
    if (!WinStartTimer(app.anchor, window, POINTER_TIMER, 250)) {
        fail("Could not start the application", "Presentation Manager could not start the frame timer");
        return;
    }
    send_event(5, WinQueryFocus(HWND_DESKTOP) == window, 0, 0, 0);
    request_frame();
}

static MRESULT EXPENTRY client_proc(HWND window, ULONG msg, MPARAM mp1, MPARAM mp2)
{
    USHORT action;
    int delta;
    MRESULT result;
    switch (msg) {
    case WM_WCG_START: start(window); return 0;
    case WM_WCG_PICK_INPUT: pick_input((uint32_t)mp1); return 0;
    case WM_PAINT: paint(window); return 0;
    case WM_ERASEBACKGROUND: return MRFROMSHORT(TRUE);
    case WM_SIZE:
        result = WinDefWindowProc(window, msg, mp1, mp2);
        request_frame();
        if (app.bitmap_width && app.bitmap_height && WinIsWindowShowing(window)) {
            WinInvalidateRect(window, NULL, FALSE);
            WinUpdateWindow(window);
        }
        return result;
    case WM_CHAR:
        if ((SHORT1FROMMP(mp1) & (KC_ALT | KC_VIRTUALKEY | KC_KEYUP)) ==
            (KC_ALT | KC_VIRTUALKEY) && SHORT2FROMMP(mp2) == VK_F4) {
            WinPostMsg(window, WM_CLOSE, 0, 0);
        } else if (app.ready && !app.failed && !app.busy &&
                   wc_a11y_key(app.anchor, app.frame, window, mp1, mp2, send_event)) {
            return 0;
        } else keyboard(mp1, mp2);
        return 0;
    case WM_MOUSEMOVE: pointer(window, mp1, 0, 0); break;
    case WM_BUTTON1DOWN: case WM_BUTTON1DBLCLK: pointer(window, mp1, 1, 1); return 0;
    case WM_BUTTON1UP: pointer(window, mp1, 1, 0); return 0;
    case WM_BUTTON2DOWN: case WM_BUTTON2DBLCLK: pointer(window, mp1, 2, 1); return 0;
    case WM_BUTTON2UP: pointer(window, mp1, 2, 0); return 0;
    case WM_BUTTON3DOWN: case WM_BUTTON3DBLCLK: pointer(window, mp1, 4, 1); return 0;
    case WM_BUTTON3UP: pointer(window, mp1, 4, 0); return 0;
    case WM_HSCROLL: case WM_VSCROLL:
        action = SHORT2FROMMP(mp2);
        delta = action == SB_LINEUP ? 48 : action == SB_LINEDOWN ? -48 :
                action == SB_PAGEUP ? 240 : action == SB_PAGEDOWN ? -240 : 0;
        if (delta) send_event(3, msg == WM_HSCROLL ? delta : 0,
                              msg == WM_VSCROLL ? delta : 0, 0, pointer_mods());
        return 0;
    case WM_SETFOCUS:
        wc_a11y_focus(SHORT1FROMMP(mp2) != 0);
        if (!SHORT1FROMMP(mp2)) {
            memset(app.keys, 0, sizeof(app.keys));
            if (app.buttons) WinSetCapture(HWND_DESKTOP, NULLHANDLE);
            app.buttons = 0;
        }
        send_event(5, SHORT1FROMMP(mp2) != 0, 0, 0, 0);
        break;
    case WM_TIMER:
        if (SHORT1FROMMP(mp1) == POINTER_TIMER) {
            check_pointer(window);
            if (wc_network_ready()) request_frame();
            render_frame(window);
        }
        return 0;
    case WM_CLOSE:
        close_window(window);
        return 0;
    }
    return WinDefWindowProc(window, msg, mp1, mp2);
}

static int run_app(const WcPmConfig *config, const WcPmEngine *engine)
{
    QMSG message;
    ULONG flags = FCF_TITLEBAR | FCF_SYSMENU | FCF_SIZEBORDER | FCF_MINMAX | FCF_TASKLIST | FCF_ICON;
    LONG screen_w, screen_h, width, height;
    int exit_code = 1;
    if (app_started || !config || !engine || !engine->start || !engine->shutdown ||
        !engine->call || !engine->memory)
        return 1;
    app_started = 1;
    app_config = config;
    app_engine = engine;
    app.log = fopen(app_config->gui_log, "wt");
    app.anchor = WinInitialize(0);
    if (!app.anchor) { log_line("WinInitialize failed", NULL); goto done; }
    app.queue = WinCreateMsgQueue(app.anchor, 0);
    if (!app.queue) { log_line("WinCreateMsgQueue failed", NULL); goto done; }
    app.codepage = WinQueryCp(app.queue);
    if (!WinRegisterClass(app.anchor, "CraftGuiClient", client_proc, CS_SIZEREDRAW, 0)) {
        log_line("WinRegisterClass failed", NULL);
        goto done;
    }
    app.frame = WinCreateStdWindow(HWND_DESKTOP, 0, &flags, "CraftGuiClient",
                                   (PSZ)app_config->title, 0, (HMODULE)app_config->icon_module,
                                   WC_APP_ICON, &app.client);
    if (!app.frame) { log_line("WinCreateStdWindow failed", NULL); goto done; }
    screen_w = WinQuerySysValue(HWND_DESKTOP, SV_CXSCREEN);
    screen_h = WinQuerySysValue(HWND_DESKTOP, SV_CYSCREEN);
    width = screen_w * 9 / 10;
    height = screen_h * 9 / 10;
    if (!WinSetWindowPos(app.frame, HWND_TOP, (screen_w - width) / 2,
                         (screen_h - height) / 2, width, height,
                         SWP_MOVE | SWP_SIZE | SWP_SHOW | SWP_ACTIVATE)) {
        log_line("WinSetWindowPos failed", NULL);
        goto done;
    }
    WinSetFocus(HWND_DESKTOP, app.client);
    if (!WinPostMsg(app.client, WM_WCG_START, 0, 0)) {
        log_line("Could not post application startup", NULL);
        goto done;
    }
    while (WinGetMsg(app.anchor, &message, NULLHANDLE, 0, 0))
        WinDispatchMsg(app.anchor, &message);
    exit_code = app.failed ? 1 : 0;
done:
    app.ready = 0;
    if (app.client) WinStopTimer(app.anchor, app.client, POINTER_TIMER);
    wc_a11y_shutdown();
    wc_network_shutdown();
    (void)wc_print_finish(1);
    if (app.frame) WinDestroyWindow(app.frame);
    if (app.queue) WinDestroyMsgQueue(app.queue);
    if (app.anchor) WinTerminate(app.anchor);
    if (app.raster) wc_raster_destroy(app.raster);
    app_engine->shutdown();
    wc_guest_services_shutdown();
    if (app.log) fclose(app.log);
    app_config = NULL;
    app_engine = NULL;
    return exit_code;
}

const WcPmApi *wc_pm_get_api(uint32_t version)
{
    static const WcPmApi api = {
        run_app,
        wc_wasi_empty_sizes, wc_wasi_random_get, wc_wasi_clock_time, wc_wasi_clock_res,
        wc_wasi_fd_write, wc_wasi_fd_read, wc_wasi_fdstat, wc_wasi_close, wc_wasi_seek,
        wc_wasi_yield, wc_host_save,
        wc_net_socket, wc_net_open_url, wc_net_release,
        wc_print_queues, wc_print_start, wc_print_page, wc_print_finish,
        wc_clipboard_read, wc_clipboard_write, wc_clipboard_image
    };
    return version == WC_PM_ABI ? &api : NULL;
}
