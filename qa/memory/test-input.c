/* Tests the actual direct-input module with deterministic I/O fault injection.
 * Build as one translation unit: wrappers affect input.c only, not the test's
 * own fixture or destination allocation. MIT OR Apache-2.0.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../native/host/input.h"

/* The managed Linux test host has no /proc task inventory. Keep address and
 * undefined-behavior checks enabled; our counters verify module resource
 * cleanup explicitly. This hook is used only by LeakSanitizer. */
int __lsan_is_turned_off(void) { return 1; }

enum Fault { NORMAL, FAIL_OPEN, FAIL_ALLOC, SHORT_READ, FAIL_READ,
             FAIL_GETC, FAIL_ERROR, FAIL_CLOSE };
static enum Fault fault;
static unsigned int opened, closed, read_calls, getc_calls;
static unsigned int allocation_calls, live_allocations;
static size_t allocated_live, allocated_peak, largest_request;

static FILE *test_fopen(const char *path, const char *mode);
static void *test_malloc(size_t size);
static void test_free(void *memory);
static size_t test_fread(void *data, size_t size, size_t count, FILE *stream);
static int test_fgetc(FILE *stream);
static int test_ferror(FILE *stream);
static int test_fclose(FILE *stream);

#define fopen test_fopen
#define malloc test_malloc
#define free test_free
#define fread test_fread
#define fgetc test_fgetc
#define ferror test_ferror
#define fclose test_fclose
#include "../../native/host/input.c"
#undef fopen
#undef malloc
#undef free
#undef fread
#undef fgetc
#undef ferror
#undef fclose

static FILE *test_fopen(const char *path, const char *mode)
{
    FILE *stream;
    if (fault == FAIL_OPEN) return NULL;
    stream = fopen(path, mode);
    if (stream) ++opened;
    return stream;
}

static void *test_malloc(size_t size)
{
    void *memory;
    ++allocation_calls;
    if (size > largest_request) largest_request = size;
    if (fault == FAIL_ALLOC) return NULL;
    memory = malloc(size);
    if (memory) {
        ++live_allocations;
        allocated_live += size;
        if (allocated_live > allocated_peak) allocated_peak = allocated_live;
    }
    return memory;
}

static void test_free(void *memory)
{
    assert(memory && live_allocations == 1);
    --live_allocations;
    assert(allocated_live == sizeof(WcInput));
    allocated_live -= sizeof(WcInput);
    free(memory);
}

static size_t test_fread(void *data, size_t size, size_t count, FILE *stream)
{
    ++read_calls;
    if (fault == FAIL_READ) return 0;
    if (fault == SHORT_READ) return fread(data, size, count / 2, stream);
    return fread(data, size, count, stream);
}

static int test_fgetc(FILE *stream)
{
    ++getc_calls;
    return fault == FAIL_GETC ? EOF : fgetc(stream);
}

static int test_ferror(FILE *stream)
{
    if (fault == FAIL_READ || fault == FAIL_GETC || fault == FAIL_ERROR) return 1;
    return ferror(stream);
}

static int test_fclose(FILE *stream)
{
    int result = fclose(stream);
    ++closed;
    return fault == FAIL_CLOSE ? EOF : result;
}

static unsigned char byte_at(uint32_t index)
{
    return (unsigned char)(index * 17U + (index >> 7));
}

static void write_fixture(const char *path, uint32_t size, const char *mode)
{
    unsigned char block[4096];
    uint32_t at = 0;
    FILE *stream = fopen(path, mode);
    assert(stream);
    while (at < size) {
        uint32_t i, count = size - at;
        if (count > sizeof(block)) count = sizeof(block);
        for (i = 0; i < count; ++i) block[i] = byte_at(at + i);
        assert(fwrite(block, 1, count, stream) == count);
        at += count;
    }
    assert(fclose(stream) == 0);
}

static void reset(enum Fault next)
{
    assert(!live_allocations && !allocated_live && opened == closed);
    fault = next;
    opened = closed = read_calls = getc_calls = allocation_calls = 0;
    allocated_peak = largest_request = 0;
}

static void clean(void)
{
    assert(opened == closed);
    assert(live_allocations == 0 && allocated_live == 0);
    assert(largest_request <= 64);
}

static void valid_file(const char *path, uint32_t size)
{
    unsigned char *allocation = malloc((size_t)size + 32);
    unsigned char *data = allocation + 16;
    WcInput *input = NULL;
    uint32_t i;
    assert(allocation);
    memset(allocation, 0xa5, (size_t)size + 32);
    write_fixture(path, size, "wb");
    reset(NORMAL);
    assert(wc_input_open(path, size, &input) == NULL && input);
    assert(wc_input_read_close(input, size ? data : NULL, size) == NULL);
    for (i = 0; i < size; ++i) assert(data[i] == byte_at(i));
    for (i = 0; i < 16; ++i)
        assert(allocation[i] == 0xa5 && data[size + i] == 0xa5);
    assert(allocation_calls == 1 && allocated_peak == sizeof(WcInput));
    assert(read_calls == (size != 0));
    clean();
    printf("PASS complete file bytes=%lu; module allocations=%u peak=%lu bytes\n",
           (unsigned long)size, allocation_calls, (unsigned long)allocated_peak);
    free(allocation);
}

