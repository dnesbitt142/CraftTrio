/* Presentation Manager clipboard with the platform code page and Mozilla's OS/2 atoms. */
#define INCL_DOS
#define INCL_WIN
#define INCL_GPI
#define INCL_DEV
#include <os2.h>
#include <uconv.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "clipboard.h"
#include "craftpm.h"

#define TEXT_LIMIT (16UL * 1024 * 1024)
#define PICTURE_LIMIT (64UL * 1024 * 1024)
#define FORMATS_LIMIT 8192U
#define CLIP_TEXT 1
#define CLIP_HTML 2
#define CLIP_RTF 3
#define CLIP_FORMATS 4
#define CLIP_PICTURE 5

extern HWND wc_gui_frame(void);
/* Declared in the IBM PM toolkit, but missing from the OpenWatcom header. */
extern ULONG APIENTRY WinEnumClipbrdFmts(HAB hab, ULONG format);

static uint32_t next_utf8(const unsigned char **input, const unsigned char *end)
{
    uint32_t lead = *(*input)++, value, minimum;
    unsigned int bytes, i;

    if (lead < 0x80) return lead;
    if (lead < 0xc2 || lead > 0xf4) return 0xfffd;
    bytes = lead < 0xe0 ? 1 : lead < 0xf0 ? 2 : 3;
    minimum = bytes == 1 ? 0x80 : bytes == 2 ? 0x800 : 0x10000;
    if ((size_t)(end - *input) < bytes) return 0xfffd;
    value = lead & (0x3f >> bytes);
    for (i = 0; i < bytes; ++i) {
        unsigned char next = (*input)[i];
        if ((next & 0xc0) != 0x80) return 0xfffd;
        value = (value << 6) | (next & 0x3f);
    }
    *input += bytes;
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
        return 0xfffd;
    return value;
}

static size_t from_utf16(const UniChar *input, size_t units, unsigned char *out, size_t cap)
{
    size_t i, written = 0;
    for (i = 0; i < units; ++i) {
        uint32_t ch = input[i];
        unsigned int n;
        if (ch >= 0xd800 && ch <= 0xdbff && i + 1 < units &&
            input[i + 1] >= 0xdc00 && input[i + 1] <= 0xdfff) {
            ch = 0x10000 + ((ch - 0xd800) << 10) + (input[++i] - 0xdc00);
        } else if (ch >= 0xd800 && ch <= 0xdfff) {
            ch = 0xfffd;
        }
        n = ch < 0x80 ? 1 : ch < 0x800 ? 2 : ch < 0x10000 ? 3 : 4;
        if (n > cap - written) return SIZE_MAX;
        if (out) {
            static const unsigned char prefix[] = { 0, 0, 0xc0, 0xe0, 0xf0 };
            unsigned int j;
            for (j = n - 1; j > 0; --j) {
                out[written + j] = 0x80 | (ch & 0x3f);
                ch >>= 6;
            }
            out[written] = prefix[n] | ch;
        }
        written += n;
    }
    return written;
}

static size_t to_utf16(const unsigned char *input, size_t length, UniChar *out, int crlf)
{
    const unsigned char *end = input + length;
    size_t written = 0;
    uint32_t previous = 0;
    while (input < end) {
        uint32_t ch = next_utf8(&input, end);
        if (crlf && ch == '\n' && previous != '\r') out[written++] = '\r';
        previous = ch;
        if (!ch) ch = 0xfffd;
        if (ch >= 0x10000) {
            ch -= 0x10000;
            out[written++] = (UniChar)(0xd800 | (ch >> 10));
            ch = 0xdc00 | (ch & 0x3ff);
        }
        out[written++] = (UniChar)ch;
    }
    return written;
}

