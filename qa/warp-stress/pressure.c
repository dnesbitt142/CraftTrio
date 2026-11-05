/* Bounded OS/2 memory pressure; never locks pages or changes system settings. */
#include "common.h"

#define MAX_BLOCKS 256
static PVOID blocks[MAX_BLOCKS];
static ULONG count, page_size = 4096, checksum;
static ULONG started, duration;
static const char *stop_path = "PRESSURE.STOP";
static FILE *log_file;

static void usage(void) {
    puts("PRESSURE [--mib 1..256] [--seconds 1..3600] [--interval-ms 100..60000]");
    puts("         [--ramp-ms 0..10000] [--churn-mib 0..16] [--floor-mib 8..256]");
    puts("         [--stop FILE] [--log FILE]");
    puts("Defaults: 16 MiB, 300 s, 1000 ms revisit/log, 100 ms ramp, no churn,");
    puts("16 MiB virtual-available floor, PRESSURE.STOP, PRESSURE.CSV.");
    puts("Stop with Ctrl-C or create the stop file. A pre-existing stop file stops immediately.");
}
static void record(const char *event, ULONG api_rc, ULONG visit_ms) {
    char stamp[40];
    ULONG available = 0;
    APIRET rc = DosQuerySysInfo(QSV_TOTAVAILMEM, QSV_TOTAVAILMEM,
                              &available, sizeof(available));
    ws_timestamp(stamp, sizeof(stamp));
    fprintf(log_file, "%s,%lu,%s,%lu,%lu,%lu,%lu,%lu,%lu\n", stamp,
            (ULONG)(ws_now() - started), event, count * WS_MIB, api_rc,
            (ULONG)rc, available, visit_ms, checksum);
    fflush(log_file);
}
static int active(void) {
    return !ws_stop(stop_path) && (ULONG)(ws_now() - started) < duration;
}
static int safe_to_allocate(ULONG floor) {
    ULONG available = 0;
    APIRET rc = DosQuerySysInfo(QSV_TOTAVAILMEM, QSV_TOTAVAILMEM,
                              &available, sizeof(available));
    if (rc || available < floor || available - floor < WS_MIB) {
        record("available_floor", rc ? rc : ERROR_NOT_ENOUGH_MEMORY, 0);
        return 0;
    }
    return 1;
}
static void touch(PVOID block, ULONG generation) {
    volatile unsigned char *p = block;
    ULONG i;
    /* A changing byte per page forces real page access without a CPU burner. */
    for (i = 0; i < WS_MIB; i += page_size) {
        unsigned char next = (unsigned char)(p[i] + generation + 1);
        p[i] = next;
        checksum += next;
    }
}
static int allocate_one(ULONG floor) {
    APIRET rc;
    PVOID p = NULL;
    if (!active() || !safe_to_allocate(floor)) return 0;
    rc = DosAllocMem(&p, WS_MIB, PAG_COMMIT | PAG_READ | PAG_WRITE);
    if (rc) { record("allocation_failed", rc, 0); return 0; }
    blocks[count++] = p;
    touch(p, count);
    record("allocated", 0, 0);
    return 1;
}
static int release_one(void) {
    APIRET rc;
    if (!count) return 1;
    rc = DosFreeMem(blocks[count - 1]);
    if (rc) { record("release_failed", rc, 0); return 0; }
    blocks[--count] = NULL;
    return 1;
}
int main(int argc, char **argv) {
    ULONG target = 16, seconds = 300, interval = 1000, ramp = 100;
    ULONG churn = 0, floor_mib = 16, floor, iteration = 0, i;
    const char *log_path = "PRESSURE.CSV";
    APIRET priority_rc;
    int result = 0, j;
    for (j = 1; j < argc; ++j) {
        ULONG *dst = NULL, low = 0, high = 0;
        if (!strcmp(argv[j], "--help")) { usage(); return 0; }
        if (!strcmp(argv[j], "--mib")) { dst = &target; low = 1; high = MAX_BLOCKS; }
        else if (!strcmp(argv[j], "--seconds")) { dst = &seconds; low = 1; high = 3600; }
        else if (!strcmp(argv[j], "--interval-ms")) { dst = &interval; low = 100; high = 60000; }
        else if (!strcmp(argv[j], "--ramp-ms")) { dst = &ramp; high = 10000; }
        else if (!strcmp(argv[j], "--churn-mib")) { dst = &churn; high = 16; }
        else if (!strcmp(argv[j], "--floor-mib")) { dst = &floor_mib; low = 8; high = 256; }
        else if (!strcmp(argv[j], "--stop") || !strcmp(argv[j], "--log")) {
            int is_stop = !strcmp(argv[j], "--stop");
            if (++j == argc || !*argv[j]) { usage(); return 2; }
            if (is_stop) stop_path = argv[j]; else log_path = argv[j];
            continue;
        } else { usage(); return 2; }
        if (++j == argc || !ws_number(argv[j], low, high, dst)) { usage(); return 2; }
    }
    if (churn > target || !strcmp(stop_path, log_path)) { usage(); return 2; }
    duration = seconds * 1000;
    floor = floor_mib * WS_MIB;
    ws_signals();
    started = ws_now();
    if (ws_stop(stop_path)) { puts("Stop requested before allocation."); return 0; }
    log_file = fopen(log_path, "wb");
    if (!log_file) { perror(log_path); return 2; }
    puts("Bounded pressure started; Ctrl-C or stop file releases all owned blocks.");
    fprintf(log_file, "timestamp,elapsed_ms,event,owned_committed_bytes,api_rc,available_rc,virtual_available_bytes,visit_ms,checksum\n");
    if (DosQuerySysInfo(QSV_PAGE_SIZE, QSV_PAGE_SIZE, &page_size, sizeof(page_size)) ||
        !page_size || page_size > WS_MIB || WS_MIB % page_size) {
        record("invalid_page_size", ERROR_INVALID_PARAMETER, 0); result = 1; goto cleanup;
    }
    priority_rc = DosSetPriority(PRTYS_PROCESS, PRTYC_IDLETIME, 0, 0);
    record("started", priority_rc, 0);
    for (i = 0; i < target && active(); ++i) {
        if (!allocate_one(floor)) { result = 1; goto cleanup; }
        if (!ws_wait(ramp, started, duration, stop_path)) goto cleanup;
    }
    while (active()) {
        ULONG first = ws_now();
        if (!safe_to_allocate(floor)) { result = 1; goto cleanup; }
        ++iteration;
        for (i = 0; i < count && active(); ++i) {
            touch(blocks[i], iteration);
            DosSleep(1);
        }
        if (!active()) break;
        for (i = 0; i < churn; ++i) {
            if (!release_one()) { result = 1; goto cleanup; }
        }
        for (i = 0; i < churn; ++i) {
            if (!allocate_one(floor)) { result = 1; goto cleanup; }
        }
        record("revisit", 0, (ULONG)(ws_now() - first));
        if (!ws_wait(interval, started, duration, stop_path)) break;
    }
cleanup:
    record("releasing", 0, 0);
    /* Attempt every block even if a failing release needs OS process cleanup. */
    for (i = count; i > 0; --i) {
        APIRET rc = DosFreeMem(blocks[i - 1]);
        if (rc) { record("release_failed", rc, 0); result = 1; }
        else blocks[i - 1] = NULL;
    }
    for (i = 0, count = 0; i < MAX_BLOCKS; ++i) if (blocks[i]) ++count;
    record(count ? "exit_os_cleanup_required" : "released", 0, 0);
    fclose(log_file);
    printf("Stopped; %lu owned MiB remaining before process exit.\n", count);
    return result;
}