static void rejected_read(const char *path, enum Fault next, uint32_t capacity,
                          int null_destination)
{
    unsigned char storage[288];
    WcInput *input = NULL;
    unsigned int i;
    reset(NORMAL);
    memset(storage, 0xa5, sizeof(storage));
    assert(wc_input_open(path, 256, &input) == NULL && input);
    fault = next;
    assert(wc_input_read_close(input, null_destination ? NULL : storage + 16,
                               capacity) != NULL);
    for (i = 0; i < 16; ++i)
        assert(storage[i] == 0xa5 && storage[272 + i] == 0xa5);
    if (capacity != 256 || null_destination) {
        assert(read_calls == 0 && getc_calls == 0);
        for (i = 0; i < sizeof(storage); ++i) assert(storage[i] == 0xa5);
    }
    clean();
}

int main(int argc, char **argv)
{
    char path[512], missing[512];
    unsigned char data[256];
    WcInput *input;
    unsigned int i;
    const uint32_t sizes[] = { 0, 1, 17, 4096, 16UL * 1024UL * 1024UL };
    const char *directory = argc > 1 ? argv[1] : ".";
    assert(snprintf(path, sizeof(path), "%s/input with spaces.bin", directory) > 0);
    assert(snprintf(missing, sizeof(missing), "%s/nonexistent-file.bin", directory) > 0);
    puts("Direct-input tests: actual input.c, no Presentation Manager or OS/2 guest");
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) valid_file(path, sizes[i]);

    write_fixture(path, 256, "wb");
    reset(NORMAL);
    input = (WcInput *)(uintptr_t)1;
    assert(wc_input_open(NULL, 0, &input) != NULL && input == NULL);
    assert(wc_input_open("", 0, &input) != NULL && input == NULL);
    assert(wc_input_open(path, 256, NULL) != NULL);
    assert(wc_input_open(missing, 0, &input) != NULL && input == NULL);
    assert(wc_input_read_close(NULL, data, sizeof(data)) != NULL);
    wc_input_discard(NULL);
    clean();
    puts("PASS invalid paths, missing file, invalid output/handle, NULL discard");

    reset(NORMAL);
    assert(wc_input_open(path, 256, &input) == NULL);
    wc_input_discard(input);
    assert(read_calls == 0 && getc_calls == 0);
    clean();
    puts("PASS cancellation/discard closes and frees without reading");

    reset(FAIL_OPEN);
    assert(wc_input_open(path, 256, &input) != NULL && input == NULL);
    clean();
    reset(FAIL_ALLOC);
    assert(wc_input_open(path, 256, &input) != NULL && input == NULL);
    assert(opened == 1 && closed == 1);
    clean();
    puts("PASS stream-open and handle-allocation failures leave no resources");

    rejected_read(path, NORMAL, 255, 0);
    rejected_read(path, NORMAL, 256, 1);
    puts("PASS capacity mismatch and NULL destination rejected before read");
    rejected_read(path, SHORT_READ, 256, 0);
    rejected_read(path, FAIL_READ, 256, 0);
    rejected_read(path, FAIL_GETC, 256, 0);
    rejected_read(path, FAIL_ERROR, 256, 0);
    rejected_read(path, FAIL_CLOSE, 256, 0);
    puts("PASS short/read/getc/stream/close failures are rejected and closed");

    reset(NORMAL);
    assert(wc_input_open(path, 256, &input) == NULL);
    write_fixture(path, 128, "wb");
    assert(wc_input_read_close(input, data, sizeof(data)) != NULL);
    clean();
    puts("PASS file truncated after open is rejected");

    write_fixture(path, 256, "wb");
    reset(NORMAL);
    assert(wc_input_open(path, 256, &input) == NULL);
    write_fixture(path, 16, "ab");
    assert(wc_input_read_close(input, data, sizeof(data)) != NULL);
    clean();
    puts("PASS file grown after open is rejected");

    write_fixture(path, 1, "wb");
    reset(NORMAL);
    assert(wc_input_open(path, 0, &input) == NULL);
    assert(wc_input_read_close(input, NULL, 0) != NULL);
    clean();
    puts("PASS expected-empty file with unexpected data is rejected");
    assert(remove(path) == 0);
    puts("PASS all direct-input tests; allocation stays constant through 16 MiB");
    return 0;
}
