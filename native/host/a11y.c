/* Native navigator for the application semantic snapshot. */
#define INCL_WIN
#include <os2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "a11y.h"
#include "clipboard.h"
#include "craftpm.h"

#define MAX_PACKET (2UL * 1024 * 1024)
#define MAX_ROWS 7000
#define MAX_LABEL 1024
#define MAX_VALUE (32UL * 1024)
#define HEADER_SIZE 16
#define ROW_SIZE 24
#define CLICK 1
#define FOCUS 2
#define DISABLED 4
#define FOCUSED 8
#define DOCUMENT 16
#define SET_ENABLED 20
#define ACTIVATE 21
#define ID_ITEMS 600
#define ID_READ 601
#define ID_ACTIVATE 602
#define ID_EDITOR 603
#define ID_CLOSE 604
#define ID_HELP 605

typedef struct Row {
    const uint8_t *label, *detail;
    uint32_t low, high, flags, depth, label_len, detail_len;
} Row;

static struct Panel {
    HWND frame, client, owner, editor, help, items, read, activate, back, close;
    HWND tab_order[5];
    PFNWP tab_proc[5];
    WcA11yEvent dispatch;
    uint8_t *packet;
    size_t length;
    size_t offsets[MAX_ROWS];
    size_t rows;
    LONG cursor_x, cursor_y, cursor_h;
    BOOL busy, cursor;
} panel;
static BOOL class_registered;

