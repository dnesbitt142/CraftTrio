/* Print through Presentation Manager's standard graphics spooler. */
#define INCL_WIN
#define INCL_GPI
#define INCL_DEV
#define INCL_SPL
#define INCL_SPLDOSPRINT
#include <os2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gui_print.h"
#include "craftpm.h"

#define MAX_QUEUE_BYTES 65535UL
#define MAX_RASTER_SIDE 2001UL

/* Dynamic job properties use this ABI in the Warp 4.5 Toolkit (pmdjp.h). */
typedef struct {
    ULONG size, property;
    LONG type;
    ULONG count, value;
} WcJobProperty;

enum {
    DJP_ORIENTATION = 1, DJP_PAPERSIZE = 6, DJP_DUPLEX = 13, DJP_COPIES = 16,
    DJP_CURRENT = 2, DJP_DUP_OFF = 1, DJP_DUP_BOOK = 2, DJP_DUP_FLIP = 3
};

static struct {
    HDC dc;
    HPS ps;
    LONG caps[6];
    unsigned int pages;
    int grayscale, rotate, scale_to_form;
    int duplex;
} job;

extern HWND wc_gui_frame(void);

static HAB anchor(void)
{
    return WinQueryAnchorBlock(wc_gui_frame());
}

int32_t wc_print_queues(uint8_t *output, uint32_t capacity)
{
    uint8_t *buffer;
    ULONG count = 0, total = 0, bytes = 0, i;
    uint32_t used = 0;
    SPLERR status;
    PPRQINFO3 queues;
    status = SplEnumQueue(NULL, 3, NULL, 0, &count, &total, &bytes, NULL);
    if (!bytes && !total) return status == 0 ? 0 : -1;
    if (!bytes || bytes > MAX_QUEUE_BYTES) return -1;
    buffer = malloc(bytes);
    if (!buffer) return -1;
    status = SplEnumQueue(NULL, 3, buffer, bytes, &count, &total, &bytes, NULL);
    if (status != 0) {
        free(buffer);
        return -1;
    }
    queues = (PPRQINFO3)buffer;
    for (i = 0; i < count; ++i) {
        const char *name = queues[i].pszName, *label = queues[i].pszComment;
        size_t name_len = name ? strlen(name) : 0, label_len = label ? strlen(label) : 0;
        if (!name_len || name_len > 255 || !queues[i].pszDriverName || !queues[i].pszDriverName[0])
            continue;
        if (label_len > 1024) label_len = 1024;
        if (4 + name_len + label_len > capacity - used) {
            free(buffer);
            return -1;
        }
        output[used++] = (queues[i].fsType & PRQ3_TYPE_APPDEFAULT) != 0;
        output[used++] = (uint8_t)name_len;
        output[used++] = (uint8_t)(label_len & 255);
        output[used++] = (uint8_t)(label_len >> 8);
        memcpy(output + used, name, name_len);
        used += name_len;
        if (label_len) {
            memcpy(output + used, label, label_len);
            used += label_len;
        }
    }
    free(buffer);
    return (int32_t)used;
}

static void close_job(void)
{
    if (job.ps) GpiDestroyPS(job.ps);
    if (job.dc) DevCloseDC(job.dc);
    memset(&job, 0, sizeof(job));
}

/* Work on a separate driver data block. On older drivers the printer's
 * installed settings remain usable and orientation is handled by rotation. */
static int set_property(HDC info, PDRIVDATA data, ULONG property, ULONG value)
{
    WcJobProperty request[2];
    LONG capacity = data->cb, length = capacity, result;
    PDRIVDATA next = malloc(capacity);
    int ok;
    if (!next) return 0;
    memcpy(next, data, capacity);
    memset(request, 0, sizeof(request));
    request[0].size = request[1].size = sizeof(request[0]);
    request[0].property = property;
    request[0].type = DJP_CURRENT;
    request[0].count = request[1].count = 1;
    request[0].value = value;
    result = DevEscape(info, DEVESC_SETJOBPROPERTIES, sizeof(request),
                       (PBYTE)request, &length, (PBYTE)next);
    ok = (result == DEV_OK || result == DEV_WARNING) && request[0].type >= 0 &&
         next->cb > 0 && next->cb <= capacity && length <= capacity;
    if (ok) memcpy(data, next, next->cb);
    free(next);
    return ok;
}

static ULONG query_property(HDC info, PDRIVDATA data, ULONG property)
{
    WcJobProperty request[2];
    LONG length = data->cb, result;
    memset(request, 0, sizeof(request));
    request[0].size = request[1].size = sizeof(request[0]);
    request[0].property = property;
    request[0].type = DJP_CURRENT;
    request[0].count = request[1].count = 1;
    request[0].value = 0xffffffffUL;
    result = DevEscape(info, DEVESC_QUERYJOBPROPERTIES, sizeof(request),
                       (PBYTE)request, &length, (PBYTE)data);
    if ((result == DEV_OK || result == DEV_WARNING) && request[0].type > 0)
        return request[0].value;
    return 0xffffffffUL;
}

