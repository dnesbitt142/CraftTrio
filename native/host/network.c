#define INCL_DOS
#define INCL_DOSERRORS
#define INCL_WIN
#include <os2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <tcpustd.h>
#include <nerrno.h>
#include "network.h"
#include "craftpm.h"

extern HWND wc_gui_frame(void);

enum { CONNECT, READ, WRITE, CLOSE, WAKE };
enum { NET_ERROR = -1, NET_WAIT = -2, NET_UNAVAILABLE = -3 };
enum { WAIT_NONE, WAIT_CONNECT, WAIT_READ, WAIT_WRITE };
enum { ZOTERO_PORT = 23116, CONNECT_TIMEOUT = 3000, IDLE_TIMEOUT = 60 * 60 * 1000 };
enum { MAX_TRANSFER = 32 * 1024 * 1024 };

static struct {
    HMODULE module;
    int (_System *init)(void);
    int (_System *error)(void);
    int (_System *create)(int, int, int);
    int (_System *connect)(int, const struct sockaddr *, int);
    int (_System *select)(int, fd_set *, fd_set *, fd_set *, struct timeval *);
    int (_System *ioctl)(int, unsigned long, char *, int);
    int (_System *option)(int, int, int, void *, int *);
    ssize_t (_System *read)(int, void *, size_t, int);
    ssize_t (_System *write)(int, const void *, size_t, int);
    int (_System *close)(int);
} tcp;

static int zotero = -1;
static int waiting;
static int wake_pending;
static ULONG wait_start;

enum { RELEASE_START, RELEASE_STATUS, RELEASE_READ };
enum { RELEASE_LIMIT = 2 * 1024 * 1024 };
static PID release_pid;
static int32_t release_size;
static ULONG release_started;
static char release_file[CCHMAXPATH];

static ULONG ticks(void)
{
    ULONG result = 0;
    DosQuerySysInfo(QSV_MS_COUNT, QSV_MS_COUNT, &result, sizeof(result));
    return result;
}

static void wait_for(int state)
{
    if (state != waiting) wait_start = ticks();
    waiting = state;
}

/* IBM's TCPIP32 exports are ordinal-only. Load on demand so offline installs
   without the optional TCP/IP components still launch the editor. */
static int load_tcp(void)
{
    char failed[CCHMAXPATH];
    if (tcp.module) return 1;
    if (DosLoadModule(failed, sizeof(failed), "TCPIP32", &tcp.module)) return 0;
#define LOAD(field, ordinal) \
    if (DosQueryProcAddr(tcp.module, ordinal, NULL, (PFN *)&tcp.field)) goto failed
    LOAD(connect, 3);
    LOAD(option, 7);
    LOAD(read, 10);
    LOAD(write, 13);
    LOAD(create, 16);
    LOAD(close, 17);
    LOAD(error, 20);
    LOAD(init, 26);
    LOAD(select, 32);
    LOAD(ioctl, 200);
#undef LOAD
    if (tcp.init() == 0) return 1;
failed:
    DosFreeModule(tcp.module);
    memset(&tcp, 0, sizeof(tcp));
    return 0;
}

static void close_zotero(void)
{
    if (zotero >= 0) tcp.close(zotero);
    zotero = -1;
    wake_pending = 0;
    wait_for(WAIT_NONE);
}

