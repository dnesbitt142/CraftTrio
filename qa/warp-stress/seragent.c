/* Temporary, manually started QA command bridge over a private COM1 handle. */
#define INCL_DOS
#define INCL_DOSERRORS
#define INCL_DOSDEVIOCTL
#ifdef WARP_STRESS_TEST
#include "test-stubs/serial-os2.h"
#else
#include <os2.h>
#endif
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_COMMAND 512
#define MAX_LINE (MAX_COMMAND + 5)
#define MAX_FILE 67108864L
#define DCB_WIRE_SIZE 11UL
#ifndef SERAGENT_MAX_SEQUENCE
#define SERAGENT_MAX_SEQUENCE 9999999UL
#endif
#ifndef SERAGENT_MAX_OUTPUT
#define SERAGENT_MAX_OUTPUT 9999999UL
#endif
#if SERAGENT_MAX_SEQUENCE < 1 || SERAGENT_MAX_SEQUENCE > 9999999UL || SERAGENT_MAX_OUTPUT < 1 || SERAGENT_MAX_OUTPUT > 9999999UL
#error Serial sequence and output bounds must fit seven decimal digits
#endif
static HFILE port;
static int broken;
static volatile sig_atomic_t stopped;
static ULONG sequence, next_output = 1;
static int sequence_exhausted;

