/* Native console startup observer. No Presentation Manager calls. */
#ifdef WARP_STRESS_TEST
#define _POSIX_C_SOURCE 200809L
#endif
#define INCL_DOS
#define INCL_DOSERRORS
#ifdef WARP_STRESS_TEST
#include "test-stubs/os2.h"
#else
#include <os2.h>
#endif
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POLL_MS 250UL
#define LOG_LIMIT 262144UL
typedef struct {
    const char *name, *exe, *log;
    char exe_path[260], log_path[260], detail[512], line[512];
    ULONG launch_begin, launch_return, ready, start_rc, session, log_rc, bytes;
    PID pid;
    HFILE handle;
    unsigned used;
    int opened, state; /* 0 waiting, 1 ready, 2 log failure, 3 launch failure */
} App;
static App apps[3] = {
    {"WordCraft", "WORDCRFT.EXE", "WCGUI.LOG"},
    {"DeckCraft", "DECKCRFT.EXE", "DCGUI.LOG"},
    {"GridCraft", "GRIDCRFT.EXE", "GCGUI.LOG"}
};
static ULONG started, timeout_ms;
static APIRET clock_error;
static volatile sig_atomic_t interrupted;

static void stop_signal(int sig) { (void)sig; interrupted = 1; }
static ULONG now(void) {
    ULONG value = 0;
    APIRET rc = DosQuerySysInfo(QSV_MS_COUNT, QSV_MS_COUNT, &value, sizeof(value));
    if (rc) clock_error = rc;
    return value;
}
static int missing(APIRET rc) { return rc == ERROR_FILE_NOT_FOUND || rc == ERROR_PATH_NOT_FOUND; }
static int equal_name(const char *a, const char *b) {
    while (*a && *b && toupper((unsigned char)*a) == toupper((unsigned char)*b)) { ++a; ++b; }
    return !*a && !*b;
}
static void quoted(FILE *out, const char *text) {
    fputc('"', out);
    while (*text) {
        if (*text == '"') fputc('"', out);
        fputc(*text++, out);
    }
    fputc('"', out);
}
static void record(FILE *out, const char *event, App *app, ULONG elapsed) {
    fprintf(out, "%s,%s,%lu,%lu,%lu,%lu,", event, app->name, started, elapsed,
            app->launch_begin, app->launch_return);
    if (app->state == 1) fprintf(out, "%lu", app->ready);
    fprintf(out, ",%lu,%lu,%lu,%lu,%lu,", app->start_rc, (ULONG)app->pid,
            app->session, app->log_rc, POLL_MS);
    quoted(out, app->detail); fputc('\n', out); fflush(out);
}
static void line_finished(App *app) {
    app->line[app->used] = 0;
    if (strstr(app->line, "failed") || !strncmp(app->line, "Could not ", 10) ||
        !strncmp(app->line, "The application rejected", 24) ||
        !strncmp(app->line, "GpiDrawBits:", 12)) {
        app->state = 2;
        strcpy(app->detail, app->line);
    } else if (app->state == 0 && !strncmp(app->line, "Startup frame: 3,", 17)) {
        ULONG observed = (ULONG)(now() - started);
        if (observed <= timeout_ms) {
            app->state = 1;
            app->ready = observed;
            strcpy(app->detail, app->line);
        }
    }
    app->used = 0;
}
static void scan(App *app) {
    char chunk[2048];
    ULONG got, action, n;
    APIRET rc;
    if (!app->opened) {
        rc = DosOpen(app->log_path, &app->handle, &action, 0, FILE_NORMAL,
                     OPEN_ACTION_FAIL_IF_NEW | OPEN_ACTION_OPEN_IF_EXISTS,
                     OPEN_ACCESS_READONLY | OPEN_SHARE_DENYNONE, NULL);
        app->log_rc = rc;
        if (rc) return;
        app->opened = 1;
    }
    do {
        got = 0;
        rc = DosRead(app->handle, chunk, sizeof(chunk), &got);
        app->log_rc = rc;
        if (rc) {
            app->state = 2;
            sprintf(app->detail, "DosRead failed: %lu", (ULONG)rc);
            return;
        }
        if (got > LOG_LIMIT - app->bytes) {
            app->state = 2;
            strcpy(app->detail, "Startup log exceeded 256 KiB observation limit");
            return;
        }
        app->bytes += got;
        for (n = 0; n < got; ++n) {
            if (chunk[n] == '\n') line_finished(app);
            else if (chunk[n] != '\r' && app->used < sizeof(app->line) - 1)
                app->line[app->used++] = chunk[n];
        }
    } while (got == sizeof(chunk));
}
static void usage(void) {
    puts("RUNTRIO C:\\CRAFT\\BASE D:\\RESULT.CSV timeout_seconds(1..600)");
    puts("Close earlier app instances and remove only WCGUI.LOG, DCGUI.LOG, GCGUI.LOG first.");
    puts("Run from an existing CMD session and keep that session open for the apps.");
}
int main(int argc, char **argv) {
    char appdir[260], *end;
    const char *csv_base;
    FILESTATUS3 st;
    FILE *out;
    APIRET rc;
    ULONG seconds, deadline, elapsed;
    size_t length;
    int i, completed = 0, all_ready = 1;
    if (argc != 4) { usage(); return 2; }
    errno = 0;
    for (end = argv[3]; *end >= '0' && *end <= '9'; ++end) {}
    if (!*argv[3] || *end) { usage(); return 2; }
    seconds = strtoul(argv[3], &end, 10);
    if (errno || seconds < 1 || seconds > 600) { usage(); return 2; }
    length = strlen(argv[1]);
    if (length < 3 || length >= sizeof(appdir) || !isalpha((unsigned char)argv[1][0]) ||
        argv[1][1] != ':' || (argv[1][2] != '\\' && argv[1][2] != '/')) {
        usage(); return 2;
    }
    strcpy(appdir, argv[1]);
    while (length > 3 && (appdir[length - 1] == '\\' || appdir[length - 1] == '/'))
        appdir[--length] = 0;
    csv_base = argv[2];
    for (end = argv[2]; *end; ++end) if (*end == '\\' || *end == '/' || *end == ':') csv_base = end + 1;
    if (!*csv_base) { usage(); return 2; }
    for (i = 0; i < 3; ++i) {
        int size;
        if (equal_name(csv_base, apps[i].log)) { usage(); return 2; }
        size = snprintf(apps[i].exe_path, sizeof(apps[i].exe_path), "%s\\%s", appdir, apps[i].exe);
        if (size < 0 || (size_t)size >= sizeof(apps[i].exe_path)) { usage(); return 2; }
        size = snprintf(apps[i].log_path, sizeof(apps[i].log_path), "%s\\%s", appdir, apps[i].log);
        if (size < 0 || (size_t)size >= sizeof(apps[i].log_path)) { usage(); return 2; }
        rc = DosQueryPathInfo(apps[i].exe_path, FIL_STANDARD, &st, sizeof(st));
        if (rc || (st.attrFile & FILE_DIRECTORY)) {
            fprintf(stderr, "Executable preflight failed: %s (rc %lu)\n", apps[i].exe_path, (ULONG)rc);
            return 2;
        }
        rc = DosQueryPathInfo(apps[i].log_path, FIL_STANDARD, &st, sizeof(st));
        if (!missing(rc)) {
            fprintf(stderr, "Fresh log required: %s (query rc %lu). No files removed.\n", apps[i].log_path, (ULONG)rc);
            return 2;
        }
    }
    /* Open before changing directory, so relative CSV paths use the caller's cwd. */
    out = fopen(argv[2], "wb");
    if (!out) { perror(argv[2]); return 2; }
    /* Plain fopen("wb") needs only this settable bit; access/share bits are invalid here. */
    rc = DosSetFHState((HFILE)fileno(out), OPEN_FLAGS_NOINHERIT);
    if (rc) { fprintf(stderr, "Could not set CSV noinherit flag (DosSetFHState): %lu\n", (ULONG)rc); fclose(out); return 2; }
    fputs("event,app,common_start_ms,event_elapsed_ms,launch_begin_elapsed_ms,launch_return_elapsed_ms,ready_elapsed_ms,start_rc,pid,session,log_rc,poll_ms,detail\n", out);
    fflush(out);
    rc = DosSetDefaultDisk((ULONG)(toupper((unsigned char)appdir[0]) - 'A' + 1));
    if (!rc) rc = DosSetCurrentDir(appdir);
    if (rc) { fprintf(stderr, "Could not set application cwd: %lu\n", (ULONG)rc); fclose(out); return 2; }
    signal(SIGINT, stop_signal);
#ifdef SIGBREAK
    signal(SIGBREAK, stop_signal);
#endif
    started = now(); deadline = seconds * 1000; timeout_ms = deadline;
    if (clock_error) { fclose(out); return 1; }
    /* No sleeps or CSV writes between these three starts. */
    for (i = 0; i < 3; ++i) {
        STARTDATA sd;
        memset(&sd, 0, sizeof(sd));
        sd.Length = sizeof(sd);
        sd.Related = SSF_RELATED_CHILD;
        sd.FgBg = SSF_FGBG_BACK;
        sd.PgmTitle = (PSZ)apps[i].name;
        sd.PgmName = (PSZ)apps[i].exe_path;
        sd.InheritOpt = SSF_INHERTOPT_PARENT;
        sd.SessionType = SSF_TYPE_PM;
        sd.PgmControl = SSF_CONTROL_VISIBLE;
        sd.ObjectBuffer = apps[i].detail;
        sd.ObjectBuffLen = sizeof(apps[i].detail);
        apps[i].launch_begin = (ULONG)(now() - started);
        rc = DosStartSession(&sd, &apps[i].session, &apps[i].pid);
        apps[i].start_rc = rc;
        apps[i].launch_return = (ULONG)(now() - started);
        apps[i].detail[sizeof(apps[i].detail) - 1] = 0;
        if (rc && rc != ERROR_SMG_START_IN_BACKGROUND) { apps[i].state = 3; ++completed; }
        else apps[i].detail[0] = 0;
    }
    for (i = 0; i < 3; ++i) record(out, apps[i].state == 3 ? "launch_failed" : "launched", &apps[i], apps[i].launch_return);
    while (completed < 3 && !interrupted && !clock_error) {
        elapsed = (ULONG)(now() - started);
        if (elapsed >= deadline) break;
        for (i = 0; i < 3; ++i) if (apps[i].state == 0) {
            scan(&apps[i]);
            if (apps[i].state) {
                ++completed;
                record(out, apps[i].state == 1 ? "ready" : "log_failed", &apps[i], (ULONG)(now() - started));
            }
        }
        if (completed < 3) {
            elapsed = (ULONG)(now() - started);
            if (elapsed < deadline) DosSleep(deadline - elapsed < POLL_MS ? deadline - elapsed : POLL_MS);
        }
    }
    for (i = 0; i < 3; ++i) {
        const char *event;
        if (apps[i].state != 1) all_ready = 0;
        if (apps[i].state == 0) {
            event = interrupted ? "interrupted" : clock_error ? "clock_failed" : "timeout";
            strcpy(apps[i].detail, "No complete Startup frame: 3, line observed");
        } else event = apps[i].state == 1 ? "complete" : "failed";
        record(out, event, &apps[i], (ULONG)(now() - started));
        if (apps[i].opened) DosClose(apps[i].handle);
    }
    if (clock_error || interrupted) all_ready = 0;
    if (ferror(out)) all_ready = 0;
    if (fclose(out)) all_ready = 0;
    printf("RUNTRIO: %s. Applications are left running; keep this CMD session open.\n", all_ready ? "all three startup frames observed" : "incomplete startup; inspect CSV/logs");
    return all_ready ? 0 : 1;
}