static int connect_zotero(void)
{
    struct sockaddr_in endpoint;
    const unsigned char ip[] = {127, 0, 0, 1};
    const unsigned char port[] = {ZOTERO_PORT >> 8, ZOTERO_PORT & 255};
    int nonblocking = 1, error;
    if (zotero >= 0) return NET_ERROR;
    if (!load_tcp()) return NET_UNAVAILABLE;
    zotero = tcp.create(AF_INET, SOCK_STREAM, 0);
    if (zotero < 0) return NET_ERROR;
    if (tcp.ioctl(zotero, FIONBIO, (char *)&nonblocking, sizeof(nonblocking)) < 0)
        goto failed;
    memset(&endpoint, 0, sizeof(endpoint));
    endpoint.sin_len = sizeof(endpoint);
    endpoint.sin_family = AF_INET;
    memcpy(&endpoint.sin_addr, ip, sizeof(ip));
    memcpy(&endpoint.sin_port, port, sizeof(port));
    if (tcp.connect(zotero, (const struct sockaddr *)&endpoint, sizeof(endpoint)) == 0) {
        wait_for(WAIT_WRITE);
        return 0;
    }
    error = tcp.error();
    if (error == SOCEINPROGRESS || error == SOCEWOULDBLOCK) {
        wait_for(WAIT_CONNECT);
        return 0;
    }
failed:
    close_zotero();
    return NET_ERROR;
}

static int socket_ready(void)
{
    struct timeval instant = {0, 0};
    fd_set selected, errors;
    int readable = waiting == WAIT_READ;
    FD_ZERO(&selected);
    FD_ZERO(&errors);
    FD_SET(zotero, &selected);
    FD_SET(zotero, &errors);
    return tcp.select(zotero + 1, readable ? &selected : NULL,
                      readable ? NULL : &selected, &errors, &instant);
}

static int timed_out(void)
{
    return ticks() - wait_start >= (waiting == WAIT_CONNECT ? CONNECT_TIMEOUT : IDLE_TIMEOUT);
}

static void release_clear(void)
{
    if (*release_file) DosDelete(release_file);
    *release_file = 0;
    release_size = 0;
}

static int32_t release_start(void)
{
    static const char arguments[] =
        "--disable --proto =https --tlsv1.2 --connect-timeout 5 --max-time 20 "
        "--fail --silent --max-filesize 2097152 --user-agent CraftApps-OS2 --output ";
    const char *url = wc_pm_config()->release_url;
    char curl[CCHMAXPATH], bad[CCHMAXPATH], name[13];
    char command[CCHMAXPATH + sizeof(arguments) + sizeof(release_file) + 164];
    const char *configured = getenv("CRAFT_CURL");
    PTIB tib;
    PPIB pib;
    HFILE file;
    ULONG action;
    RESULTCODES child;
    size_t curl_length;
    APIRET error;
    if (release_pid) return NET_WAIT;
    if (!url || strlen(url) >= 160) return NET_UNAVAILABLE;
    if (!configured || !*configured) configured = getenv("WORDCRAFT_CURL");
    release_clear();
    if (configured && *configured && strlen(configured) < sizeof(curl))
        strcpy(curl, configured);
    else if (DosSearchPath(SEARCH_ENVIRONMENT, "PATH", "CURL.EXE",
                           (PBYTE)curl, sizeof(curl)) != 0)
        return NET_UNAVAILABLE;
    if (DosGetInfoBlocks(&tib, &pib) != 0) return NET_ERROR;
    sprintf(name, "WCR%05lX.TMP", (ticks() ^ (unsigned long)pib->pib_ulpid) & 0xfffffUL);
    error = DosOpen(name, &file, &action, 0, FILE_NORMAL,
                    OPEN_ACTION_CREATE_IF_NEW | OPEN_ACTION_FAIL_IF_EXISTS,
                    OPEN_ACCESS_WRITEONLY | OPEN_SHARE_DENYREADWRITE, NULL);
    if (error) return NET_ERROR;
    DosClose(file);
    if (DosQueryPathInfo(name, FIL_QUERYFULLNAME, release_file, sizeof(release_file)) != 0) {
        DosDelete(name);
        *release_file = 0;
        return NET_ERROR;
    }
    curl_length = strlen(curl);
    memcpy(command, curl, curl_length + 1);
    sprintf(command + curl_length + 1, "%s\"%s\" %s", arguments, release_file, url);
    command[curl_length + 2 + strlen(command + curl_length + 1)] = 0;
    error = DosExecPgm(bad, sizeof(bad), EXEC_ASYNCRESULT, command, NULL, &child, curl);
    if (error) {
        release_clear();
        return NET_UNAVAILABLE;
    }
    release_pid = child.codeTerminate;
    release_started = ticks();
    return NET_WAIT;
}

