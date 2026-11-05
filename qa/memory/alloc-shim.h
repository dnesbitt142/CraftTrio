/* Test-only allocation accounting and deterministic failure injection. */
#ifndef QA_MEMORY_ALLOC_SHIM_H
#define QA_MEMORY_ALLOC_SHIM_H
#include <stddef.h>
#include <stdlib.h>
void *qa_malloc(size_t size);
void *qa_calloc(size_t count, size_t size);
void *qa_realloc(void *ptr, size_t size);
void qa_free(void *ptr);
size_t qa_memory_live(void);
size_t qa_memory_peak(void);
size_t qa_memory_calls(void);
void qa_memory_reset_peak(void);
void qa_memory_fail_after(long count);
#define malloc qa_malloc
#define calloc qa_calloc
#define realloc qa_realloc
#define free qa_free
#endif