int32_t wc_print_start(const char *name, uint32_t length, uint32_t orientation,
                       uint32_t paper, uint32_t duplex, uint32_t grayscale)
{
    ULONG bytes = 0, property, applied;
    int32_t status = -2;
    char queue[256], driver[256], *separator;
    char title[96];
    DEVOPENSTRUC options;
    PPRQINFO3 entry;
    PDRIVDATA properties = NULL;
    HDC info = NULLHANDLE;
    HPS info_ps = NULLHANDLE;
    SIZEL size = {0, 0};
    HAB hab = anchor();
    if (job.dc || !length || length >= sizeof(queue) || memchr(name, 0, length) ||
        orientation < 1 || orientation > 2 || duplex > 3 || !hab)
        return -1;
    snprintf(title, sizeof(title), "%s document", wc_pm_config()->title);
    memcpy(queue, name, length);
    queue[length] = 0;
    SplQueryQueue(NULL, queue, 3, NULL, 0, &bytes);
    if (!bytes || bytes > MAX_QUEUE_BYTES) return -2;
    entry = malloc(bytes);
    if (!entry) return -2;
    if (SplQueryQueue(NULL, queue, 3, entry, bytes, &bytes) != 0 ||
        !entry->pszDriverName || strlen(entry->pszDriverName) >= sizeof(driver)) goto done;
    strcpy(driver, entry->pszDriverName);
    separator = strchr(driver, '.');
    if (separator) *separator = 0;
    if (!driver[0]) goto done;
    memset(&options, 0, sizeof(options));
    options.pszLogAddress = queue;
    options.pszDriverName = driver;
    options.pdriv = entry->pDriverData;
    options.pszDataType = "PM_Q_STD";
    /* The application has already arranged the requested copies in page order. */
    options.pszQueueProcParams = "COP=1";
    status = -3;
    if (entry->pDriverData && entry->pDriverData->cb > 0 &&
        (ULONG)entry->pDriverData->cb <= MAX_QUEUE_BYTES) {
        properties = malloc(entry->pDriverData->cb);
        if (!properties) goto done;
        memcpy(properties, entry->pDriverData, entry->pDriverData->cb);
        info = DevOpenDC(hab, OD_INFO, "*", 3, (PDEVOPENDATA)&options, NULLHANDLE);
        if (info) info_ps = GpiCreatePS(hab, info, &size, PU_PELS | GPIA_ASSOC);
    }
    if (info_ps) {
        /* A driver may reject a property it doesn't implement. In that
         * case preserve the defaults and report explicit unsupported choices. */
        set_property(info, properties, DJP_COPIES, 1);
        set_property(info, properties, DJP_ORIENTATION, orientation);
        if (paper) {
            set_property(info, properties, DJP_PAPERSIZE, paper);
            if (query_property(info, properties, DJP_PAPERSIZE) != paper) {
                status = -4;
                goto done;
            }
        }
        property = duplex;
        if (orientation == 2 && duplex >= DJP_DUP_BOOK)
            property = duplex == DJP_DUP_BOOK ? DJP_DUP_FLIP : DJP_DUP_BOOK;
        if (duplex) set_property(info, properties, DJP_DUPLEX, property);
        applied = query_property(info, properties, DJP_DUPLEX);
        job.duplex = applied == DJP_DUP_BOOK || applied == DJP_DUP_FLIP;
        if ((duplex >= DJP_DUP_BOOK && applied != property) ||
            (duplex == DJP_DUP_OFF && job.duplex)) {
            status = -5;
            goto done;
        }
        options.pdriv = properties;
    } else if (paper || duplex >= DJP_DUP_BOOK) {
        status = paper ? -4 : -5;
        goto done;
    }
    job.dc = DevOpenDC(hab, OD_QUEUED, "*", 9, (PDEVOPENDATA)&options, NULLHANDLE);
    if (!job.dc || !DevQueryCaps(job.dc, CAPS_WIDTH, 6, job.caps) ||
        job.caps[0] < 1 || job.caps[1] < 1 || job.caps[0] > 100000 || job.caps[1] > 100000 ||
        job.caps[4] < 1 || job.caps[5] < 1)
        goto done;
    job.ps = GpiCreatePS(hab, job.dc, &size, PU_PELS | GPIA_ASSOC);
    if (!job.ps || DevEscape(job.dc, DEVESC_STARTDOC, strlen(title) + 1, (PBYTE)title, NULL, NULL) != DEV_OK)
        goto done;
    job.grayscale = !!grayscale;
    job.scale_to_form = paper != 0;
    job.rotate = (orientation == 2) !=
                 ((double)job.caps[0] / job.caps[4] > (double)job.caps[1] / job.caps[5]);
    status = job.duplex;
done:
    if (info_ps) GpiDestroyPS(info_ps);
    if (info) DevCloseDC(info);
    free(properties);
    free(entry);
    if (status < 0) close_job();
    return status;
}