/* UCONV is part of Warp 4. Keep an ASCII fallback for customized installations. */
typedef int (CALLCONV *CreateConv)(UniChar *, UconvObject *);
typedef int (CALLCONV *FreeConv)(UconvObject);
typedef int (CALLCONV *MapCp)(unsigned long, UniChar *, size_t);
typedef int (CALLCONV *FromUcs)(UconvObject, UniChar **, size_t *, void **, size_t *, size_t *);
typedef int (CALLCONV *ToUcs)(UconvObject, void **, size_t *, UniChar **, size_t *, size_t *);
typedef int (CALLCONV *QueryConv)(UconvObject, uconv_attribute_t *, size_t, char *, char *, udcrange_t *);
typedef int (CALLCONV *SetConv)(UconvObject, uconv_attribute_t *);

static struct {
    HMODULE module;
    CreateConv create;
    FreeConv release;
    MapCp map_cp;
    FromUcs from_ucs;
    ToUcs to_ucs;
    QueryConv query;
    SetConv set;
    int attempted;
} unicode;

static UconvObject platform_conversion(void)
{
    UniChar name[24];
    UconvObject conv;
    uconv_attribute_t attr;
    HWND frame = wc_gui_frame();
    ULONG cp = frame ? WinQueryCp(WinQueryWindowULong(frame, QWL_HMQ)) : 0;
    if (!unicode.attempted) {
        char error[256];
        unicode.attempted = 1;
        if (DosLoadModule(error, sizeof(error), "UCONV", &unicode.module) != 0) return NULL;
#define FIND(member, ordinal) DosQueryProcAddr(unicode.module, ordinal, NULL, (PFN *)&unicode.member)
        if (FIND(create, 1) || FIND(to_ucs, 2) || FIND(from_ucs, 3) ||
            FIND(release, 4) || FIND(query, 7) || FIND(set, 8) || FIND(map_cp, 10)) {
            DosFreeModule(unicode.module);
            unicode.module = 0;
        }
#undef FIND
    }
    if (!unicode.module || unicode.map_cp(cp, name, sizeof(name) / sizeof(*name)) ||
        unicode.create(name, &conv)) return NULL;
    if (unicode.query(conv, &attr, sizeof(attr), NULL, NULL, NULL) == 0) {
        attr.options |= UCONV_OPTION_SUBSTITUTE_BOTH;
        attr.subchar_len = attr.subuni_len = 1;
        attr.subchar[0] = '?';
        attr.subuni[0] = 0xfffd;
        unicode.set(conv, &attr);
    }
    return conv;
}

static size_t encode_platform(UniChar *text, size_t units, unsigned char *out, size_t cap)
{
    UconvObject conv;
    if (!units) return 0;
    conv = platform_conversion();
    if (conv) {
        size_t substitutions;
        size_t remaining = cap;
        void *output = out;
        int rc = unicode.from_ucs(conv, &text, &units, &output, &remaining, &substitutions);
        unicode.release(conv);
        return !rc && !units ? cap - remaining : SIZE_MAX;
    } else {
        size_t i;
        if (units > cap) return SIZE_MAX;
        for (i = 0; i < units; ++i) out[i] = text[i] < 128 ? (unsigned char)text[i] : '?';
        return units;
    }
}

static size_t decode_platform(const unsigned char *text, size_t length,
                              unsigned char *out, size_t cap)
{
    UconvObject conv;
    UniChar *wide, *output;
    size_t n, remaining, substitutions;
    void *input = (void *)text;

    if (length > TEXT_LIMIT) return SIZE_MAX;
    if (!length) return 0;
    wide = malloc(length * sizeof(*wide));
    if (!wide) return SIZE_MAX;
    conv = platform_conversion();
    if (conv) {
        remaining = length;
        output = wide;
        n = unicode.to_ucs(conv, &input, &length, &output, &remaining, &substitutions) == 0 && !length
            ? from_utf16(wide, (size_t)(output - wide), out, cap) : SIZE_MAX;
        unicode.release(conv);
    } else {
        for (n = 0; n < length; ++n) wide[n] = text[n] < 128 ? text[n] : 0xfffd;
        n = from_utf16(wide, length, out, cap);
    }
    free(wide);
    return n;
}