static int release_ready(void)
{
    PID pid;
    RESULTCODES result;
    APIRET error;
    FILESTATUS3 info;
    if (release_pid) {
        error = DosWaitChild(DCWA_PROCESS, DCWW_NOWAIT, &result, &pid, release_pid);
        if (error == ERROR_CHILD_NOT_COMPLETE) {
            if (ticks() - release_started < 25000) return 0;
            DosKillProcess(DKP_PROCESS, release_pid);
            DosWaitChild(DCWA_PROCESS, DCWW_WAIT, &result, &pid, release_pid);
        }
        release_pid = 0;
        if (error || result.codeTerminate != 0 || result.codeResult != 0 ||
            DosQueryPathInfo(release_file, FIL_STANDARD, &info, sizeof(info)) != 0 ||
            !info.cbFile || info.cbFile > RELEASE_LIMIT) {
            release_clear();
            release_size = NET_ERROR;
        } else {
            release_size = info.cbFile;
        }
    }
    return release_size != 0;
}

int32_t wc_net_release(uint32_t operation, uint32_t address, uint32_t length)
{
    size_t extent = 0;
    uint8_t *memory;
    int32_t size;
    FILE *file;
    if (operation == RELEASE_START) return release_start();
    if (operation == RELEASE_STATUS) {
        release_ready();
        size = release_size;
        if (size < 0) release_size = 0;
        return size ? size : NET_WAIT;
    }
    if (operation != RELEASE_READ || release_size <= 0 || length != (uint32_t)release_size)
        return NET_ERROR;
    memory = wc_pm_memory(&extent);
    if (!memory || address > extent || length > extent - address) return NET_ERROR;
    file = fopen(release_file, "rb");
    size = NET_ERROR;
    if (file) {
        if (fread(memory + address, 1, length, file) == length && fgetc(file) == EOF && !ferror(file))
            size = length;
        fclose(file);
    }
    release_clear();
    return size;
}

int wc_network_ready(void)
{
    if (wake_pending) {
        wake_pending = 0;
        return 1;
    }
    return release_ready() ||
           (zotero >= 0 && waiting != WAIT_NONE && (timed_out() || socket_ready() != 0));
}

int32_t wc_net_socket(uint32_t operation, uint32_t address, uint32_t length)
{
    size_t extent = 0;
    uint8_t *memory;
    int count, error, size = sizeof(error);
    if (operation == CONNECT) return connect_zotero();
    if (operation == CLOSE) { close_zotero(); return 0; }
    if (operation == WAKE) { wake_pending = 1; return 0; }
    if (zotero < 0 || (operation != READ && operation != WRITE) ||
        !length || length > MAX_TRANSFER) return NET_ERROR;
    memory = wc_pm_memory(&extent);
    if (!memory || address > extent || length > extent - address) return NET_ERROR;
    if (waiting == WAIT_CONNECT) {
        count = socket_ready();
        if (count == 0) return timed_out() ? NET_ERROR : NET_WAIT;
        if (count < 0 ||
            tcp.option(zotero, SOL_SOCKET, SO_ERROR, &error, &size) < 0 ||
            error) return NET_ERROR;
    }
    count = operation == READ ? tcp.read(zotero, memory + address, length, 0)
                              : tcp.write(zotero, memory + address, length, 0);
    if (count >= 0) {
        wait_for(WAIT_NONE);
        return count;
    }
    error = tcp.error();
    if (error != SOCEWOULDBLOCK && error != SOCEINTR) return NET_ERROR;
    wait_for(operation == READ ? WAIT_READ : WAIT_WRITE);
    return timed_out() ? NET_ERROR : NET_WAIT;
}

