#define INCL_WIN
#define INCL_DOS
#include <os2.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "clipboard.h"

static HWND frame;
static unsigned char engine[8192];
static FILE *logfile;
static unsigned int failures;
struct w2c_wc__host;
extern uint32_t w2c_wc__host_clipboard_read(struct w2c_wc__host *, uint32_t, uint32_t, uint32_t);
extern uint32_t w2c_wc__host_clipboard_write(struct w2c_wc__host *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
extern uint32_t w2c_wc__host_clipboard_image(struct w2c_wc__host *, uint32_t, uint32_t, uint32_t);

HWND wc_gui_frame(void) { return frame; }

uint8_t *wc_reactor_memory(size_t *size)
{
    *size = sizeof(engine);
    return engine;
}

static void check(int outcome, const char *name)
{
    fprintf(logfile, "%s %s\n", outcome ? "PASS" : "FAIL", name);
    if (!outcome) ++failures;
    fflush(logfile);
}

static void read_probe(uint32_t kind, const char *value, const char *name)
{
    size_t n = w2c_wc__host_clipboard_read(NULL, kind, 0, 0);
    int ok = n == strlen(value) && w2c_wc__host_clipboard_read(NULL, kind, 0, n) == n &&
             !memcmp(engine, value, n);
    if (!ok) fprintf(logfile, "reported length=%lu expected=%lu\n", (unsigned long)n, (unsigned long)strlen(value));
    check(ok, name);
}

int main(int argc, char **argv)
{
    const char utf8[] = "WarpClip caf\xc3\xa9 \xf0\x9f\x98\x80";
    const unsigned char native[] = "WarpClip caf\x82";
    const char html[] = "<p><b>caf\xc3\xa9</b></p>";
    const char rtf[] = "{\\rtf1\\ansi {\\b WarpClip}}";
    static const unsigned char colors[16] = {
        255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255
    };
    unsigned char encoded[80], decoded[80];
    HAB hab = WinInitialize(0);
    HMQ queue = WinCreateMsgQueue(hab, 0);
    int length;
    unsigned char *data;
    ULONG flags, region;
    HATOMTBL atoms;
    ULONG format;

    frame = WinCreateWindow(HWND_DESKTOP, WC_STATIC, "", 0, 0, 0, 1, 1,
                            HWND_DESKTOP, HWND_TOP, 100, NULL, NULL);
    logfile = fopen("A:\\CLIPTEST.LOG", argc > 1 && argv[1][0] == 'W' ? "w" : "a");
    if (!logfile || !frame) return 9;
    if (argc > 1 && argv[1][0] == 'W') {
        length = wc_pm_encode((const unsigned char *)utf8, sizeof(utf8) - 1, encoded, sizeof(encoded));
        fprintf(logfile, "PM cp=%lu encoded length=%d bytes=", WinQueryCp(queue), length);
        for (unsigned int i = 0; i < (unsigned int)length && i < sizeof(encoded); ++i) fprintf(logfile, "%02x ", encoded[i]);
        fputc('\n', logfile);
        check(length >= (int)sizeof(native) - 1 && !memcmp(encoded, native, sizeof(native) - 1), "active code-page encode");
        length = wc_pm_decode(native, sizeof(native) - 1, decoded, sizeof(decoded));
        check(length == 14 && !memcmp(decoded, utf8, length), "active code-page decode");
        memcpy(engine, utf8, sizeof(utf8) - 1);
        memcpy(engine + 64, html, sizeof(html) - 1);
        check(w2c_wc__host_clipboard_write(NULL, 0, sizeof(utf8) - 1, 64, sizeof(html) - 1, 0, 0), "PM write");
    } else {
        read_probe(1, utf8, "cross-process Unicode read");
        read_probe(2, html, "cross-process HTML read");
        if (WinOpenClipbrd(hab)) {
            data = (unsigned char *)WinQueryClipbrdData(hab, CF_TEXT);
            region = 64;
            flags = 0;
            if (data) DosQueryMem(data, &region, &flags);
            fprintf(logfile, "CF_TEXT region=%lu flags=%lx\n", region, flags);
            check(data && !memcmp(data, native, sizeof(native) - 1), "other app reads CF_TEXT");
            WinCloseClipbrd(hab);
        }
        if (WinOpenClipbrd(hab)) {
            atoms = WinQuerySystemAtomTable();
            format = WinAddAtom(atoms, "Rich Text Format");
            WinEmptyClipbrd(hab);
            if (!DosAllocSharedMem((PVOID *)&data, NULL, sizeof(native), PAG_COMMIT | PAG_READ | PAG_WRITE | OBJ_GIVEABLE)) {
                memcpy(data, native, sizeof(native));
                check(WinSetClipbrdData(hab, (ULONG)data, CF_TEXT, CFI_POINTER), "other app sets CF_TEXT");
            }
            if (!DosAllocSharedMem((PVOID *)&data, NULL, sizeof(rtf), PAG_COMMIT | PAG_READ | PAG_WRITE | OBJ_GIVEABLE)) {
                memcpy(data, rtf, sizeof(rtf));
                check(WinSetClipbrdData(hab, (ULONG)data, format, CFI_POINTER), "other app sets RTF");
            }
            WinCloseClipbrd(hab);
        }
        read_probe(1, "WarpClip caf\xc3\xa9", "CF_TEXT import");
        read_probe(3, rtf, "RTF import");
        length = w2c_wc__host_clipboard_read(NULL, 4, 0, 0);
        check(length > 0 && length < sizeof(engine) &&
              w2c_wc__host_clipboard_read(NULL, 4, 0, length) == length, "native format enumeration");
        memcpy(engine, colors, sizeof(colors));
        check(w2c_wc__host_clipboard_image(NULL, 0, 2, 2), "native bitmap output");
        length = w2c_wc__host_clipboard_read(NULL, 5, 0, 0);
        check(length == 70, "native bitmap size");
        check(w2c_wc__host_clipboard_read(NULL, 5, 0, 70) == 70 &&
              engine[0] == 'B' && engine[1] == 'M' &&
              engine[54] == 255 && !engine[55] && !engine[56] &&
              !engine[62] && !engine[63] && engine[64] == 255, "native bitmap colors and orientation");
    }
    fprintf(logfile, "EXIT failures=%u\n", failures);
    fclose(logfile);
    WinDestroyWindow(frame);
    WinDestroyMsgQueue(queue);
    WinTerminate(hab);
    return failures ? 1 : 0;
}