size_t wc_pm_encode(const unsigned char *text, size_t length, unsigned char *out, size_t cap)
{
    UniChar *wide;
    size_t n;
    if (length > TEXT_LIMIT) return SIZE_MAX;
    if (!length) return 0;
    wide = malloc(length * sizeof(*wide));
    if (!wide) return SIZE_MAX;
    n = encode_platform(wide, to_utf16(text, length, wide, 0), out, cap);
    free(wide);
    return n;
}

size_t wc_pm_decode(const unsigned char *text, size_t length, unsigned char *out, size_t cap)
{
    return decode_platform(text, length, out, cap);
}

static unsigned char *span(uint32_t offset, uint32_t length)
{
    size_t extent;
    unsigned char *base = wc_pm_memory(&extent);
    if (!base || offset > extent || length > extent - offset) return NULL;
    return base + offset;
}

static ULONG clip_atom(const char *name, int create)
{
    HATOMTBL table = WinQuerySystemAtomTable();
    ATOM atom = WinFindAtom(table, name);
    return !atom && create ? WinAddAtom(table, name) : atom;
}

static const unsigned char *clip_pointer(HAB hab, ULONG format, ULONG *bytes)
{
    ULONG flags;
    const unsigned char *ptr;
    if (!format || !WinQueryClipbrdFmtInfo(hab, format, &flags) || !(flags & CFI_POINTER)) return NULL;
    ptr = (const unsigned char *)WinQueryClipbrdData(hab, format);
    if (!ptr || DosQueryMem((PVOID)ptr, bytes, &flags) != 0 || !(flags & PAG_COMMIT) ||
        !(flags & PAG_READ) || !(flags & PAG_SHARED)) return NULL;
    return ptr;
}

static size_t read_text(HAB hab, ULONG format, unsigned char *out, size_t cap, int encoding)
{
    ULONG bytes = (TEXT_LIMIT + 1) * 2;
    const unsigned char *src = clip_pointer(hab, format, &bytes);
    const unsigned char *end;
    if (!src) return SIZE_MAX;
    if (encoding == 2 && bytes >= 2 && src[0] == '{' && !src[1]) encoding = 1;
    if (encoding == 1) {
        const UniChar *wide = (const UniChar *)src;
        ULONG i, count = bytes / sizeof(*wide);
        if ((uintptr_t)wide % sizeof(*wide)) return SIZE_MAX;
        for (i = 0; i < count; ++i) {
            if (!wide[i]) return from_utf16(wide, i, out, cap);
        }
        return SIZE_MAX;
    }
    if (bytes > TEXT_LIMIT + 1) bytes = TEXT_LIMIT + 1;
    end = memchr(src, 0, bytes);
    if (!end) return SIZE_MAX;
    if (encoding == 0) return decode_platform(src, (size_t)(end - src), out, cap);
    if ((size_t)(end - src) > cap) return SIZE_MAX;
    if (out) memcpy(out, src, (size_t)(end - src));
    return (size_t)(end - src);
}

static size_t read_formats(HAB hab, unsigned char *out, size_t cap)
{
    HATOMTBL table = WinQuerySystemAtomTable();
    ULONG format = 0;
    size_t written = 0;
    unsigned int i;
    for (i = 0; i < 64 && (format = WinEnumClipbrdFmts(hab, format)) != 0; ++i) {
        char name[128];
        size_t n;
        switch (format) {
        case CF_TEXT: strcpy(name, "CF_TEXT"); break;
        case CF_BITMAP: strcpy(name, "CF_BITMAP"); break;
        case CF_METAFILE: strcpy(name, "CF_METAFILE"); break;
        case CF_PALETTE: strcpy(name, "CF_PALETTE"); break;
        default:
            n = WinQueryAtomName(table, (ATOM)format, name, sizeof(name));
            if (!n || n >= sizeof(name)) continue;
            name[n] = 0;
        }
        n = strlen(name);
        if (n + 1 > cap - written) return SIZE_MAX;
        if (out) {
            memcpy(out + written, name, n);
            out[written + n] = '\n';
        }
        written += n + 1;
    }
    return written;
}

