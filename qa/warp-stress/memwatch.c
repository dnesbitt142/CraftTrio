/* Warp system counters and optional task inventory; no process-RSS guesses. */
#include "common.h"
#include <ctype.h>

#ifndef WARP_STRESS_TEST
APIRET16 APIENTRY16 Dos16MemAvail(PULONG);
#endif
typedef APIRET (APIENTRY *QueryState)(ULONG, ULONG, PID, TID, PVOID, ULONG);
static QueryState query_state;
static unsigned char *task_buffer;
static ULONG task_bytes;

static void ws_csv_string(FILE *out, const char *s) {
    fputc('"', out);
    while (*s) {
        if (*s == '"') fputc('"', out);
        fputc(*s++, out);
    }
    fputc('"', out);
}

static void usage(void) {
    puts("MEMWATCH [--seconds 1..3600] [--interval-ms 100..60000] [--log FILE]");
    puts("         [--stop FILE] [--swap X:\\path\\SWAPPER.DAT]");
    puts("         [--tasks FILE] [--tasks-ms 1000..60000] [--no-legacy]");
    puts("Defaults: 300s, 1000ms, MEMWATCH.CSV, MEMWATCH.STOP, legacy estimate on.");
    puts("Tasks are optional inventory, not process memory measurements.");
}
static void decimal64(FILE *out, unsigned long long n) {
    char s[24];
    unsigned int at = sizeof(s);
    s[--at] = 0;
    do { s[--at] = (char)('0' + n % 10); n /= 10; } while (n);
    fputs(s + at, out);
}
static int inside(const void *p, ULONG bytes) {
    ULONG start = (ULONG)task_buffer, address = (ULONG)p;
    return address >= start && bytes <= task_bytes && address - start <= task_bytes - bytes;
}
static const char *module_name(USHORT handle) {
    QSLREC *m;
    ULONG steps = 0;
    if (!inside(task_buffer, sizeof(QSPTRREC))) return "";
    m = ((QSPTRREC *)task_buffer)->pLibRec;
    while (inside(m, sizeof(*m)) && steps++ < 16384) {
        if (m->hmte == handle && inside(m->pName, 1)) {
            ULONG max = task_bytes - ((ULONG)m->pName - (ULONG)task_buffer);
            if (memchr(m->pName, 0, max)) return (const char *)m->pName;
        }
        if (!m->pNextRec) break;
        m = (QSLREC *)m->pNextRec;
    }
    return "";
}
static APIRET tasks(FILE *out, ULONG elapsed, const char *stamp) {
    APIRET rc;
    QSPREC *p;
    ULONG guard = 0;
    if (!query_state) return ERROR_INVALID_FUNCTION;
    for (;;) {
        if (!task_buffer) {
            /* Extra committed slack accommodates older Warp implementations. */
            rc = DosAllocMem((PPVOID)&task_buffer, task_bytes + 65536,
                             PAG_READ | PAG_WRITE | PAG_COMMIT);
            if (rc) return rc;
        }
        memset(task_buffer, 0, task_bytes + 65536);
        rc = query_state(QS_PROCESS | QS_MTE, 0, 0, 0, task_buffer, task_bytes);
        if (rc != ERROR_BUFFER_OVERFLOW || task_bytes >= 1048576) break;
        DosFreeMem(task_buffer); task_buffer = NULL;
        task_bytes *= 2;
    }
    if (rc) return rc;
    p = ((QSPTRREC *)task_buffer)->pProcRec;
    while (inside(p, sizeof(*p)) && p->RecType == QS_PROCESS) {
        ULONG n, system = 0, user = 0, next;
        if (++guard > 16384 || !inside(p->pThrdRec, (ULONG)p->cTCB * sizeof(QSTREC)))
            return ERROR_INVALID_DATA;
        for (n = 0; n < p->cTCB; ++n) {
            system += p->pThrdRec[n].systime;
            user += p->pThrdRec[n].usertime;
        }
        fprintf(out, "%s,%lu,0,%u,%u,%lu,%lu,%lu,%u,%lu,%lu,%u,", stamp,
                elapsed, p->pid, p->ppid, p->type, p->stat, p->sgid,
                p->cTCB, system, user, p->hMte);
        ws_csv_string(out, module_name(p->hMte)); fputc('\n', out);
        next = (ULONG)p->pThrdRec + (ULONG)p->cTCB * sizeof(QSTREC);
        if (next <= (ULONG)p) return ERROR_INVALID_DATA;
        p = (QSPREC *)next;
    }
    fflush(out);
    return 0;
}
static void snapshot(FILE *out, const char *stamp, ULONG elapsed,
                     const char *swap_path, int legacy) {
    ULONG mem[5] = {0, 0, 0, 0, 0}, physical_free = 0;
    FILESTATUS3 swap;
    FSALLOCATE disk;
    APIRET qrc, lrc = ERROR_INVALID_FUNCTION, src = ERROR_INVALID_PARAMETER;
    APIRET drc = ERROR_INVALID_PARAMETER;
    qrc = DosQuerySysInfo(QSV_TOTPHYSMEM, QSV_MAXSHMEM, mem, sizeof(mem));
    if (legacy) {
        lrc = Dos16MemAvail(&physical_free);
        if (!lrc && !qrc && physical_free > mem[0]) lrc = ERROR_INVALID_DATA;
    }
    if (swap_path) {
        src = DosQueryPathInfo(swap_path, FIL_STANDARD, &swap, sizeof(swap));
        if (isalpha((unsigned char)swap_path[0]) && swap_path[1] == ':') {
            drc = DosQueryFSInfo((ULONG)(toupper((unsigned char)swap_path[0]) - 'A' + 1),
                                 FSIL_ALLOC, &disk, sizeof(disk));
        }
    }
    fprintf(out, "%s,%lu,%lu,", stamp, elapsed, (ULONG)qrc);
    if (!qrc) fprintf(out, "%lu,%lu,%lu,%lu,%lu", mem[0], mem[1], mem[2], mem[3], mem[4]);
    else fputs(",,,,", out);
    fprintf(out, ",%lu,", (ULONG)lrc);
    if (!lrc) fprintf(out, "%lu", physical_free);
    fprintf(out, ",%lu,", (ULONG)src);
    if (!src) fprintf(out, "%lu", swap.cbFile);
    fprintf(out, ",%lu,", (ULONG)drc);
    if (!drc) decimal64(out, (unsigned long long)disk.cUnit * disk.cSectorUnit * disk.cbSector);
    fputc(',', out);
    if (!drc) decimal64(out, (unsigned long long)disk.cUnitAvail * disk.cSectorUnit * disk.cbSector);
    fputc('\n', out);
    fflush(out);
}
int main(int argc, char **argv) {
    ULONG seconds = 300, interval = 1000, task_interval = 10000;
    ULONG started, duration, last_task = 0;
    const char *log_path = "MEMWATCH.CSV", *stop_path = "MEMWATCH.STOP";
    const char *swap_path = NULL, *task_path = NULL;
    FILE *out, *task_out = NULL;
    int legacy = 1, j, result = 0;
    for (j = 1; j < argc; ++j) {
        ULONG *dst = NULL, low = 0, high = 0;
        if (!strcmp(argv[j], "--help")) { usage(); return 0; }
        if (!strcmp(argv[j], "--no-legacy")) { legacy = 0; continue; }
        if (!strcmp(argv[j], "--seconds")) { dst = &seconds; low = 1; high = 3600; }
        else if (!strcmp(argv[j], "--interval-ms")) { dst = &interval; low = 100; high = 60000; }
        else if (!strcmp(argv[j], "--tasks-ms")) { dst = &task_interval; low = 1000; high = 60000; }
        else if (!strcmp(argv[j], "--log") || !strcmp(argv[j], "--stop") ||
                 !strcmp(argv[j], "--swap") || !strcmp(argv[j], "--tasks")) {
            const char *option = argv[j];
            if (++j == argc || !*argv[j]) { usage(); return 2; }
            if (!strcmp(option, "--log")) log_path = argv[j];
            else if (!strcmp(option, "--stop")) stop_path = argv[j];
            else if (!strcmp(option, "--swap")) swap_path = argv[j];
            else task_path = argv[j];
            continue;
        } else { usage(); return 2; }
        if (++j == argc || !ws_number(argv[j], low, high, dst)) { usage(); return 2; }
    }
    if (!strcmp(log_path, stop_path) || (task_path &&
        (!strcmp(task_path, log_path) || !strcmp(task_path, stop_path)))) { usage(); return 2; }
    ws_signals();
    started = ws_now(); duration = seconds * 1000;
    if (ws_stop(stop_path)) { puts("Stop requested before sampling."); return 0; }
    out = fopen(log_path, "wb");
    if (!out) { perror(log_path); return 2; }
    fputs("timestamp,elapsed_ms,qsv_rc,physical_total_bytes,system_resident_bytes,virtual_available_bytes,max_private_block_bytes,max_shared_block_bytes,legacy_rc,free_physical_legacy_estimate_bytes,swapfile_rc,swapfile_bytes,swap_drive_rc,swap_drive_total_bytes,swap_drive_free_bytes\n", out);
    if (task_path) {
        HMODULE doscalls = 0;
        PFN proc = NULL;
        APIRET rc;
        task_out = fopen(task_path, "wb");
        if (!task_out) { perror(task_path); fclose(out); return 2; }
        fputs("timestamp,elapsed_ms,query_rc,pid,ppid,type,status,screen_group,threads,thread_system_counter_sum,thread_user_counter_sum,module_handle,module\n", task_out);
        rc = DosQueryModuleHandle("DOSCALLS", &doscalls);
        if (!rc) rc = DosQueryProcAddr(doscalls, 368, NULL, &proc);
        if (!rc) query_state = (QueryState)proc;
        task_bytes = 65536;
    }
    puts("MEMWATCH sampling; Ctrl-C or stop file ends cleanly. CSV bytes are labeled by API meaning.");
    while (!ws_stop(stop_path) && (ULONG)(ws_now() - started) < duration) {
        char stamp[40];
        ULONG elapsed = (ULONG)(ws_now() - started);
        ws_timestamp(stamp, sizeof(stamp));
        snapshot(out, stamp, elapsed, swap_path, legacy);
        if (task_out && (!elapsed || !last_task || elapsed - last_task >= task_interval)) {
            APIRET rc = tasks(task_out, elapsed, stamp);
            if (rc) {
                fprintf(task_out, "%s,%lu,%lu,,,,,,,,,,\n", stamp, elapsed, (ULONG)rc);
                fflush(task_out);
            }
            last_task = elapsed + 1;
        }
        if (!ws_wait(interval, started, duration, stop_path)) break;
    }
    if (task_buffer && DosFreeMem(task_buffer)) result = 1;
    if (task_out) fclose(task_out);
    fclose(out);
    return result;
}