int32_t wc_print_page(const uint8_t *rgba, uint32_t width, uint32_t height,
                      uint32_t width_millipoints, uint32_t height_millipoints)
{
    uint32_t w, h, row, y, x, band, first, last;
    LONG print_width, print_height, left, bottom;
    double target_width, target_height, fit;
    uint8_t *bgr;
    BITMAPINFO2 bitmap;
    POINTL points[4];
    if (!job.ps || job.pages >= 5000 || width > MAX_RASTER_SIDE || height > MAX_RASTER_SIDE)
        return -1;
    if (WinGetPhysKeyState(HWND_DESKTOP, 1) & 0x8000) return -2;
    if (rgba && (!width || !height || !width_millipoints || !height_millipoints)) return -1;
    if (job.pages && DevEscape(job.dc, DEVESC_NEWFRAME, 0, NULL, NULL, NULL) != DEV_OK)
        return -3;
    ++job.pages;
    if (!rgba) return 0;
    w = job.rotate ? height : width;
    h = job.rotate ? width : height;
    target_width = (job.rotate ? height_millipoints : width_millipoints) * (0.0254 / 72000.0) * job.caps[4];
    target_height = (job.rotate ? width_millipoints : height_millipoints) * (0.0254 / 72000.0) * job.caps[5];
    fit = (double)job.caps[0] / target_width;
    if (fit > (double)job.caps[1] / target_height) fit = (double)job.caps[1] / target_height;
    if (!job.scale_to_form && fit > 1.0) fit = 1.0;
    print_width = (LONG)(target_width * fit + 0.5);
    print_height = (LONG)(target_height * fit + 0.5);
    if (print_width < 1 || print_height < 1) return -1;
    left = (job.caps[0] - print_width) / 2;
    bottom = (job.caps[1] - print_height) / 2;
    row = (3 * w + 3) & ~3UL;
    band = 60000 / row;
    bgr = malloc(band * row);
    if (!bgr) return -1;
    memset(&bitmap, 0, sizeof(bitmap));
    bitmap.cbFix = sizeof(BITMAPINFOHEADER2);
    bitmap.cx = w;
    bitmap.cPlanes = 1;
    bitmap.cBitCount = 24;
    points[0].x = left;
    points[1].x = left + print_width - 1;
    points[2].x = 0;
    points[2].y = 0;
    points[3].x = w;
    for (first = 0; first < h; first = last) {
        if (WinGetPhysKeyState(HWND_DESKTOP, 1) & 0x8000) {
            free(bgr);
            return -2;
        }
        last = first + band < h ? first + band : h;
        points[0].y = bottom + (LONG)((uint32_t)print_height * first / h);
        points[1].y = bottom + (LONG)((uint32_t)print_height * last / h) - 1;
        if (points[1].y < points[0].y) continue;
        memset(bgr, 0, row * (last - first));
        for (y = first; y < last; ++y) {
            uint8_t *dst = bgr + row * (y - first);
            for (x = 0; x < w; ++x) {
                const uint8_t *src = job.rotate
                    ? rgba + ((height - 1 - x) * width + (width - 1 - y)) * 4
                    : rgba + ((height - 1 - y) * width + x) * 4;
                if (job.grayscale) {
                    uint8_t gray = (uint8_t)((77U * src[0] + 150U * src[1] + 29U * src[2] + 128U) >> 8);
                    *dst++ = gray; *dst++ = gray; *dst++ = gray;
                } else {
                    *dst++ = src[2]; *dst++ = src[1]; *dst++ = src[0];
                }
            }
        }
        bitmap.cy = last - first;
        points[3].y = bitmap.cy;
        if (GpiDrawBits(job.ps, bgr, &bitmap, 4, points, ROP_SRCCOPY, BBO_IGNORE) == GPI_ERROR) {
            free(bgr);
            return -3;
        }
    }
    free(bgr);
    return 0;
}

int32_t wc_print_finish(int cancel)
{
    LONG status, length;
    USHORT id = 0;
    if (!job.dc) return -1;
    if (cancel || !job.pages) {
        status = DevEscape(job.dc, DEVESC_ABORTDOC, 0, NULL, NULL, NULL);
        close_job();
        return cancel && status == DEV_OK ? 0 : -1;
    }
    length = sizeof(id);
    status = DevEscape(job.dc, DEVESC_ENDDOC, 0, NULL, &length, (PBYTE)&id);
    if (status != DEV_OK) DevEscape(job.dc, DEVESC_ABORTDOC, 0, NULL, NULL, NULL);
    close_job();
    return status == DEV_OK ? (int32_t)id : -1;
}