static size_t read_encoded(HAB hab, const char *name, unsigned char *out, size_t cap)
{
    ULONG extent = PICTURE_LIMIT + sizeof(uint32_t);
    const unsigned char *src = clip_pointer(hab, clip_atom(name, 0), &extent);
    uint32_t length;
    if (!src || extent < sizeof(length)) return SIZE_MAX;
    memcpy(&length, src, sizeof(length));
    if (!length || length > extent - sizeof(length) || length > PICTURE_LIMIT || length > cap)
        return SIZE_MAX;
    if (out) memcpy(out, src + sizeof(length), length);
    return length;
}

static size_t read_bitmap(HAB hab, unsigned char *out, size_t cap)
{
    /* Supply a standard BMP file to the same bounded importer used for PNG and JPEG. */
    HBITMAP bitmap, previous;
    BITMAPINFOHEADER2 header;
    BITMAPINFO2 info;
    HDC dc;
    HPS ps;
    SIZEL size = { 0, 0 };
    ULONG flags, stride, total, value;
    size_t result = SIZE_MAX;

    if (!WinQueryClipbrdFmtInfo(hab, CF_BITMAP, &flags) || !(flags & CFI_HANDLE))
        return SIZE_MAX;
    bitmap = (HBITMAP)WinQueryClipbrdData(hab, CF_BITMAP);
    memset(&header, 0, sizeof(header));
    header.cbFix = sizeof(header);
    if (!bitmap || !GpiQueryBitmapInfoHeader(bitmap, &header) ||
        !header.cx || !header.cy || header.cx > 16000 || header.cy > 16000) return SIZE_MAX;
    stride = (header.cx * 3 + 3) & ~3UL;
    if (header.cy > (PICTURE_LIMIT - 54) / stride) return SIZE_MAX;
    total = 54 + stride * header.cy;
    if (total > cap) return SIZE_MAX;
    if (!out) return total;

    dc = DevOpenDC(hab, OD_MEMORY, "*", 0, NULL, NULLHANDLE);
    if (!dc) return SIZE_MAX;
    ps = GpiCreatePS(hab, dc, &size, PU_PELS | GPIF_DEFAULT | GPIT_MICRO | GPIA_ASSOC);
    if (ps) {
        previous = GpiSetBitmap(ps, bitmap);
        if (previous != HBM_ERROR) {
            memset(&info, 0, sizeof(info));
            info.cbFix = sizeof(BITMAPINFOHEADER2);
            info.cx = header.cx;
            info.cy = header.cy;
            info.cPlanes = 1;
            info.cBitCount = 24;
            if (GpiQueryBitmapBits(ps, 0, header.cy, out + 54, &info) == (LONG)header.cy) {
                memset(out, 0, 54);
                out[0] = 'B';
                out[1] = 'M';
                memcpy(out + 2, &total, 4);
                value = 54;
                memcpy(out + 10, &value, 4);
                value = 40;
                memcpy(out + 14, &value, 4);
                memcpy(out + 18, &header.cx, 4);
                memcpy(out + 22, &header.cy, 4);
                out[26] = 1;
                out[28] = 24;
                result = total;
            }
            GpiSetBitmap(ps, previous);
        }
        GpiDestroyPS(ps);
    }
    DevCloseDC(dc);
    return result;
}

