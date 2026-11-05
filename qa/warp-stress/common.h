#ifndef WARP_STRESS_COMMON_H
#define WARP_STRESS_COMMON_H

#define INCL_DOS
#define INCL_DOSERRORS
#ifdef WARP_STRESS_TEST
#include "test-stubs/os2.h"
#else
#include <os2.h>
#endif
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WS_MIB 1048576UL
static volatile sig_atomic_t ws_interrupted;

static void ws_signal(int sig) { (void)sig; ws_interrupted = 1; }
static void ws_signals(void) {
    signal(SIGINT, ws_signal);
#ifdef SIGBREAK
    signal(SIGBREAK, ws_signal);
#endif
    signal(SIGTERM, ws_signal);
}
static int ws_number(const char *s, ULONG low, ULONG high, ULONG *value) {
    char *end;
    unsigned long n;
    const char *p = s;
    if (!*p) return 0;
    while (*p) { if (*p < '0' || *p > '9') return 0; ++p; }
    errno = 0;
    n = strtoul(s, &end, 10);
    if (errno || *end || n < low || n > high) return 0;
    *value = (ULONG)n;
    return 1;
}
static ULONG ws_now(void) {
    ULONG now = 0;
    if (DosQuerySysInfo(QSV_MS_COUNT, QSV_MS_COUNT, &now, sizeof(now))) {
        /* A missing clock is a hard stop, never an unbounded workload. */
        ws_interrupted = 1;
    }
    return now;
}
static int ws_stop(const char *path) {
    FILESTATUS3 st;
    return ws_interrupted || DosQueryPathInfo(path, FIL_STANDARD, &st, sizeof(st)) == 0;
}
static int ws_wait(ULONG delay, ULONG started, ULONG duration, const char *stop) {
    ULONG first = ws_now();
    while (!ws_stop(stop) && (ULONG)(ws_now() - started) < duration) {
        ULONG elapsed = (ULONG)(ws_now() - first);
        ULONG left;
        if (elapsed >= delay) return 1;
        left = delay - elapsed;
        DosSleep(left > 100 ? 100 : left);
    }
    return 0;
}
static void ws_timestamp(char *out, size_t size) {
    DATETIME dt;
    if (!DosGetDateTime(&dt)) {
        sprintf(out, "%04u-%02u-%02uT%02u:%02u:%02u.%02u", dt.year,
                dt.month, dt.day, dt.hours, dt.minutes, dt.seconds, dt.hundredths);
    } else if (size) {
        out[0] = 0;
    }
}
#endif
