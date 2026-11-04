/* OS/2 implementations of the linked engine's host calls. */
#define INCL_DOS
#include <os2.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "guest_services.h"
#include "gui_files.h"
#include "craftpm.h"
#include "rng.h"

#define WASI_BADF 8U
#define WASI_FAULT 21U
#define WASI_INVAL 28U
#define WASI_IO 29U
#define WASI_NOSYS 52U
#define WASI_SPIPE 70U

static uint8_t *memory_span(uint32_t offset, uint32_t length)
{
    size_t extent = 0;
    uint8_t *memory = wc_pm_memory(&extent);
    if (!memory || offset > extent || length > extent - offset) return NULL;
    return memory + offset;
}

/* The engine and OS/2 host both store integers in little-endian order. */
static uint32_t read32(const uint8_t *span)
{
    uint32_t value;
    memcpy(&value, span, sizeof(value));
    return value;
}

static void write32(uint8_t *span, uint32_t value)
{
    memcpy(span, &value, sizeof(value));
}

static void write64(uint8_t *span, uint64_t value)
{
    memcpy(span, &value, sizeof(value));
}

uint32_t wc_wasi_empty_sizes(uint32_t count, uint32_t size)
{
    uint8_t *count_span = memory_span(count, 4), *size_span = memory_span(size, 4);
    if (!count_span || !size_span) return WASI_FAULT;
    write32(count_span, 0);
    write32(size_span, 0);
    return 0;
}

uint32_t wc_wasi_random_get(uint32_t data, uint32_t size)
{
    uint8_t *bytes;
    if (!size) return 0;
    bytes = memory_span(data, size);
    if (!bytes) return WASI_FAULT;
    return wc_rng_fill(bytes, size) ? 0 : WASI_IO;
}

uint32_t wc_wasi_clock_time(uint32_t id, uint32_t result)
{
    uint8_t *output = memory_span(result, 8);
    ULONG ms;
    time_t now;
    uint64_t ns;
    static uint32_t previous_ms;
    static uint64_t overflow_ms;
    if (!output) return WASI_FAULT;
    if (id == 0) {
        now = time(NULL);
        if (now == (time_t)-1) return WASI_IO;
        ns = (uint64_t)now * UINT64_C(1000000000);
    } else if (id == 1) {
        if (DosQuerySysInfo(QSV_MS_COUNT, QSV_MS_COUNT, &ms, sizeof(ms)) != 0)
            return WASI_IO;
        if ((uint32_t)ms < previous_ms) overflow_ms += UINT64_C(4294967296);
        previous_ms = (uint32_t)ms;
        ns = (overflow_ms + previous_ms) * UINT64_C(1000000);
    } else {
        return WASI_INVAL;
    }
    write64(output, ns);
    return 0;
}

uint32_t wc_wasi_clock_res(uint32_t id, uint32_t result)
{
    uint8_t *output = memory_span(result, 8);
    if (id > 1) return WASI_INVAL;
    if (!output) return WASI_FAULT;
    write64(output, id == 0 ? UINT64_C(1000000000) : UINT64_C(1000000));
    return 0;
}

uint32_t wc_wasi_fd_write(uint32_t fd, uint32_t vectors, uint32_t count, uint32_t result)
{
    uint32_t i, size, total = 0;
    uint8_t *entries, *output, *bytes;
    FILE *log;
    int failed = 0;
    if (fd != 1 && fd != 2) return WASI_BADF;
    if (count > 1024) return WASI_INVAL;
    entries = memory_span(vectors, count * 8);
    output = memory_span(result, 4);
    if (!entries || !output) return WASI_FAULT;
    for (i = 0; i < count; ++i) {
        size = read32(entries + i * 8 + 4);
        if (size > UINT32_MAX - total) return WASI_INVAL;
        if (size && !memory_span(read32(entries + i * 8), size)) return WASI_FAULT;
        total += size;
    }
    if (total) {
        log = fopen(wc_pm_config()->engine_log, "ab");
        if (!log) return WASI_IO;
        for (i = 0; i < count; ++i) {
            size = read32(entries + i * 8 + 4);
            if (!size) continue;
            bytes = memory_span(read32(entries + i * 8), size);
            if (fwrite(bytes, 1, size, log) != size) { failed = 1; break; }
        }
        if (fclose(log) != 0 || failed) return WASI_IO;
    }
    write32(output, total);
    return 0;
}

uint32_t wc_wasi_fd_read(uint32_t fd, uint32_t result)
{
    uint8_t *output = memory_span(result, 4);
    if (fd != 0) return WASI_BADF;
    if (!output) return WASI_FAULT;
    write32(output, 0);
    return 0;
}

uint32_t wc_wasi_fdstat(uint32_t fd, uint32_t result)
{
    uint8_t *output = memory_span(result, 24);
    if (fd > 2) return WASI_BADF;
    if (!output) return WASI_FAULT;
    memset(output, 0, 24);
    output[0] = 2;
    write64(output + 8, fd == 0 ? 2 : 64);
    return 0;
}

uint32_t wc_wasi_close(uint32_t fd) { return fd < 3 ? 0 : WASI_BADF; }
uint32_t wc_wasi_seek(uint32_t fd) { return fd < 3 ? WASI_SPIPE : WASI_BADF; }

uint32_t wc_wasi_yield(void)
{
    DosSleep(0);
    return 0;
}

int32_t wc_host_save(uint32_t name, uint32_t name_size, uint32_t data, uint32_t data_size,
                     uint32_t save_as, uint32_t output, uint32_t output_size)
{
    const uint8_t *name_bytes = memory_span(name, name_size);
    const uint8_t *data_bytes = memory_span(data, data_size);
    uint8_t *output_bytes = memory_span(output, output_size);
    if (!name_bytes || !data_bytes || !output_bytes) return -2;
    return wc_gui_save((const char *)name_bytes, name_size, data_bytes, data_size,
                       save_as, (char *)output_bytes, output_size);
}

void wc_guest_services_shutdown(void)
{
    wc_rng_shutdown();
}
