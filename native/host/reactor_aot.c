/* Calls into the linked native translation of the application's Rust engine. */
#include "craftpm.h"
#include "word.h"
#include "wasm-rt-impl.h"
#include "wasm-rt-exceptions.h"

struct w2c_wasi__snapshot__preview1 { int unused; };
struct w2c_wc__host { int unused; };

static struct w2c_wasi__snapshot__preview1 wasi;
static struct w2c_wc__host host;
static w2c_word engine;
static int initialized;
static int exited;

const char *wc_reactor_start(void)
{
    wasm_rt_trap_t trap;
    if (initialized) return "application engine is already running";
    wasm_rt_init();
    initialized = 1;
    trap = wasm_rt_impl_try();
    if (trap) return wasm_rt_strerror(trap);
    wasm2c_word_instantiate(&engine, &wasi, &host);
    return NULL;
}

void wc_reactor_shutdown(void)
{
    if (initialized) {
        wasm2c_word_free(&engine);
        wasm_rt_free();
        initialized = 0;
    }
}

uint8_t *wc_reactor_memory(size_t *size)
{
    wasm_rt_memory_t *memory = w2c_word_memory(&engine);
    *size = memory->size;
    return memory->data;
}

const char *wc_reactor_call(WcOperation op, const int32_t *a, uint32_t *result)
{
    wasm_rt_trap_t trap;
    if (!initialized) return "application engine is unavailable";
    exited = 0;
    trap = wasm_rt_impl_try();
    if (trap) return exited ? "The application engine exited" : wasm_rt_strerror(trap);
    switch (op) {
    case WCR_INIT: *result = w2c_word_wc_init(&engine, a[0], a[1]); break;
    case WCR_FRAME: *result = w2c_word_wc_frame(&engine, a[0], a[1], a[2]); break;
    case WCR_EVENT: *result = w2c_word_wc_event(&engine, a[0], a[1], a[2], a[3], a[4]); break;
    case WCR_TEXT: *result = w2c_word_wc_text(&engine, a[0]); break;
    case WCR_DRAW_PTR: *result = w2c_word_wc_draw_ptr(&engine); break;
    case WCR_DRAW_LEN: *result = w2c_word_wc_draw_len(&engine); break;
    case WCR_OPEN_REQUESTED: *result = w2c_word_wc_open_requested(&engine); break;
    case WCR_RESERVE_INPUT: *result = w2c_word_wc_reserve_input(&engine, a[0]); break;
    case WCR_ACCEPT_INPUT: *result = w2c_word_wc_accept_input(&engine, a[0], a[1]); break;
    case WCR_CLOSE_REQUEST: *result = w2c_word_wc_close_request(&engine); break;
    case WCR_SHOULD_CLOSE: *result = w2c_word_wc_should_close(&engine); break;
    case WCR_A11Y_PTR: *result = w2c_word_wc_a11y_ptr(&engine); break;
    case WCR_A11Y_LEN: *result = w2c_word_wc_a11y_len(&engine); break;
    case WCR_REPAINT_DELAY: *result = w2c_word_wc_repaint_delay_ms(&engine); break;
    default: return "Unknown application engine operation";
    }
    return NULL;
}

void wc_reactor_exit(uint32_t code)
{
    (void)code;
    exited = 1;
    wasm_rt_trap(WASM_RT_TRAP_UNREACHABLE);
}

typedef struct w2c_wasi__snapshot__preview1 Wasi;
#define WASI(name) w2c_wasi__snapshot__preview1_##name

u32 WASI(clock_time_get)(Wasi *context, u32 id, u64 precision, u32 output)
{
    (void)context;
    (void)precision;
    return wc_pm->wc_wasi_clock_time(id, output);
}

u32 WASI(clock_res_get)(Wasi *context, u32 id, u32 output)
{
    (void)context;
    return wc_pm->wc_wasi_clock_res(id, output);
}

u32 WASI(environ_get)(Wasi *context, u32 output, u32 data)
{
    (void)context; (void)output; (void)data;
    return 0;
}

u32 WASI(environ_sizes_get)(Wasi *context, u32 count, u32 size)
{
    (void)context;
    return wc_pm->wc_wasi_empty_sizes(count, size);
}

u32 WASI(fd_close)(Wasi *context, u32 fd)
{
    (void)context;
    return wc_pm->wc_wasi_close(fd);
}

u32 WASI(fd_fdstat_get)(Wasi *context, u32 fd, u32 output)
{
    (void)context;
    return wc_pm->wc_wasi_fdstat(fd, output);
}

u32 WASI(fd_read)(Wasi *context, u32 fd, u32 vectors, u32 count, u32 output)
{
    (void)context; (void)vectors; (void)count;
    return wc_pm->wc_wasi_fd_read(fd, output);
}

u32 WASI(fd_seek)(Wasi *context, u32 fd, u64 offset, u32 whence, u32 output)
{
    (void)context; (void)offset; (void)whence; (void)output;
    return wc_pm->wc_wasi_seek(fd);
}

u32 WASI(fd_write)(Wasi *context, u32 fd, u32 vectors, u32 count, u32 output)
{
    (void)context;
    return wc_pm->wc_wasi_fd_write(fd, vectors, count, output);
}

u32 WASI(random_get)(Wasi *context, u32 data, u32 size)
{
    (void)context;
    return wc_pm->wc_wasi_random_get(data, size);
}

u32 WASI(sched_yield)(Wasi *context)
{
    (void)context;
    return wc_pm->wc_wasi_yield();
}

void WASI(proc_exit)(Wasi *context, u32 code)
{
    (void)context;
    wc_reactor_exit(code);
}