uint32_t wc_clipboard_read(uint32_t kind,
                                      uint32_t offset, uint32_t capacity)
{
    HAB hab = WinQueryAnchorBlock(wc_gui_frame());
    unsigned char *out = capacity ? span(offset, capacity) : NULL;
    size_t result = SIZE_MAX, limit = kind == CLIP_PICTURE ? PICTURE_LIMIT :
                                 kind == CLIP_FORMATS ? FORMATS_LIMIT : TEXT_LIMIT;
    size_t cap = capacity ? capacity : limit;
    if (!hab || (capacity && !out) || cap > limit || !WinOpenClipbrd(hab)) return UINT32_MAX;
    switch (kind) {
    case CLIP_TEXT:
        result = read_text(hab, clip_atom("text/unicode", 0), out, cap, 1);
        if (result == SIZE_MAX) result = read_text(hab, CF_TEXT, out, cap, 0);
        break;
    case CLIP_HTML:
        result = read_text(hab, clip_atom("text/html", 0), out, cap, 1);
        break;
    case CLIP_RTF:
        result = read_text(hab, clip_atom("Rich Text Format", 0), out, cap, 2);
        if (result == SIZE_MAX) result = read_text(hab, clip_atom("text/rtf", 0), out, cap, 2);
        break;
    case CLIP_FORMATS:
        result = read_formats(hab, out, cap);
        break;
    case CLIP_PICTURE: {
        static const char *formats[] = { "image/png", "image/jpeg", "image/jpg", "image/gif", "image/webp" };
        unsigned int i;
        for (i = 0; i < sizeof(formats) / sizeof(*formats) && result == SIZE_MAX; ++i)
            result = read_encoded(hab, formats[i], out, cap);
        if (result == SIZE_MAX) result = read_bitmap(hab, out, cap);
        break;
    }
    }
    WinCloseClipbrd(hab);
    return result == SIZE_MAX ? UINT32_MAX : (uint32_t)result;
}

typedef struct {
    ULONG format;
    void *data;
} ClipData;

static void *allocate_shared(size_t length)
{
    void *ptr;
    if (DosAllocSharedMem(&ptr, NULL, length, PAG_READ | PAG_WRITE | PAG_COMMIT | OBJ_GIVEABLE))
        return NULL;
    return ptr;
}

static void *wide_shared(const unsigned char *bytes, size_t length, size_t *units)
{
    UniChar *wide = allocate_shared((2 * length + 1) * sizeof(*wide));
    if (wide) {
        *units = to_utf16(bytes, length, wide, 1);
        wide[*units] = 0;
    }
    return wide;
}

uint32_t wc_clipboard_write(uint32_t text, uint32_t text_len,
                                       uint32_t html, uint32_t html_len, uint32_t math, uint32_t math_len)
{
    HWND frame = wc_gui_frame();
    HAB hab = WinQueryAnchorBlock(frame);
    const unsigned char *plain, *formatted, *formula;
    ClipData data[4] = { { 0, NULL } };
    size_t units = 0, n, ansi_cap;
    uint32_t result = 0;
    unsigned int i;

    if (!hab || text_len > TEXT_LIMIT || html_len > TEXT_LIMIT || math_len > TEXT_LIMIT) return 0;
    plain = span(text, text_len);
    formatted = html_len ? span(html, html_len) : NULL;
    formula = math_len ? span(math, math_len) : NULL;
    if (!plain || (html_len && !formatted) || (math_len && !formula)) return 0;

    data[0].format = clip_atom("text/unicode", 1);
    data[0].data = wide_shared(plain, text_len, &units);
    if (!data[0].format || !data[0].data) goto done;
    data[1].format = CF_TEXT;
    ansi_cap = 4 * units;
    data[1].data = allocate_shared(ansi_cap + 1);
    if (data[1].data) {
        n = encode_platform(data[0].data, units, data[1].data, ansi_cap);
        if (n != SIZE_MAX) ((unsigned char *)data[1].data)[n] = 0;
        else { DosFreeMem(data[1].data); data[1].data = NULL; }
    }
    if (html_len) {
        data[2].format = clip_atom("text/html", 1);
        data[2].data = wide_shared(formatted, html_len, &units);
        if (!data[2].format || !data[2].data) goto done;
    }
    if (math_len) {
        data[3].format = clip_atom("application/mathml+xml", 1);
        data[3].data = allocate_shared(math_len + sizeof(math_len));
        if (data[3].format && data[3].data) {
            memcpy(data[3].data, &math_len, sizeof(math_len));
            memcpy((unsigned char *)data[3].data + sizeof(math_len), formula, math_len);
        }
    }
    if (!WinOpenClipbrd(hab)) goto done;
    if (WinEmptyClipbrd(hab)) {
        uint32_t plain_set = 0, html_set = 0;
        for (i = 0; i < sizeof(data) / sizeof(*data); ++i) {
            if (data[i].format && data[i].data &&
                WinSetClipbrdData(hab, (ULONG)data[i].data, data[i].format, CFI_POINTER)) {
                data[i].data = NULL; /* Presentation Manager owns it now. */
                if (i < 2) plain_set = 1;
                if (i == 2) html_set = 1;
            }
        }
        /* Success also when the plain fallback was set: an egui copy would empty it again. */
        result = plain_set || html_set;
    }
    WinCloseClipbrd(hab);

done:
    for (i = 0; i < sizeof(data) / sizeof(*data); ++i)
        if (data[i].data) DosFreeMem(data[i].data);
    return result;
}

