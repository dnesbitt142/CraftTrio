/* Standalone native printer test for WordCraft's production GPI backend. */
#define INCL_WIN
#include <os2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gui_print.h"

static HWND frame;
HWND wc_gui_frame(void) { return frame; }

int main(int argc, char **argv)
{
    HAB hab = WinInitialize(0);
    HMQ queue = WinCreateMsgQueue(hab, 0);
    static unsigned char output[65535];
    unsigned char *pixels, *pixel;
    int size, status, y, x, p;
    unsigned int orientation = argc > 1 ? atoi(argv[1]) : 1;
    unsigned int paper = argc > 2 ? atoi(argv[2]) : 0;
    unsigned int duplex = argc > 3 ? atoi(argv[3]) : 0;
    unsigned int gray = argc > 4 ? atoi(argv[4]) : 0;
    frame = WinCreateWindow(HWND_DESKTOP, WC_STATIC, "WordCraft printer probe", 0,
                            0, 0, 0, 0, HWND_DESKTOP, HWND_BOTTOM, 0, NULL, NULL);
    freopen("WCPRINT.LOG", "w", stdout);
    size = wc_print_queues(output, sizeof(output));
    printf("Queues: %d bytes\n", size);
    printf("Settings: %u %u %u %u\n", orientation, paper, duplex, gray);
    if (size > 4 && output[1] != 0) {
        printf("Queue: %.*s\n", output[1], output + 4);
        status = wc_print_start((const char *)output + 4, output[1], orientation, paper, duplex, gray);
        printf("Start: %d\n", status);
        fflush(stdout);
        if (status >= 0) {
            pixels = malloc(612 * 792 * 4);
            if (!pixels) { wc_print_finish(1); return 3; }
            memset(pixels, 255, 612 * 792 * 4);
            for (p = 0; p < 2; ++p) {
                for (y = 72; y < 288; ++y)
                    for (x = 72; x < 288; ++x) {
                        pixel = pixels + (y * 612 + x) * 4;
                        pixel[0] = p ? 30 : 245;
                        pixel[1] = x < 180 ? 35 : 150;
                        pixel[2] = p ? 225 : 35;
                    }
                status = wc_print_page(pixels, 612, 792, 612000, 792000);
                printf("Page %d: %d\n", p + 1, status);
                fflush(stdout);
                if (status) break;
            }
            printf("Finish: %d\n", wc_print_finish(status != 0));
            free(pixels);
        }
    }
    WinDestroyWindow(frame);
    WinDestroyMsgQueue(queue);
    WinTerminate(hab);
    fclose(stdout);
    return 0;
}
