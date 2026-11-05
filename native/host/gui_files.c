/* Presentation Manager file dialogs and byte I/O for the Craft Apps. */
#define INCL_WIN
#define INCL_DOS
#include <os2.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "gui_files.h"
#include "craftpm.h"

#define WC_GUI_INPUT_LIMIT (16UL * 1024UL * 1024UL)
#define WC_GUI_OUTPUT_LIMIT (48UL * 1024UL * 1024UL)

extern HWND wc_gui_frame(void);
static char last_saved[WC_GUI_PATH_CAP];
static char last_opened[WC_GUI_PATH_CAP];
static unsigned long sequence;

static void file_error(const char *message)
{
    WinMessageBox(HWND_DESKTOP, wc_gui_frame(), message, (PSZ)wc_pm_config()->title, 0,
                  MB_OK | MB_ERROR | MB_MOVEABLE);
}

static int ascii_path(const char *path)
{
    const unsigned char *p = (const unsigned char *)path;
    for (; *p; ++p) if (*p < 32 || *p >= 127) return 0;
    return path[0] != '\0';
}

static int choose_path(char *path, int save, const char *title, const char *suggestion)
{
    FILEDLG dlg;
    memset(&dlg, 0, sizeof(dlg));
    dlg.cbSize = sizeof(dlg);
    dlg.fl = FDS_CENTER | (save ? FDS_SAVEAS_DIALOG : FDS_OPEN_DIALOG);
    dlg.pszTitle = (PSZ)title;
    if (strlen(suggestion) >= sizeof(dlg.szFullFile)) return -1;
    strcpy(dlg.szFullFile, suggestion);
    if (!WinFileDlg(HWND_DESKTOP, wc_gui_frame(), &dlg) || dlg.lReturn != DID_OK)
        return 0;
    if (!ascii_path(dlg.szFullFile)) {
        file_error("This build requires ASCII filenames. Document text can contain Unicode.");
        return -1;
    }
    strcpy(path, dlg.szFullFile);
    return 1;
}

int wc_gui_pick_input(uint32_t kind, char path[WC_GUI_PATH_CAP],
                      WcInput **input, uint32_t *size)
{
    const char *title, *error;
    FILESTATUS3 info;
    int picked;
    *input = NULL;
    *size = 0;
    if (kind < 1 || kind > 4) return -1;
    title = wc_pm_config()->open_titles[kind - 1];
    picked = choose_path(path, 0, title, kind == 1 ? wc_pm_config()->document_filter : "*.*");
    if (picked <= 0) return picked;
    if (DosQueryPathInfo(path, FIL_STANDARD, &info, sizeof(info)) != 0 ||
        (info.attrFile & FILE_DIRECTORY) || info.cbFile > WC_GUI_INPUT_LIMIT) {
        file_error("Choose a readable file no larger than 16 MiB.");
        return -1;
    }
    error = wc_input_open(path, (uint32_t)info.cbFile, input);
    if (error) { file_error(error); return -1; }
    *size = (uint32_t)info.cbFile;
    return 1;
}

void wc_gui_input_accepted(uint32_t kind, const char *path)
{
    if (kind == 1 && path && strlen(path) < sizeof(last_opened))
        strcpy(last_opened, path);
}

static const char *basename_of(const char *path)
{
    const char *name = path;
    const char *p;
    for (p = path; *p; ++p)
        if (*p == '\\' || *p == '/' || *p == ':') name = p + 1;
    return name;
}