void wc_network_shutdown(void)
{
    if (release_pid) {
        RESULTCODES result;
        PID pid;
        DosKillProcess(DKP_PROCESS, release_pid);
        DosWaitChild(DCWA_PROCESS, DCWW_WAIT, &result, &pid, release_pid);
        release_pid = 0;
    }
    release_clear();
    close_zotero();
    if (tcp.module) DosFreeModule(tcp.module);
    memset(&tcp, 0, sizeof(tcp));
}

static void url_error(const char *message)
{
    WinMessageBox(HWND_DESKTOP, wc_gui_frame(), message, wc_pm_config()->title, 0,
                  MB_OK | MB_WARNING | MB_MOVEABLE);
}

void wc_net_open_url(uint32_t address, uint32_t length)
{
    static const char profile[] = "WPURLDEFAULTSETTINGS";
    char browser[CCHMAXPATH], directory[CCHMAXPATH], parameters[1024];
    char argument[4096], command[sizeof(parameters) + sizeof(argument) + 1];
    char *out = argument;
    const char *configured = getenv("CRAFT_BROWSER");
    size_t extent = 0;
    uint8_t *memory = wc_pm_memory(&extent);
    PROGDETAILS details;
    uint32_t i;
    if (!memory || !length || length > 2048 || address > extent || length > extent - address)
        return;
    memory += address;
    if (!(length >= 8 && memicmp(memory, "https://", 8) == 0) &&
        !(length >= 7 && memicmp(memory, "http://", 7) == 0) &&
        !(length >= 7 && memicmp(memory, "mailto:", 7) == 0)) {
        url_error("Only HTTP, HTTPS and mail links can be opened.");
        return;
    }
    /* Quote once for the browser's argv. WinStartApp runs the executable
       directly. Escape bytes that could end the argument or be lost in the
       guest's code page, while preserving existing URL escapes. */
    *out++ = '"';
    for (i = 0; i < length; ++i) {
        unsigned char c = memory[i];
        if (c <= 0x20 || c >= 0x7f || c == '"' || c == '\\') {
            static const char hex[] = "0123456789ABCDEF";
            if (out > argument + sizeof(argument) - 5) {
                url_error("The web link is too long to open.");
                return;
            }
            *out++ = '%';
            *out++ = hex[c >> 4];
            *out++ = hex[c & 15];
        } else {
            if (out > argument + sizeof(argument) - 3) {
                url_error("The web link is too long to open.");
                return;
            }
            *out++ = c;
        }
    }
    *out++ = '"';
    *out = 0;
    if (!configured || !*configured) configured = getenv("WORDCRAFT_BROWSER");
    if (configured && *configured && strlen(configured) < sizeof(browser)) {
        strcpy(browser, configured);
        directory[0] = parameters[0] = 0;
    } else {
        PrfQueryProfileString(HINI_USER, profile, "DefaultBrowserExe", "EXPLORE.EXE",
                              browser, sizeof(browser));
        PrfQueryProfileString(HINI_USER, profile, "DefaultWorkingDir", "",
                              directory, sizeof(directory));
        if (!PrfQueryProfileString(HINI_USER, profile, "DefaultBrowserParameters", "",
                                   parameters, sizeof(parameters)) || !*parameters)
            PrfQueryProfileString(HINI_USER, profile, "DefaultParameters", "",
                                  parameters, sizeof(parameters));
    }
    memset(&details, 0, sizeof(details));
    details.Length = sizeof(details);
    details.progt.progc = PROG_DEFAULT;
    details.progt.fbVisible = SHE_VISIBLE;
    details.pszTitle = (PSZ)wc_pm_config()->title;
    details.pszExecutable = browser;
    strcpy(command, parameters);
    if (*parameters) strcat(command, " ");
    strcat(command, argument);
    details.pszParameters = command;
    details.pszStartupDir = *directory ? directory : NULL;
    if (!WinStartApp(NULLHANDLE, &details, NULL, NULL, SAF_INSTALLEDCMDLINE))
        url_error("Set an OS/2 default browser or set CRAFT_BROWSER to its executable.");
}