static uint32_t word(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static BOOL read_row(const uint8_t *packet, size_t length, size_t offset, Row *row)
{
    const uint8_t *p;
    if (offset > length || length - offset < ROW_SIZE) return FALSE;
    p = packet + offset;
    row->low = word(p);
    row->high = word(p + 4);
    row->flags = word(p + 8);
    row->depth = word(p + 12);
    row->label_len = word(p + 16);
    row->detail_len = word(p + 20);
    if (row->label_len > MAX_LABEL || row->detail_len > MAX_VALUE ||
        row->label_len + row->detail_len > length - offset - ROW_SIZE) return FALSE;
    row->label = p + ROW_SIZE;
    row->detail = row->label + row->label_len;
    return TRUE;
}

static BOOL item_at(LONG index, Row *row)
{
    return index >= 0 && (size_t)index < panel.rows &&
           read_row(panel.packet, panel.length, panel.offsets[index], row);
}

static void return_to_editor(void)
{
    WinSetWindowPos(panel.owner, HWND_TOP, 0, 0, 0, 0, SWP_ZORDER | SWP_ACTIVATE);
    WinSetFocus(HWND_DESKTOP, panel.editor);
}

static void activate_row(void)
{
    Row row;
    if (!item_at((SHORT)WinQueryLboxSelectedItem(panel.items), &row) ||
        !(row.flags & (CLICK | FOCUS))) return;
    return_to_editor();
    panel.dispatch(ACTIVATE, (int)row.low, (int)row.high, 0, 0);
}

static size_t native_text(const uint8_t *text, size_t length, char *out, size_t capacity)
{
    size_t i, count;
    if (!capacity) return 0;
    count = wc_pm_encode(text, length, (unsigned char *)out, capacity - 1);
    if (count == (size_t)-1) {
        count = length < capacity ? length : capacity - 1;
        for (i = 0; i < count; ++i) out[i] = text[i] < 128 ? (char)text[i] : '?';
    }
    for (i = 0; i < count; ++i)
        if (!out[i]) out[i] = ' ';
    out[count] = '\0';
    return count;
}

static void detail(void)
{
    static char text[MAX_VALUE + MAX_LABEL + 128];
    size_t len = 0;
    LONG previous;
    Row row;
    BOOL active = FALSE;
    if (item_at((SHORT)WinQueryLboxSelectedItem(panel.items), &row)) {
        active = (row.flags & (CLICK | FOCUS)) != 0;
        len = native_text(row.label, row.label_len, text, sizeof(text));
        if (row.flags & DISABLED) { strcpy(text + len, " (unavailable)"); len += strlen(text + len); }
        if (row.flags & FOCUSED) { strcpy(text + len, " (editor focus)"); len += strlen(text + len); }
        if (row.flags & DOCUMENT) {
            strcpy(text + len, "\n\nText from the current pages. Scroll the editor to read other pages.");
            len += strlen(text + len);
        }
        if (row.detail_len) {
            memcpy(text + len, "\n\n", 2);
            len += 2;
            len += native_text(row.detail, row.detail_len, text + len, sizeof(text) - len);
        }
    } else {
        strcpy(text, "Waiting for the editor.");
        len = strlen(text);
    }
    WinEnableWindow(panel.activate, active);
    WinSendMsg(panel.read, MLM_DISABLEREFRESH, 0, 0);
    WinSendMsg(panel.read, MLM_SETREADONLY, MPFROMSHORT(FALSE), 0);
    previous = (LONG)WinSendMsg(panel.read, MLM_QUERYTEXTLENGTH, 0, 0);
    if (previous > 0) WinSendMsg(panel.read, MLM_DELETE, 0, MPFROMLONG(previous));
    WinSendMsg(panel.read, MLM_SETSEL, 0, 0);
    if (len) WinSendMsg(panel.read, MLM_INSERT, MPFROMP(text), 0);
    WinSendMsg(panel.read, MLM_SETSEL, 0, 0);
    WinSendMsg(panel.read, MLM_SETREADONLY, MPFROMSHORT(TRUE), 0);
    WinSendMsg(panel.read, MLM_ENABLEREFRESH, 0, 0);
}

static void read_document(void)
{
    size_t i;
    Row row;
    for (i = 0; i < panel.rows; ++i) {
        if (item_at((LONG)i, &row) && (row.flags & DOCUMENT)) {
            WinSendMsg(panel.items, LM_SELECTITEM, MPFROMLONG(i), MPFROMSHORT(TRUE));
            detail();
            WinSetFocus(HWND_DESKTOP, panel.read);
            return;
        }
    }
}

static void layout(HWND window)
{
    RECTL area;
    LONG w, h, half;
    if (!panel.help || !WinQueryWindowRect(window, &area)) return;
    w = area.xRight;
    h = area.yTop;
    if (w < 310 || h < 110) return;
    half = (w - 26) / 2;
    WinSetWindowPos(panel.help, 0, 10, h - 28, w - 20, 19, SWP_MOVE | SWP_SIZE);
    WinSetWindowPos(panel.items, 0, 10, 45, half, h - 78, SWP_MOVE | SWP_SIZE);
    WinSetWindowPos(panel.read, 0, 16 + half, 45, half, h - 78, SWP_MOVE | SWP_SIZE);
    WinSetWindowPos(panel.activate, 0, 10, 10, 92, 26, SWP_MOVE | SWP_SIZE);
    WinSetWindowPos(panel.back, 0, 108, 10, 112, 26, SWP_MOVE | SWP_SIZE);
    WinSetWindowPos(panel.close, 0, w - 82, 10, 72, 26, SWP_MOVE | SWP_SIZE);
}

static void close_panel(void)
{
    HWND frame = panel.frame;
    if (!frame) return;
    panel.frame = NULLHANDLE;
    wc_a11y_focus(FALSE);
    panel.dispatch(SET_ENABLED, 0, 0, 0, 0);
    return_to_editor();
    WinDestroyWindow(frame);
    free(panel.packet);
    memset(&panel, 0, sizeof(panel));
}

static MRESULT EXPENTRY child_proc(HWND window, ULONG msg, MPARAM mp1, MPARAM mp2)
{
    const HWND *order = panel.tab_order;
    USHORT flags = SHORT1FROMMP(mp1), vk = SHORT2FROMMP(mp2);
    unsigned int i, next;
    for (i = 0; i < 5; ++i)
        if (order[i] == window) break;
    if (i == 5) return WinDefWindowProc(window, msg, mp1, mp2);
    if (msg == WM_CHAR && (flags & (KC_CTRL | KC_ALT | KC_KEYUP | KC_SCANCODE)) ==
        (KC_CTRL | KC_SCANCODE) && CHAR4FROMMP(mp1) == 0x20) {
        read_document();
        return 0;
    }
    if (msg == WM_CHAR && (flags & KC_VIRTUALKEY) && !(flags & KC_KEYUP)) {
        if (vk == VK_ESC) {
            WinPostMsg(panel.client, WM_CLOSE, 0, 0);
            return 0;
        }
        if (vk == VK_F12 && (flags & (KC_SHIFT | KC_CTRL | KC_ALT)) == (KC_SHIFT | KC_CTRL)) {
            return_to_editor();
            return 0;
        }
        if (vk == VK_TAB || vk == VK_BACKTAB) {
            next = i;
            do {
                next = (next + ((vk == VK_BACKTAB || (flags & KC_SHIFT)) ? 4 : 1)) % 5;
            } while (next != i && !WinIsWindowEnabled(order[next]));
            WinSetFocus(HWND_DESKTOP, order[next]);
            return 0;
        }
        if ((vk == VK_ENTER || vk == VK_NEWLINE) && window == panel.items) {
            activate_row();
            return 0;
        }
    }
    return panel.tab_proc[i](window, msg, mp1, mp2);
}

static MRESULT EXPENTRY panel_proc(HWND window, ULONG msg, MPARAM mp1, MPARAM mp2)
{
    RECTL rect;
    HPS ps;
    switch (msg) {
    case WM_SIZE:
        layout(window);
        return 0;
    case WM_PAINT:
        ps = WinBeginPaint(window, NULLHANDLE, &rect);
        if (ps) {
            WinFillRect(ps, &rect, SYSCLR_DIALOGBACKGROUND);
            WinEndPaint(ps);
        }
        return 0;
    case WM_CONTROL:
        if (SHORT1FROMMP(mp1) == ID_ITEMS && !panel.busy) {
            if (SHORT2FROMMP(mp1) == LN_SELECT) detail();
            if (SHORT2FROMMP(mp1) == LN_ENTER) activate_row();
        }
        return 0;
    case WM_COMMAND:
        switch (SHORT1FROMMP(mp1)) {
        case ID_ACTIVATE: activate_row(); return 0;
        case ID_EDITOR: return_to_editor(); return 0;
        case ID_CLOSE: close_panel(); return 0;
        }
        break;
    case WM_CLOSE:
        close_panel();
        return 0;
    }
    return WinDefWindowProc(window, msg, mp1, mp2);
}

static HWND control(PSZ class_name, PSZ title, ULONG style, USHORT id)
{
    return WinCreateWindow(panel.client, class_name, title, WS_VISIBLE | style,
                           0, 0, 0, 0, panel.client, HWND_TOP, id, NULL, NULL);
}

static BOOL open_panel(HAB anchor, HWND owner, HWND editor, WcA11yEvent dispatch)
{
    ULONG flags = FCF_TITLEBAR | FCF_SYSMENU | FCF_SIZEBORDER | FCF_MINMAX | FCF_TASKLIST;
    LONG screen_w, screen_h, w, h;
    char title[96];
    unsigned int i;
    if (panel.frame) return TRUE;
    if (!class_registered) {
        if (!WinRegisterClass(anchor, "CraftAccessibility", panel_proc, CS_SIZEREDRAW, 0)) return FALSE;
        class_registered = TRUE;
    }
    snprintf(title, sizeof(title), "%s Accessibility Navigator", wc_pm_config()->title);
    panel.frame = WinCreateStdWindow(HWND_DESKTOP, 0, &flags, "CraftAccessibility",
                                     title, 0, 0, 0, &panel.client);
    if (!panel.frame) return FALSE;
    panel.owner = owner;
    panel.editor = editor;
    panel.dispatch = dispatch;
    panel.items = control(WC_LISTBOX, "Document and controls", LS_NOADJUSTPOS, ID_ITEMS);
    panel.read = control(WC_MLE, "Selected item text", MLS_BORDER | MLS_WORDWRAP |
                         MLS_VSCROLL | MLS_READONLY | MLS_IGNORETAB, ID_READ);
    panel.activate = control(WC_BUTTON, "Activate", BS_PUSHBUTTON, ID_ACTIVATE);
    panel.back = control(WC_BUTTON, "Editor", BS_PUSHBUTTON, ID_EDITOR);
    panel.close = control(WC_BUTTON, "Close", BS_PUSHBUTTON, ID_CLOSE);
    panel.help = control(WC_STATIC, "Arrows select an item. Tab moves between controls. Ctrl+D reads the document.",
                         SS_TEXT | DT_LEFT | DT_VCENTER, ID_HELP);
    if (!panel.items || !panel.read || !panel.activate || !panel.back || !panel.close || !panel.help) {
        WinDestroyWindow(panel.frame);
        panel.frame = NULLHANDLE;
        return FALSE;
    }
    panel.tab_order[0] = panel.items;
    panel.tab_order[1] = panel.read;
    panel.tab_order[2] = panel.activate;
    panel.tab_order[3] = panel.back;
    panel.tab_order[4] = panel.close;
    for (i = 0; i < 5; ++i) {
        panel.tab_proc[i] = WinSubclassWindow(panel.tab_order[i], child_proc);
        if (!panel.tab_proc[i]) {
            WinDestroyWindow(panel.frame);
            panel.frame = NULLHANDLE;
            return FALSE;
        }
    }
    WinSendMsg(panel.read, MLM_FORMAT, MPFROMSHORT(MLFIE_NOTRANS), 0);
    WinSendMsg(panel.read, MLM_SETTEXTLIMIT, MPFROMLONG((MAX_VALUE + MAX_LABEL + 128)), 0);
    screen_w = WinQuerySysValue(HWND_DESKTOP, SV_CXSCREEN);
    screen_h = WinQuerySysValue(HWND_DESKTOP, SV_CYSCREEN);
    w = screen_w * 3 / 4;
    h = screen_h * 2 / 3;
    if (!WinSetWindowPos(panel.frame, HWND_TOP, (screen_w - w) / 2, (screen_h - h) / 2,
                         w, h, SWP_MOVE | SWP_SIZE)) {
        WinDestroyWindow(panel.frame);
        panel.frame = NULLHANDLE;
        return FALSE;
    }
    detail();
    dispatch(SET_ENABLED, 1, 0, 0, 0);
    return TRUE;
}

BOOL wc_a11y_key(HAB anchor, HWND owner, HWND editor, MPARAM mp1, MPARAM mp2, WcA11yEvent dispatch)
{
    USHORT flags = SHORT1FROMMP(mp1);
    if (SHORT2FROMMP(mp2) != VK_F12 ||
        (flags & (KC_SHIFT | KC_CTRL | KC_ALT | KC_VIRTUALKEY)) != (KC_SHIFT | KC_CTRL | KC_VIRTUALKEY))
        return FALSE;
    if (flags & (KC_KEYUP | KC_PREVDOWN)) return TRUE;
    if (!open_panel(anchor, owner, editor, dispatch)) {
        WinMessageBox(HWND_DESKTOP, owner, "The Accessibility Navigator could not open.",
                      wc_pm_config()->title, 0, MB_OK | MB_ERROR);
        return TRUE;
    }
    WinSetWindowPos(panel.frame, HWND_TOP, 0, 0, 0, 0, SWP_SHOW | SWP_ZORDER | SWP_ACTIVATE);
    WinSetFocus(HWND_DESKTOP, panel.items);
    return TRUE;
}

BOOL wc_a11y_enabled(void)
{
    return panel.frame != NULLHANDLE;
}

void wc_a11y_focus(BOOL focused)
{
    if (!focused && panel.cursor) {
        WinDestroyCursor(panel.editor);
        panel.cursor = FALSE;
    }
}

static void native_caret(const uint8_t *packet)
{
    LONG x = (LONG)word(packet + 4), y = (LONG)word(packet + 8);
    LONG h = (LONG)word(packet + 12);
    RECTL client;
    if (WinQueryFocus(HWND_DESKTOP) != panel.editor || !WinQueryWindowRect(panel.editor, &client) ||
        x < 0 || x >= client.xRight || h < 1 || h > client.yTop ||
        y < 0 || y > client.yTop - h) {
        wc_a11y_focus(FALSE);
        return;
    }
    y = client.yTop - y - h;
    if (panel.cursor && h == panel.cursor_h) {
        if (x != panel.cursor_x || y != panel.cursor_y)
            WinCreateCursor(panel.editor, x, y, 0, h, CURSOR_SETPOS, NULL);
    } else {
        wc_a11y_focus(FALSE);
        /* Keep the native caret queryable while egui draws the visible caret. */
        panel.cursor = WinCreateCursor(panel.editor, x, y, 0, h, CURSOR_SOLID, NULL);
    }
    panel.cursor_x = x;
    panel.cursor_y = y;
    panel.cursor_h = h;
}

BOOL wc_a11y_update(const uint8_t *packet, size_t length)
{
    size_t offset, count, i;
    uint8_t *copy;
    LONG index, selected = LIT_NONE, focused = LIT_NONE;
    Row row, old;
    BOOL has_old;
    if (!panel.frame || length < HEADER_SIZE || length > MAX_PACKET ||
        memcmp(packet, "WCA1", 4)) return FALSE;
    if (panel.packet && panel.length == length &&
        !memcmp(panel.packet + HEADER_SIZE, packet + HEADER_SIZE, length - HEADER_SIZE)) {
        native_caret(packet);
        return TRUE;
    }
    for (offset = HEADER_SIZE, count = 0; offset < length; ++count) {
        if (count == MAX_ROWS || !read_row(packet, length, offset, &row)) return FALSE;
        offset += ROW_SIZE + row.label_len + row.detail_len;
    }
    native_caret(packet);
    copy = malloc(length);
    if (!copy) return FALSE;
    memcpy(copy, packet, length);
    has_old = item_at((SHORT)WinQueryLboxSelectedItem(panel.items), &old);
    panel.busy = TRUE;
    WinEnableWindowUpdate(panel.items, FALSE);
    WinSendMsg(panel.items, LM_DELETEALL, 0, 0);
    for (offset = HEADER_SIZE, i = 0; i < count; ++i) {
        char label[MAX_LABEL * 2 + 128];
        size_t spaces;
        if (!read_row(copy, length, offset, &row)) break;
        spaces = row.depth > 12 ? 24 : row.depth * 2;
        memset(label, ' ', spaces);
        spaces += native_text(row.label, row.label_len, label + spaces, sizeof(label) - spaces);
        if (row.flags & DISABLED) strcpy(label + spaces, " (unavailable)");
        index = (SHORT)WinInsertLboxItem(panel.items, LIT_END, label);
        if (index < 0) break;
        panel.offsets[i] = offset;
        if (has_old && row.low == old.low && row.high == old.high) selected = index;
        if (row.flags & FOCUSED) focused = index;
        offset += ROW_SIZE + row.label_len + row.detail_len;
    }
    free(panel.packet);
    panel.packet = copy;
    panel.rows = i;
    panel.length = length;
    if (selected == LIT_NONE) selected = focused == LIT_NONE ? 0 : focused;
    if (selected >= 0 && (size_t)selected < panel.rows)
        WinSendMsg(panel.items, LM_SELECTITEM, MPFROMLONG(selected), MPFROMSHORT(TRUE));
    WinEnableWindowUpdate(panel.items, TRUE);
    panel.busy = FALSE;
    detail();
    return TRUE;
}

void wc_a11y_shutdown(void)
{
    wc_a11y_focus(FALSE);
    if (panel.frame) WinDestroyWindow(panel.frame);
    free(panel.packet);
    memset(&panel, 0, sizeof(panel));
}
