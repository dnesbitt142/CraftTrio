/* Account requested live bytes, not allocator arenas or OS resident pages. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef union Header {
    long double alignment;
    struct { size_t size; } value;
} Header;
static size_t live, peak, calls;
static long fail_after = -1;

static int fail(void)
{
    ++calls;
    if (fail_after < 0) return 0;
    if (fail_after == 0) return 1;
    --fail_after;
    return 0;
}

void *qa_malloc(size_t size)
{
    Header *header;
    if (fail() || size > (size_t)-1 - sizeof(Header)) return NULL;
    header = malloc(sizeof(Header) + size);
    if (!header) return NULL;
    header->value.size = size;
    live += size;
    if (live > peak) peak = live;
    return header + 1;
}

void qa_free(void *ptr)
{
    Header *header;
    if (!ptr) return;
    header = (Header *)ptr - 1;
    live -= header->value.size;
    free(header);
}

void *qa_calloc(size_t count, size_t size)
{
    void *ptr;
    if (count && size > (size_t)-1 / count) return NULL;
    ptr = qa_malloc(count * size);
    if (ptr) memset(ptr, 0, count * size);
    return ptr;
}

void *qa_realloc(void *ptr, size_t size)
{
    Header *old;
    void *next;
    size_t keep;
    if (!ptr) return qa_malloc(size);
    if (!size) { qa_free(ptr); return NULL; }
    old = (Header *)ptr - 1;
    next = qa_malloc(size);
    if (!next) return NULL;
    keep = old->value.size < size ? old->value.size : size;
    memcpy(next, ptr, keep);
    qa_free(ptr);
    return next;
}

size_t qa_memory_live(void) { return live; }
size_t qa_memory_peak(void) { return peak; }
size_t qa_memory_calls(void) { return calls; }
void qa_memory_reset_peak(void) { peak = live; }
void qa_memory_fail_after(long count) { fail_after = count; }