u32 w2c_wc__host_save(struct w2c_wc__host *context, u32 name, u32 name_size,
                      u32 data, u32 data_size, u32 save_as, u32 output, u32 output_size)
{
    (void)context;
    return (u32)wc_pm->wc_host_save(name, name_size, data, data_size, save_as, output, output_size);
}

u32 w2c_wc__host_net_socket(struct w2c_wc__host *context, u32 operation, u32 address, u32 length)
{
    (void)context;
    return (u32)wc_pm->wc_net_socket(operation, address, length);
}

void w2c_wc__host_open_url_native(struct w2c_wc__host *context, u32 address, u32 length)
{
    (void)context;
    wc_pm->wc_net_open_url(address, length);
}

u32 w2c_wc__host_net_release(struct w2c_wc__host *context, u32 operation, u32 address, u32 length)
{
    (void)context;
    return (u32)wc_pm->wc_net_release(operation, address, length);
}

u32 w2c_wc__host_clipboard_read(struct w2c_wc__host *context, u32 kind, u32 offset, u32 capacity)
{
    (void)context;
    return wc_pm->wc_clipboard_read(kind, offset, capacity);
}

u32 w2c_wc__host_clipboard_write(struct w2c_wc__host *context, u32 text, u32 text_len,
                                  u32 html, u32 html_len, u32 math, u32 math_len)
{
    (void)context;
    return wc_pm->wc_clipboard_write(text, text_len, html, html_len, math, math_len);
}

u32 w2c_wc__host_clipboard_image(struct w2c_wc__host *context, u32 offset, u32 width, u32 height)
{
    (void)context;
    return wc_pm->wc_clipboard_image(offset, width, height);
}

/* Host filesystem access is available only through the native file picker. */
#define WASI_BADF 8U
u32 WASI(fd_filestat_get)(Wasi *w, u32 a, u32 b)
{ (void)w; (void)a; (void)b; return WASI_BADF; }

u32 WASI(fd_prestat_get)(Wasi *w, u32 a, u32 b)
{ (void)w; (void)a; (void)b; return WASI_BADF; }

u32 WASI(fd_prestat_dir_name)(Wasi *w, u32 a, u32 b, u32 c)
{ (void)w; (void)a; (void)b; (void)c; return WASI_BADF; }

u32 WASI(fd_readdir)(Wasi *w, u32 a, u32 b, u32 c, u64 d, u32 e)
{ (void)w; (void)a; (void)b; (void)c; (void)d; (void)e; return WASI_BADF; }

u32 WASI(path_create_directory)(Wasi *w, u32 a, u32 b, u32 c)
{ (void)w; (void)a; (void)b; (void)c; return WASI_BADF; }

u32 WASI(path_filestat_get)(Wasi *w, u32 a, u32 b, u32 c, u32 d, u32 e)
{ (void)w; (void)a; (void)b; (void)c; (void)d; (void)e; return WASI_BADF; }

u32 WASI(path_open)(Wasi *w, u32 a, u32 b, u32 c, u32 d, u32 e, u64 f, u64 g, u32 h, u32 i)
{ (void)w; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i; return WASI_BADF; }

u32 WASI(path_readlink)(Wasi *w, u32 a, u32 b, u32 c, u32 d, u32 e, u32 f)
{ (void)w; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; return WASI_BADF; }

u32 WASI(path_rename)(Wasi *w, u32 a, u32 b, u32 c, u32 d, u32 e, u32 f)
{ (void)w; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; return WASI_BADF; }

u32 WASI(path_unlink_file)(Wasi *w, u32 a, u32 b, u32 c)
{ (void)w; (void)a; (void)b; (void)c; return WASI_BADF; }

#if defined(WC_DECKCRAFT)
u32 WASI(fd_sync)(Wasi *w, u32 fd)
{ (void)w; (void)fd; return WASI_BADF; }

u32 WASI(path_remove_directory)(Wasi *w, u32 fd, u32 path, u32 length)
{ (void)w; (void)fd; (void)path; (void)length; return WASI_BADF; }
#endif

static uint8_t *print_span(u32 offset, u32 length)
{
    size_t size;
    uint8_t *memory = wc_reactor_memory(&size);
    if (!memory || offset > size || length > size - offset) return NULL;
    return memory + offset;
}

u32 w2c_wc__host_print_queues(struct w2c_wc__host *context, u32 offset, u32 capacity)
{
    uint8_t *output = print_span(offset, capacity);
    (void)context;
    return output ? (u32)wc_pm->wc_print_queues(output, capacity) : (u32)-1;
}

u32 w2c_wc__host_print_start(struct w2c_wc__host *context, u32 offset, u32 length,
                              u32 orientation, u32 paper, u32 duplex, u32 gray)
{
    const uint8_t *name = print_span(offset, length);
    (void)context;
    return name ? (u32)wc_pm->wc_print_start((const char *)name, length, orientation, paper, duplex, gray) : (u32)-1;
}

u32 w2c_wc__host_print_page(struct w2c_wc__host *context, u32 offset, u32 width, u32 height,
                             u32 width_millipoints, u32 height_millipoints)
{
    const uint8_t *pixels;
    (void)context;
    if (offset == 0 && width == 0 && height == 0) return (u32)wc_pm->wc_print_page(NULL, 0, 0, 0, 0);
    if (!width || !height || width > 2001 || height > 2001) return (u32)-1;
    pixels = print_span(offset, width * height * 4);
    return pixels ? (u32)wc_pm->wc_print_page(pixels, width, height, width_millipoints, height_millipoints) : (u32)-1;
}

u32 w2c_wc__host_print_finish(struct w2c_wc__host *context, u32 cancel)
{
    (void)context;
    return (u32)wc_pm->wc_print_finish(cancel != 0);
}