static int equivalent_name(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

static int preserve_extension(char *path, const char *requested)
{
    const char *ext = strrchr(basename_of(requested), '.');
    const char *chosen = strrchr(basename_of(path), '.');
    if (!ext) return 1;
    if (chosen) return equivalent_name(chosen, ext);
    if (strlen(path) + strlen(ext) >= WC_GUI_PATH_CAP) return 0;
    strcat(path, ext);
    return 1;
}

static int sibling(char *result, const char *path, const char *extension)
{
    size_t prefix = (size_t)(basename_of(path) - path);
    unsigned long token = ((unsigned long)clock() ^ (++sequence * 1103515245UL)) & 0xffffffUL;
    if (prefix + 13 > WC_GUI_PATH_CAP) return 0;
    memcpy(result, path, prefix);
    sprintf(result + prefix, "WC%06lX.%s", token, extension);
    return 1;
}

static int write_file(const char *path, const void *data, uint32_t size, int replace)
{
    char temporary[WC_GUI_PATH_CAP], backup[WC_GUI_PATH_CAP];
    char message[WC_GUI_PATH_CAP + 80];
    FILE *stream;
    int fd = -1, ok, moved = 0;
    unsigned int attempt;
    for (attempt = 0; attempt < 128; ++attempt) {
        if (!sibling(temporary, path, "TMP")) return 0;
        fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_BINARY, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) return 0;
    stream = fdopen(fd, "wb");
    if (!stream) {
        close(fd);
        remove(temporary);
        return 0;
    }
    ok = (!size || fwrite(data, 1, size, stream) == size) && !ferror(stream);
    if (fclose(stream) != 0) ok = 0;
    if (!ok) { remove(temporary); return 0; }
    if (replace) {
        for (attempt = 0; attempt < 128; ++attempt) {
            if (!sibling(backup, path, "BAK")) break;
            if (rename(path, backup) == 0) { moved = 1; break; }
            if (errno != EEXIST) break;
        }
        if (!moved) { remove(temporary); return 0; }
    }
    if (rename(temporary, path) != 0) {
        if (moved && rename(backup, path) != 0) {
            sprintf(message, "The original file is available at %s", backup);
            file_error(message);
        }
        remove(temporary);
        return 0;
    }
    if (moved) remove(backup);
    return 1;
}

int32_t wc_gui_save(const char *name, uint32_t name_size, const void *data,
                    uint32_t data_size, uint32_t save_as, char *output,
                    uint32_t output_size)
{
    char path[WC_GUI_PATH_CAP], requested[WC_GUI_PATH_CAP];
    FILESTATUS3 info;
    int picked, reuse, replace;
    const char *known_path = NULL;
    char title[96];
    size_t length;
    if (!name_size || name_size >= sizeof(requested) || data_size > WC_GUI_OUTPUT_LIMIT)
        return -2;
    if (memchr(name, 0, name_size)) return -2;
    memcpy(requested, name, name_size);
    requested[name_size] = 0;
    if (!ascii_path(requested)) {
        if (strlen(basename_of(requested)) >= sizeof(requested) - 10) return -2;
        /* Keep the format even when the suggested filename needs renaming. */
        {
            const char *extension = strrchr(basename_of(requested), '.');
            char fallback[WC_GUI_PATH_CAP];
            strcpy(fallback, "Document");
            strcat(fallback, extension && ascii_path(extension) ? extension : wc_pm_config()->document_extension);
            strcpy(requested, fallback);
        }
    }
    if (!save_as) {
        if (last_saved[0] && equivalent_name(last_saved, requested)) known_path = last_saved;
        else if (last_opened[0] && equivalent_name(last_opened, requested)) known_path = last_opened;
    }
    reuse = known_path != NULL;
    if (reuse) strcpy(path, known_path);
    else {
        snprintf(title, sizeof(title), "Save %s file", wc_pm_config()->title);
        picked = choose_path(path, 1, title, basename_of(requested));
        if (picked <= 0) return picked == 0 ? -1 : -2;
        if (!preserve_extension(path, requested)) {
            file_error("Use the filename extension for the selected format.");
            return -2;
        }
    }
    length = strlen(path);
    if (length > output_size) return -2;
    replace = DosQueryPathInfo(path, FIL_STANDARD, &info, sizeof(info)) == 0;
    if (replace && (info.attrFile & (FILE_DIRECTORY | FILE_READONLY))) {
        file_error("The destination cannot be replaced.");
        return -2;
    }
    if (replace && !reuse && WinMessageBox(HWND_DESKTOP, wc_gui_frame(),
        "Replace the existing file?", (PSZ)wc_pm_config()->title, 0,
        MB_YESNO | MB_DEFBUTTON2 | MB_QUERY | MB_MOVEABLE) != MBID_YES)
        return -1;
    if (!write_file(path, data, data_size, replace)) {
        file_error("Could not save the document.");
        return -2;
    }
    strcpy(last_saved, path);
    if (length == name_size && memcmp(path, name, length) == 0) return 0;
    memcpy(output, path, length);
    return (int32_t)length;
}