static void stop_signal(int sig) { (void)sig; stopped = 1; }
static ULONG ticks(void) {
    ULONG value = 0;
    DosQuerySysInfo(QSV_MS_COUNT, QSV_MS_COUNT, &value, sizeof(value));
    return value;
}
static int send_bytes(const char *data, ULONG size) {
    while (size && !broken) {
        ULONG written = 0;
        APIRET rc = DosWrite(port, (PVOID)data, size, &written);
        if (rc || !written || written > size) {
            fprintf(stderr, "SERAGENT serial write failed rc=%lu bytes=%lu\n", (ULONG)rc, written);
            broken = 1; return 0;
        }
        data += written; size -= written;
    }
    return !broken;
}
static int sendf(const char *format, ...) {
    char text[1024];
    int size;
    va_list args;
    va_start(args, format);
    size = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (size < 0 || (size_t)size >= sizeof(text)) { broken = 1; return 0; }
    return send_bytes(text, (ULONG)size);
}
static void hex_path(const char *path, char *out) {
    static const char hex[] = "0123456789ABCDEF";
    while (*path) {
        unsigned char c = (unsigned char)*path++;
        *out++ = hex[c >> 4]; *out++ = hex[c & 15];
    }
    *out = 0;
}
static int base64_line(const unsigned char *block, size_t size) {
    static const char b[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char line[67];
    size_t i, at = 0;
    for (i = 0; i < size; i += 3) {
        ULONG value = (ULONG)block[i] << 16;
        if (i + 1 < size) value |= (ULONG)block[i + 1] << 8;
        if (i + 2 < size) value |= block[i + 2];
        line[at++] = b[(value >> 18) & 63]; line[at++] = b[(value >> 12) & 63];
        line[at++] = i + 1 < size ? b[(value >> 6) & 63] : '=';
        line[at++] = i + 2 < size ? b[value & 63] : '=';
    }
    line[at++] = '\r'; line[at++] = '\n';
    return send_bytes(line, (ULONG)at);
}
static int get_file(const char *path) {
    char hex[520];
    FILE *file = fopen(path, "rb");
    long length = -1;
    ULONG total = 0;
    unsigned char block[48];
    size_t got;
    int failure = 0, saved_errno;
    hex_path(path, hex);
    if (file && !fseek(file, 0, SEEK_END)) length = ftell(file);
    if (!file || length < 0 || length > MAX_FILE || fseek(file, 0, SEEK_SET)) {
        saved_errno = length > MAX_FILE ? EFBIG : errno ? errno : EIO;
        sendf("GUESTOUT-ERROR %s %d\r\n", hex, saved_errno);
        if (file) fclose(file);
        return 1;
    }
    sendf("GUESTOUT-BEGIN %ld %s\r\n", length, hex);
    while (!broken && (got = fread(block, 1, sizeof(block), file)) != 0) {
        total += (ULONG)got;
        if (total > (ULONG)length) { failure = 1; break; }
        base64_line(block, got);
    }
    if (ferror(file) || total != (ULONG)length) failure = 1;
    if (fclose(file)) failure = 1;
    if (!broken) {
        if (failure) sendf("GUESTOUT-ABORT\r\n");
        else sendf("GUESTOUT-END %lu\r\n", total);
    }
    return failure || broken;
}
static APIRET control(ULONG function, PVOID parameters, ULONG size) {
    ULONG used = size;
    return DosDevIOCtl(port, IOCTL_ASYNC, function, parameters, size, &used, NULL, 0, NULL);
}
static APIRET configure(ULONG baud) {
    DCBINFO dcb;
    unsigned char line[3] = {8, 0, 0};
    ULONG size = DCB_WIRE_SIZE;
    APIRET rc;
    if (baud == 38400) {
        USHORT speed = 38400;
        rc = control(ASYNC_SETBAUDRATE, &speed, sizeof(speed));
    } else {
        /* Extended baud packet is a DWORD integer rate and one fraction byte. */
        unsigned char speed[5] = {0x00, 0xc2, 0x01, 0x00, 0x00};
        rc = control(ASYNC_EXTSETBAUDRATE, speed, sizeof(speed));
    }
    if (rc) return rc;
    rc = control(ASYNC_SETLINECTRL, line, sizeof(line));
    if (rc) return rc;
    memset(&dcb, 0, sizeof(dcb));
    rc = DosDevIOCtl(port, IOCTL_ASYNC, ASYNC_GETDCBINFO,
                    NULL, 0, NULL, &dcb, DCB_WIRE_SIZE, &size);
    if (rc) return rc;
    dcb.usReadTimeout = 99;    /* 1 second; driver units are 0.01 s, based on zero. */
    dcb.usWriteTimeout = 999;  /* 10 seconds, never infinite. */
    dcb.fbCtlHndShake = MODE_DTR_CONTROL;
    dcb.fbFlowReplace = MODE_RTS_CONTROL;
    dcb.fbTimeout = MODE_WAIT_READ_TIMEOUT;
    dcb.bErrorReplacementChar = 0;
    dcb.bBreakReplacementChar = 0;
    dcb.bXONChar = 0x11; dcb.bXOFFChar = 0x13;
    return control(ASYNC_SETDCBINFO, &dcb, DCB_WIRE_SIZE);
}
static int absolute_path(const char *path) {
    size_t length = strlen(path);
    return length >= 3 && length <= 259 && isalpha((unsigned char)path[0]) &&
           path[1] == ':' && (path[2] == '\\' || path[2] == '/') &&
           !strchr(path, '*') && !strchr(path, '?');
}
static APIRET reserve_output(char *path, size_t capacity) {
    while (next_output <= SERAGENT_MAX_OUTPUT) {
        HFILE file;
        ULONG action;
        FILESTATUS3 info;
        APIRET rc;
        snprintf(path, capacity, "C:\\CRAFT\\A%07lu.OUT", next_output++);
        rc = DosOpen(path, &file, &action, 0, FILE_NORMAL,
                     OPEN_ACTION_CREATE_IF_NEW | OPEN_ACTION_FAIL_IF_EXISTS,
                     OPEN_ACCESS_WRITEONLY | OPEN_SHARE_DENYREADWRITE | OPEN_FLAGS_NOINHERIT, NULL);
        if (!rc) return DosClose(file);
        /* Existing captures may still be held by an earlier child/session. */
        if (rc == ERROR_FILE_EXISTS ||
            DosQueryPathInfo(path, FIL_STANDARD, &info, sizeof(info)) == 0) continue;
        return rc;
    }
    return ERROR_TOO_MANY_OPEN_FILES;
}
static void command(char *line, int invalid) {
    ULONG first = ticks();
    int result = 1;
    const char *kind = "INVALID";
    if (sequence >= SERAGENT_MAX_SEQUENCE) {
        sendf("SERAGENT ACK 0 INVALID\r\n");
        sendf("SERAGENT ERROR 0 sequence_limit_restart_agent\r\n");
        sendf("SERAGENT DONE 0 1 0\r\n");
        stopped = 1; sequence_exhausted = 1; return;
    }
    ++sequence;
    if (!invalid) {
        if (!strncmp(line, "EXEC ", 5)) kind = "EXEC";
        else if (!strncmp(line, "GET ", 4)) kind = "GET";
        else if (!strcmp(line, "EXIT")) kind = "EXIT";
    }
    sendf("SERAGENT ACK %lu %s\r\n", sequence, kind);
    if (invalid) {
        sendf("SERAGENT ERROR %lu invalid_or_overlong_ascii_line\r\n", sequence);
    } else if (!strcmp(kind, "EXIT")) {
        stopped = 1; result = 0;
    } else if (!strcmp(kind, "EXEC")) {
        char shell[MAX_COMMAND + 64], output_path[32];
        size_t length = strlen(line + 5);
        if (!length || length > MAX_COMMAND) {
            sendf("SERAGENT ERROR %lu command_length\r\n", sequence);
        } else {
            int transfer;
            APIRET reserve_rc = reserve_output(output_path, sizeof(output_path));
            if (reserve_rc) {
                sendf("SERAGENT ERROR %lu output_reservation_rc_%lu\r\n", sequence, (ULONG)reserve_rc);
                sendf("SERAGENT DONE %lu 1 %lu\r\n", sequence, (ULONG)(ticks() - first));
                return;
            }
            /* A child can retain this stream; later EXECs always use a new name. */
            snprintf(shell, sizeof(shell), "(%s) >%s 2>&1", line + 5, output_path);
#ifdef WARP_STRESS_TEST
            result = seragent_test_system(shell);
#else
            result = system(shell);
#endif
            transfer = get_file(output_path);
            if (!result && transfer) result = 1;
        }
    } else if (!strcmp(kind, "GET")) {
        char *path = line + 4;
        size_t length = strlen(path);
        if (length >= 2 && path[0] == '"' && path[length - 1] == '"') {
            path[length - 1] = 0; ++path;
        }
        if (!absolute_path(path)) sendf("SERAGENT ERROR %lu explicit_absolute_file_path_required\r\n", sequence);
        else result = get_file(path);
    } else sendf("SERAGENT ERROR %lu unknown_command\r\n", sequence);
    sendf("SERAGENT DONE %lu %d %lu\r\n", sequence, result, (ULONG)(ticks() - first));
}
int main(int argc, char **argv) {
    APIRET rc;
    ULONG action, baud = 38400;
    char line[MAX_LINE + 1], input[64];
    unsigned used = 0;
    int invalid = 0, after_cr = 0;
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "38400") && strcmp(argv[1], "115200"))) {
        fputs("SERAGENT [38400|115200] (manually started temporary COM1 QA agent)\n", stderr);
        return 2;
    }
    if (argc == 2 && !strcmp(argv[1], "115200")) baud = 115200;
    rc = DosOpen("COM1", &port, &action, 0, FILE_NORMAL,
                 OPEN_ACTION_FAIL_IF_NEW | OPEN_ACTION_OPEN_IF_EXISTS,
                 OPEN_ACCESS_READWRITE | OPEN_SHARE_DENYNONE | OPEN_FLAGS_NOINHERIT, NULL);
    if (rc) { fprintf(stderr, "SERAGENT COM1 open failed rc=%lu\n", (ULONG)rc); return 1; }
    rc = configure(baud);
    if (rc) { fprintf(stderr, "SERAGENT COM1 configure failed rc=%lu\n", (ULONG)rc); DosClose(port); return 1; }
    signal(SIGINT, stop_signal);
