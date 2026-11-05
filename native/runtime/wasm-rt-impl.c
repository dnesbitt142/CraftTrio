/*
 * Copyright 2018 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "wasm-rt-impl.h"

#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __OS2__
#define INCL_DOSMEMMGR
#define INCL_DOSERRORS
#include <os2.h>
#endif

#if WASM_RT_INSTALL_SIGNAL_HANDLER && !defined(_WIN32)
#include <signal.h>
#include <unistd.h>
#endif

#if WASM_RT_USE_MMAP || WASM_RT_INSTALL_SIGNAL_HANDLER
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#endif

#define PAGE_SIZE 65536
#define WC_NATIVE_PAGE 4096U
#define WC_MEMORY_REGIONS 128U
typedef struct {
  const wasm_rt_memory_t* memory;
  uint32_t offset, length, committed;
} WcMemoryRegion;
static WcMemoryRegion wc_memory_regions[WC_MEMORY_REGIONS];
#if defined(__OS2__) && !defined(WC_WASM_MAX_PAGES)
/* Keep the same 96 MiB limit as the prior OS/2 host. */
#define WC_WASM_MAX_PAGES (96 * 1024 * 1024 / PAGE_SIZE)
#endif

#if WASM_RT_INSTALL_SIGNAL_HANDLER
static bool g_signal_handler_installed = false;
#ifdef _WIN32
static void* g_sig_handler_handle = 0;
#else
static char* g_alt_stack = 0;
#endif
#endif

#if WASM_RT_USE_STACK_DEPTH_COUNT
WASM_RT_THREAD_LOCAL uint32_t wasm_rt_call_stack_depth;
WASM_RT_THREAD_LOCAL uint32_t wasm_rt_saved_call_stack_depth;
#endif

WASM_RT_THREAD_LOCAL wasm_rt_jmp_buf g_wasm_rt_jmp_buf;

#ifdef WASM_RT_TRAP_HANDLER
extern void WASM_RT_TRAP_HANDLER(wasm_rt_trap_t code);
#endif

#ifdef WASM_RT_GROW_FAILED_HANDLER
extern void WASM_RT_GROW_FAILED_HANDLER();
#endif

void wasm_rt_trap(wasm_rt_trap_t code) {
  assert(code != WASM_RT_TRAP_NONE);
#if WASM_RT_USE_STACK_DEPTH_COUNT
  wasm_rt_call_stack_depth = wasm_rt_saved_call_stack_depth;
#endif

#ifdef WASM_RT_TRAP_HANDLER
  WASM_RT_TRAP_HANDLER(code);
  wasm_rt_unreachable();
#else
  WASM_RT_LONGJMP(g_wasm_rt_jmp_buf, code);
#endif
}

#if WASM_RT_USE_MMAP || WASM_RT_INSTALL_SIGNAL_HANDLER
#ifdef _WIN32
static void* os_mmap(size_t size) {
  void* ret = VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_NOACCESS);
  return ret;
}

static int os_munmap(void* addr, size_t size) {
  // Windows can only unmap the whole mapping
  (void)size; /* unused */
  BOOL succeeded = VirtualFree(addr, 0, MEM_RELEASE);
  return succeeded ? 0 : -1;
}

static int os_mprotect(void* addr, size_t size) {
  if (size == 0) {
    return 0;
  }
  void* ret = VirtualAlloc(addr, size, MEM_COMMIT, PAGE_READWRITE);
  if (ret == addr) {
    return 0;
  }
  VirtualFree(addr, 0, MEM_RELEASE);
  return -1;
}

static void os_print_last_error(const char* msg) {
  DWORD errorMessageID = GetLastError();
  if (errorMessageID != 0) {
    LPSTR messageBuffer = 0;
    // The api creates the buffer that holds the message
    size_t size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, errorMessageID, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)&messageBuffer, 0, NULL);
    (void)size;
    printf("%s. %s\n", msg, messageBuffer);
    LocalFree(messageBuffer);
  } else {
    printf("%s. No error code.\n", msg);
  }
}