uint32_t wc_clipboard_image(uint32_t offset,
                                       uint32_t width, uint32_t height)
{
    HAB hab = WinQueryAnchorBlock(wc_gui_frame());
    HDC dc;
    HPS ps;
    HBITMAP bitmap = NULLHANDLE;
    SIZEL size = { 0, 0 };
    BITMAPINFOHEADER2 header;
    BITMAPINFO2 info;
    unsigned char *bits;
    const unsigned char *pixels;
    uint32_t stride, x, y, result = 0;

    if (!hab || !width || !height || width > 16000 || height > 16000 ||
        height > PICTURE_LIMIT / (4 * width)) return 0;
    pixels = span(offset, width * height * 4);
    if (!pixels) return 0;
    stride = (width * 3 + 3) & ~3U;
    bits = calloc(height, stride);
    if (!bits) return 0;
    for (y = 0; y < height; ++y) {
        const unsigned char *source = pixels + 4 * width * (height - y - 1);
        unsigned char *target = bits + y * stride;
        for (x = 0; x < width; ++x) {
            unsigned int alpha = source[3], white = (255 - alpha) * 255;
            target[0] = (source[2] * alpha + white + 127) / 255;
            target[1] = (source[1] * alpha + white + 127) / 255;
            target[2] = (source[0] * alpha + white + 127) / 255;
            source += 4;
            target += 3;
        }
    }
    dc = DevOpenDC(hab, OD_MEMORY, "*", 0, NULL, NULLHANDLE);
    if (dc) {
        ps = GpiCreatePS(hab, dc, &size, PU_PELS | GPIF_DEFAULT | GPIT_MICRO | GPIA_ASSOC);
        if (ps) {
            memset(&header, 0, sizeof(header));
            header.cbFix = sizeof(header);
            header.cx = width;
            header.cy = height;
            header.cPlanes = 1;
            header.cBitCount = 24;
            memset(&info, 0, sizeof(info));
            memcpy(&info, &header, sizeof(header));
            bitmap = GpiCreateBitmap(ps, &header, CBM_INIT, bits, &info);
            GpiDestroyPS(ps);
        }
        DevCloseDC(dc);
    }
    free(bits);
    if (bitmap && bitmap != HBM_ERROR) {
        if (WinOpenClipbrd(hab)) {
            result = WinEmptyClipbrd(hab) && WinSetClipbrdData(hab, bitmap, CF_BITMAP, CFI_HANDLE);
            WinCloseClipbrd(hab);
        }
        if (!result) GpiDeleteBitmap(bitmap);
    }
    return result;
}
