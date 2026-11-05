/* Explicit-file, bounded serial-friendly extraction; redirect stdout to COM1. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static void path_hex(const char *s) {
    while (*s) printf("%02X", (unsigned char)*s++);
}
static void line64(const unsigned char *p, size_t n) {
    static const char b[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i;
    for (i = 0; i < n; i += 3) {
        unsigned long v = (unsigned long)p[i] << 16;
        if (i + 1 < n) v |= (unsigned long)p[i+1] << 8;
        if (i + 2 < n) v |= p[i+2];
        putchar(b[(v >> 18) & 63]); putchar(b[(v >> 12) & 63]);
        putchar(i + 1 < n ? b[(v >> 6) & 63] : '=');
        putchar(i + 2 < n ? b[v & 63] : '=');
    }
    putchar('\n');
}
int main(int argc, char **argv) {
    int k, failed = 0;
    if (argc < 2 || argc > 33) {
        fputs("GUESTOUT file [file ...] (1..32 explicit paths, max64MiB/file)\n", stderr);
        return 2;
    }
    for (k = 1; k < argc; ++k) {
        FILE *f = fopen(argv[k], "rb");
        long length = -1;
        unsigned long total = 0;
        unsigned char block[48];
        size_t got;
        if (f && !fseek(f, 0, SEEK_END)) length = ftell(f);
        if (!f || length < 0 || length > 67108864L || fseek(f, 0, SEEK_SET)) {
            fputs("GUESTOUT-ERROR ", stdout); path_hex(argv[k]);
            printf(" %d\n", errno); fflush(stdout);
            if (f) fclose(f);
            failed = 1; continue;
        }
        printf("GUESTOUT-BEGIN %ld ", length); path_hex(argv[k]); putchar('\n');
        while ((got = fread(block, 1, sizeof(block), f)) != 0) {
            total += (unsigned long)got;
            if (total > (unsigned long)length) break;
            line64(block, got);
        }
        if (ferror(f) || total != (unsigned long)length) {
            puts("GUESTOUT-ABORT"); failed = 1;
        } else printf("GUESTOUT-END %lu\n", total);
        fclose(f); fflush(stdout);
        if (ferror(stdout)) return 1;
    }
    return failed;
}