#if WASM_RT_INSTALL_SIGNAL_HANDLER

static LONG os_signal_handler(PEXCEPTION_POINTERS info) {
  if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
    wasm_rt_trap(WASM_RT_TRAP_OOB);
  } else if (info->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
    wasm_rt_trap(WASM_RT_TRAP_EXHAUSTION);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

static void os_install_signal_handler(void) {
  g_sig_handler_handle =
      AddVectoredExceptionHandler(1 /* CALL_FIRST */, os_signal_handler);
}

static void os_cleanup_signal_handler(void) {
  RemoveVectoredExceptionHandler(g_sig_handler_handle);
}

#endif

#else
static void* os_mmap(size_t size) {
  int map_prot = PROT_NONE;
  int map_flags = MAP_ANONYMOUS | MAP_PRIVATE;
  uint8_t* addr = mmap(NULL, size, map_prot, map_flags, -1, 0);
  if (addr == MAP_FAILED)
    return NULL;
  return addr;
}

static int os_munmap(void* addr, size_t size) {
  return munmap(addr, size);
}

static int os_mprotect(void* addr, size_t size) {
  return mprotect(addr, size, PROT_READ | PROT_WRITE);
}

static void os_print_last_error(const char* msg) {
  perror(msg);
}

#if WASM_RT_INSTALL_SIGNAL_HANDLER
static void os_signal_handler(int sig, siginfo_t* si, void* unused) {
  if (si->si_code == SEGV_ACCERR) {
    wasm_rt_trap(WASM_RT_TRAP_OOB);
  } else {
    wasm_rt_trap(WASM_RT_TRAP_EXHAUSTION);
  }
}

static void os_install_signal_handler(void) {
  /* Use alt stack to handle SIGSEGV from stack overflow */
  g_alt_stack = malloc(SIGSTKSZ);
  if (g_alt_stack == NULL) {
    perror("malloc failed");
    abort();
  }

  stack_t ss;
  ss.ss_sp = g_alt_stack;
  ss.ss_flags = 0;
  ss.ss_size = SIGSTKSZ;
  if (sigaltstack(&ss, NULL) != 0) {
    perror("sigaltstack failed");
    abort();
  }

  struct sigaction sa;
  memset(&sa, '\0', sizeof(sa));
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  sa.sa_sigaction = os_signal_handler;

  /* Install SIGSEGV and SIGBUS handlers, since macOS seems to use SIGBUS. */
  if (sigaction(SIGSEGV, &sa, NULL) != 0 || sigaction(SIGBUS, &sa, NULL) != 0) {
    perror("sigaction failed");
    abort();
  }
}

static void os_cleanup_signal_handler(void) {
  /* Undo what was done in os_install_signal_handler */
  struct sigaction sa;
  memset(&sa, '\0', sizeof(sa));
  sa.sa_handler = SIG_DFL;
  if (sigaction(SIGSEGV, &sa, NULL) != 0 || sigaction(SIGBUS, &sa, NULL)) {
    perror("sigaction failed");
    abort();
  }

  if (sigaltstack(NULL, NULL) != 0) {
    perror("sigaltstack failed");
    abort();
  }

  free(g_alt_stack);
}
#endif

#endif
#endif

void wasm_rt_init(void) {
#if WASM_RT_INSTALL_SIGNAL_HANDLER
  if (!g_signal_handler_installed) {
    g_signal_handler_installed = true;
    os_install_signal_handler();
  }
#endif
}

bool wasm_rt_is_initialized(void) {
#if WASM_RT_INSTALL_SIGNAL_HANDLER
  return g_signal_handler_installed;
#else
  return true;
#endif
}

void wasm_rt_free(void) {
#if WASM_RT_INSTALL_SIGNAL_HANDLER
  os_cleanup_signal_handler();
  g_signal_handler_installed = false;
#endif
}

#if WASM_RT_USE_MMAP

static uint64_t get_allocation_size_for_mmap(wasm_rt_memory_t* memory) {
  assert(!memory->is64 &&
         "memory64 is not yet compatible with WASM_RT_USE_MMAP");
#if WASM_RT_MEMCHECK_GUARD_PAGES
  /* Reserve 8GiB. */
  const uint64_t max_size = 0x200000000ul;
  return max_size;
#else
  if (memory->max_pages != 0) {
    const uint64_t max_size = memory->max_pages * PAGE_SIZE;
    return max_size;
  }

  /* Reserve 4GiB. */
  const uint64_t max_size = 0x100000000ul;
  return max_size;
#endif
}

#endif

#ifdef __OS2__
static void report_memory_error(const char* api, APIRET rc, ULONG size) {
  FILE* log = fopen("WCENGINE.LOG", "ab");
  if (log) {
    fprintf(log, "%s failed with OS/2 error %lu for %lu bytes\n", api,
            (unsigned long)rc, (unsigned long)size);
    fclose(log);
  }
}
#endif

void wasm_rt_allocate_memory(wasm_rt_memory_t* memory,
                             uint64_t initial_pages,
                             uint64_t max_pages,
                             bool is64) {
#ifdef WC_WASM_MAX_PAGES
  if (max_pages > WC_WASM_MAX_PAGES) {
    max_pages = WC_WASM_MAX_PAGES;
  }
  if (is64 || initial_pages > max_pages) {
    wasm_rt_trap(WASM_RT_TRAP_EXHAUSTION);
  }
#endif
  uint64_t byte_length = initial_pages * PAGE_SIZE;
  memory->size = byte_length;
  memory->pages = initial_pages;
  memory->max_pages = max_pages;
  memory->is64 = is64;

#if defined(__OS2__)
  PVOID address = NULL;
  if (max_pages > UINT32_MAX / PAGE_SIZE) {
    wasm_rt_trap(WASM_RT_TRAP_EXHAUSTION);
  }
  const ULONG reserve_size = (ULONG)(max_pages * PAGE_SIZE);
  if (reserve_size) {
    APIRET rc = DosAllocMem(&address, reserve_size, OBJ_ANY | PAG_READ | PAG_WRITE);
    if (rc == ERROR_INVALID_PARAMETER) {
      rc = DosAllocMem(&address, reserve_size, PAG_READ | PAG_WRITE);
    }
    if (rc) {
      report_memory_error("DosAllocMem", rc, reserve_size);
      wasm_rt_trap(WASM_RT_TRAP_EXHAUSTION);
    }
    if (byte_length) {
      rc = DosSetMem(address, (ULONG)byte_length, PAG_COMMIT | PAG_DEFAULT);
      if (rc) {
        DosFreeMem(address);
        report_memory_error("DosSetMem", rc, (ULONG)byte_length);
        wasm_rt_trap(WASM_RT_TRAP_EXHAUSTION);
      }
      memset(address, 0, (size_t)byte_length);
    }
  }
  memory->data = address;
#elif WASM_RT_USE_MMAP
  const uint64_t mmap_size = get_allocation_size_for_mmap(memory);
  void* addr = os_mmap(mmap_size);
  if (!addr) {
    os_print_last_error("os_mmap failed.");
    abort();
  }
  int ret = os_mprotect(addr, byte_length);
  if (ret != 0) {
    os_print_last_error("os_mprotect failed.");
    abort();
  }
  memory->data = addr;
#else
  memory->data = calloc((size_t)byte_length, 1);
  if (byte_length && !memory->data) {
    wasm_rt_trap(WASM_RT_TRAP_EXHAUSTION);
  }
#endif
}

static uint64_t grow_memory_impl(wasm_rt_memory_t* memory, uint64_t delta) {
  uint64_t old_pages = memory->pages;
  uint64_t new_pages = memory->pages + delta;
  if (delta == 0) {
    return old_pages;
  }
  if (new_pages < old_pages || new_pages > memory->max_pages) {
    return (uint64_t)-1;
  }
  uint64_t old_size = old_pages * PAGE_SIZE;
  uint64_t new_size = new_pages * PAGE_SIZE;
  uint64_t delta_size = delta * PAGE_SIZE;
#if defined(__OS2__)
  uint8_t* new_data = memory->data;
  APIRET rc = DosSetMem(new_data + old_size, (ULONG)delta_size,
                        PAG_COMMIT | PAG_DEFAULT);
  if (rc) {
    report_memory_error("DosSetMem", rc, (ULONG)delta_size);
    return (uint64_t)-1;
  }
  memset(new_data + old_size, 0, (size_t)delta_size);
#elif WASM_RT_USE_MMAP
  uint8_t* new_data = memory->data;
  int ret = os_mprotect(new_data + old_size, delta_size);
  if (ret != 0) {
    return (uint64_t)-1;
  }
#else
  uint8_t* new_data = realloc(memory->data, (size_t)new_size);
  if (new_data == NULL) {
    return (uint64_t)-1;
  }
#if !WABT_BIG_ENDIAN
  memset(new_data + old_size, 0, delta_size);
#endif
#endif
#if WABT_BIG_ENDIAN
  memmove(new_data + new_size - old_size, new_data, old_size);
  memset(new_data, 0, delta_size);
#endif
  memory->pages = new_pages;
  memory->size = new_size;
  memory->data = new_data;
  return old_pages;
}

uint64_t wasm_rt_grow_memory(wasm_rt_memory_t* memory, uint64_t delta) {
  uint64_t ret = grow_memory_impl(memory, delta);
#ifdef WASM_RT_GROW_FAILED_HANDLER
  if (ret == -1) {
    WASM_RT_GROW_FAILED_HANDLER();
  }
#endif
  return ret;
}

void wasm_rt_free_memory(wasm_rt_memory_t* memory) {
  unsigned int wc_i;
  for (wc_i = 0; wc_i < WC_MEMORY_REGIONS; ++wc_i)
    if (wc_memory_regions[wc_i].memory == memory)
      memset(&wc_memory_regions[wc_i], 0, sizeof(wc_memory_regions[wc_i]));
#if defined(__OS2__)
  if (memory->data) {
    DosFreeMem(memory->data);
    memory->data = NULL;
  }
#elif WASM_RT_USE_MMAP
  const uint64_t mmap_size = get_allocation_size_for_mmap(memory);
  os_munmap(memory->data, mmap_size);  // ignore error
#else
  free(memory->data);
#endif
}

uint64_t wasm_rt_reclaimed_memory_bytes(const wasm_rt_memory_t* memory) {
  uint64_t bytes = 0;
  unsigned int i;
  for (i = 0; i < WC_MEMORY_REGIONS; ++i) {
    const WcMemoryRegion* region = &wc_memory_regions[i];
    if (region->memory == memory) {
#if defined(__OS2__)
      uint32_t scanned = 0;
      /* Query actual mappings, including a possible partial failed commit.
       * Unknown/query-failed ranges count as committed, never overclaimed. */
      while (scanned < region->length) {
        ULONG extent = region->length - scanned, flags = 0;
        if (DosQueryMem(memory->data + region->offset + scanned, &extent, &flags) ||
            !extent) break;
        if (extent > region->length - scanned) extent = region->length - scanned;
        if (!(flags & (PAG_COMMIT | PAG_FREE))) bytes += extent;
        scanned += extent;
      }
#else
      bytes += (uint64_t)region->length - region->committed;
#endif
    }
  }
  return bytes;
}

#if defined(__OS2__)
/* Idempotently restore access by querying homogeneous mapped runs. This also
 * handles an OS call that committed only part of a requested range. */
static int wc_restore_user_pages(uint8_t* address, uint32_t length) {
  while (length) {
    ULONG extent = length, flags = 0;
    APIRET rc = DosQueryMem(address, &extent, &flags);
    if (rc || !extent || (extent & (WC_NATIVE_PAGE - 1U))) {
      report_memory_error("DosQueryMem restore", rc ? rc : ERROR_INVALID_PARAMETER, length);
      return 0;
    }
    if (extent > length) extent = length;
    if ((flags & (PAG_COMMIT | PAG_READ | PAG_WRITE | PAG_GUARD)) !=
        (PAG_COMMIT | PAG_READ | PAG_WRITE)) {
      ULONG wanted = PAG_READ | PAG_WRITE;
      if (!(flags & PAG_COMMIT)) wanted |= PAG_COMMIT;
      rc = DosSetMem(address, extent, wanted);
      if (rc) { report_memory_error("DosSetMem restore", rc, extent); return 0; }
    }
    address += extent;
    length -= extent;
  }
  return 1;
}
#endif

uint64_t wasm_rt_committed_memory_bytes(const wasm_rt_memory_t* memory) {
  const uint64_t reclaimed = wasm_rt_reclaimed_memory_bytes(memory);
  return memory && reclaimed <= memory->size ? memory->size - reclaimed : 0;
}

/* This ledger contains no guest pointers to decommitted memory. Its records
 * describe user bytes of still-owned System allocations, never libc headers,
 * free lists, Rust objects, stack or module data. Registering ownership is the
 * allocator's responsibility; subsequent operations are checked here as well.
 * Every registered object has an accessible prefix and a decommitted suffix.
 */
uint32_t wasm_rt_memory_pages(wasm_rt_memory_t* memory, uint32_t operation,
                              uint32_t offset, uint32_t length) {
#if defined(__OS2__) || defined(WC_MEMORY_RECLAIM_TEST)
  unsigned int i;
  int vacant = -1;
  WcMemoryRegion* found = NULL;
  if (!memory || !memory->data || !length ||
      (offset & (WC_NATIVE_PAGE - 1U)) ||
      (length & (WC_NATIVE_PAGE - 1U)) ||
      offset > memory->size || length > memory->size - offset)
    return 1;
  for (i = 0; i < WC_MEMORY_REGIONS; ++i) {
    WcMemoryRegion* region = &wc_memory_regions[i];
    if (!region->memory) { if (vacant < 0) vacant = (int)i; continue; }
    if (region->memory != memory) continue;
    if ((uint64_t)offset < (uint64_t)region->offset + region->length &&
        (uint64_t)region->offset < (uint64_t)offset + length) {
      if (operation == 0) return 1; /* no aliases or overlapping owners */
      if (offset >= region->offset &&
          (uint64_t)offset + length <= (uint64_t)region->offset + region->length)
        found = region;
      else
        return 1;
    }
  }
  if (operation == 0) {
    if (vacant < 0) return 1;
    found = &wc_memory_regions[vacant];
    found->memory = memory;
    found->offset = offset;
    found->length = length;
    found->committed = length;
    return 0;
  }
  if (!found) return 1;
  if (operation == 3) {
    if (offset != found->offset || length != found->length ||
        found->committed != found->length) return 1;
    memset(found, 0, sizeof(*found));
    return 0;
  }
  if (operation == 1) {
    if ((uint64_t)offset != (uint64_t)found->offset + found->committed ||
        length > found->length - found->committed) return 1;
#if defined(__OS2__)
    if (!wc_restore_user_pages(memory->data + offset, length)) return 1;
#endif
    memset(memory->data + offset, 0, length);
    found->committed += length;
    return 0;
  }
  if (operation == 2) {
    if (offset < found->offset ||
        (uint64_t)offset + length != (uint64_t)found->offset + found->committed)
      return 1;
#if defined(__OS2__)
    {
      APIRET rc = DosSetMem(memory->data + offset, length, PAG_DECOMMIT);
      if (rc) {
        report_memory_error("DosSetMem decommit", rc, length);
        /* Do not assume a failed multi-page kernel call was atomic. Return a
         * recoverable error only after every affected page is accessible.
         * If rollback cannot restore access, stop the engine instead of ever
         * returning an inaccessible pointer from its allocator. */
        if (!wc_restore_user_pages(memory->data + offset, length))
          wasm_rt_trap(WASM_RT_TRAP_EXHAUSTION);
        return 1;
      }
    }
#endif
    found->committed -= length;
    return 0;
  }
  return 1;
#else
  /* Plain calloc/realloc hosts cannot safely decommit their unaligned base.
   * Refuse registration so the Rust wrapper falls back to ordinary System. */
  (void)memory; (void)operation; (void)offset; (void)length;
  return 1;
#endif
}

#define DEFINE_TABLE_OPS(type)                                          \
  void wasm_rt_allocate_##type##_table(wasm_rt_##type##_table_t* table, \
                                       uint32_t elements,               \
                                       uint32_t max_elements) {         \
    table->size = elements;                                             \
    table->max_size = max_elements;                                     \
    table->data = calloc(table->size, sizeof(wasm_rt_##type##_t));      \
  }                                                                     \
  void wasm_rt_free_##type##_table(wasm_rt_##type##_table_t* table) {   \
    free(table->data);                                                  \
  }                                                                     \
  uint32_t wasm_rt_grow_##type##_table(wasm_rt_##type##_table_t* table, \
                                       uint32_t delta,                  \
                                       wasm_rt_##type##_t init) {       \
    uint32_t old_elems = table->size;                                   \
    uint64_t new_elems = (uint64_t)table->size + delta;                 \
    if (new_elems == 0) {                                               \
      return 0;                                                         \
    }                                                                   \
    if ((new_elems < old_elems) || (new_elems > table->max_size)) {     \
      return (uint32_t)-1;                                              \
    }                                                                   \
    void* new_data =                                                    \
        realloc(table->data, new_elems * sizeof(wasm_rt_##type##_t));   \
    if (!new_data) {                                                    \
      return (uint32_t)-1;                                              \
    }                                                                   \
    table->data = new_data;                                             \
    table->size = new_elems;                                            \
    for (uint32_t i = old_elems; i < new_elems; i++) {                  \
      table->data[i] = init;                                            \
    }                                                                   \
    return old_elems;                                                   \
  }

DEFINE_TABLE_OPS(funcref)
DEFINE_TABLE_OPS(externref)

const char* wasm_rt_strerror(wasm_rt_trap_t trap) {
  switch (trap) {
    case WASM_RT_TRAP_NONE:
      return "No error";
    case WASM_RT_TRAP_OOB:
#if WASM_RT_MERGED_OOB_AND_EXHAUSTION_TRAPS
      return "Out-of-bounds access in linear memory or a table, or call stack "
             "exhausted";
#else
      return "Out-of-bounds access in linear memory or a table";
    case WASM_RT_TRAP_EXHAUSTION:
      return "Call stack exhausted";
#endif
    case WASM_RT_TRAP_INT_OVERFLOW:
      return "Integer overflow on divide or truncation";
    case WASM_RT_TRAP_DIV_BY_ZERO:
      return "Integer divide by zero";
    case WASM_RT_TRAP_INVALID_CONVERSION:
      return "Conversion from NaN to integer";
    case WASM_RT_TRAP_UNREACHABLE:
      return "Unreachable instruction executed";
    case WASM_RT_TRAP_CALL_INDIRECT:
      return "Invalid call_indirect or return_call_indirect";
    case WASM_RT_TRAP_UNCAUGHT_EXCEPTION:
      return "Uncaught exception";
    case WASM_RT_TRAP_UNALIGNED:
      return "Unaligned atomic memory access";
  }
  return "invalid trap code";
}
