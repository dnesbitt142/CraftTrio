/* App-specific launcher for the shared Presentation Manager host. */
#define INCL_DOS
#define INCL_WIN
#include <os2.h>
#include <stdio.h>
#include <string.h>
#include "craftpm.h"

const WcPmApi *wc_pm;

static WcPmConfig config = {
#if defined(WC_DECKCRAFT)
    "DeckCraft", {"Open a presentation or outline", "Open a picture", "Open an audio file", "Open a video file"},
    "*.*", ".pptx", "DCGUI.LOG", "DCENGINE.LOG",
    "https://api.github.com/repos/storytold/deckcraft/releases/latest", 0, 1
#elif defined(WC_GRIDCRAFT)
    "GridCraft", {"Open a workbook", "Open a picture", "Open a font", "Open a file"},
    "*.*", ".xlsx", "GCGUI.LOG", "GCENGINE.LOG",
    "https://api.github.com/repos/storytold/gridcraft/releases/latest", 0, 1
#else
    "WordCraft", {"Open a document", "Open a picture", "Open a font", "Open a file"},
    "*.*", ".docx", "WCGUI.LOG", "WCENGINE.LOG",
    "https://api.github.com/repos/storytold/wordcraft/releases/latest", 0, 0
#endif
};

static const WcPmEngine engine = {
    wc_reactor_start, wc_reactor_shutdown, wc_reactor_call, wc_reactor_memory
};

static int startup_error(const char *message)
{
    HAB anchor = WinInitialize(0);
    if (anchor) {
        HMQ queue = WinCreateMsgQueue(anchor, 0);
        if (queue) {
            WinMessageBox(HWND_DESKTOP, HWND_DESKTOP, (PSZ)message, (PSZ)config.title, 0,
                          MB_OK | MB_ERROR | MB_MOVEABLE);
            WinDestroyMsgQueue(queue);
        } else {
            fprintf(stderr, "%s\n", message);
        }
        WinTerminate(anchor);
    } else {
        fprintf(stderr, "%s\n", message);
    }
    return 1;
}

int main(void)
{
    PTIB tib;
    PPIB pib;
    if (DosGetInfoBlocks(&tib, &pib) != 0)
        return startup_error("Could not locate the application module.");
    config.icon_module = pib->pib_hmte;
#ifdef WC_STATIC_HOST
    wc_pm = wc_pm_get_api(WC_PM_ABI);
    if (!wc_pm) return startup_error("The Presentation Manager interface version is unavailable.");
    return wc_pm->run(&config, &engine);
#else
    {
        static const char dll[] = "CRAFTPM.DLL";
        char path[CCHMAXPATH], loader_error[CCHMAXPATH], message[320], *directory;
        HMODULE module;
        PFN symbol;
        APIRET status;
        int result;
        if (DosQueryModuleName(pib->pib_hmte, sizeof(path), (PSZ)path) != 0)
            return startup_error("Could not locate the application directory.");
        directory = strrchr(path, '\\');
        if (!directory || (size_t)(directory - path) + sizeof(dll) >= sizeof(path))
            return startup_error("The application path is too long.");
        strcpy(directory + 1, dll);
        status = DosLoadModule((PSZ)loader_error, sizeof(loader_error), (PSZ)path, &module);
        if (status) {
            snprintf(message, sizeof(message), "Could not load CRAFTPM.DLL beside the application (OS/2 error %lu).",
                     (unsigned long)status);
            return startup_error(message);
        }
        if (DosQueryProcAddr(module, 0, "wc_pm_get_api", &symbol) == 0)
            wc_pm = ((WcPmGetApi)symbol)(WC_PM_ABI);
        if (!wc_pm) {
            DosFreeModule(module);
            return startup_error("CRAFTPM.DLL does not provide the required interface version.");
        }
        result = wc_pm->run(&config, &engine);
        wc_pm = NULL;
        DosFreeModule(module);
        return result;
    }
#endif
}