#ifdef SIGBREAK
    signal(SIGBREAK, stop_signal);
#endif
    fprintf(stderr, "SERAGENT active on COM1 at %lu 8N1; EXIT or Ctrl-C stops it.\n", baud);
    sendf("SERAGENT READY COM1 %lu\r\n", baud);
    while (!stopped && !broken) {
        ULONG got = 0, i;
        rc = DosRead(port, input, sizeof(input), &got);
        if (rc) {
            sendf("SERAGENT ERROR 0 serial_read_rc_%lu\r\n", (ULONG)rc);
            fprintf(stderr, "SERAGENT COM1 read failed rc=%lu\n", (ULONG)rc);
            broken = 1; break;
        }
        if (!got) { DosSleep(10); continue; }
        for (i = 0; i < got && !stopped && !broken; ++i) {
            unsigned char c = (unsigned char)input[i];
            if (c == '\n' && after_cr) { after_cr = 0; continue; }
            after_cr = c == '\r';
            if (c == '\r' || c == '\n') {
                line[used] = 0;
                if (used || invalid) command(line, invalid);
                used = 0; invalid = 0;
            } else if (c < 32 || c > 126) invalid = 1;
            else if (used < MAX_LINE) line[used++] = (char)c;
            else invalid = 1;
        }
    }
    rc = DosClose(port);
    return broken || sequence_exhausted || rc ? 1 : 0;
}
