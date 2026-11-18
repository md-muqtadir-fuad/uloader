/*
 * U-loader - browser integration.
 *
 * The browser extension talks to "uloader.exe" through Chrome/Edge native
 * messaging: the browser starts us with "chrome-extension://<id>/" as the
 * first argument and exchanges length-prefixed JSON on stdin/stdout.
 * A download request becomes a job file that is handed to the running
 * U-loader window (WM_COPYDATA), or to a newly started one.
 *
 * Only the U-loader extension (fixed ID below) may talk to this host: the
 * browser enforces "allowed_origins" from the manifest we register.
 */
#include "uloader.h"
#include <ole2.h>
#include <exdisp.h>
#include <shldisp.h>
#include <shlobj.h>
#include <servprov.h>

#define UL_EXT_ID "ednpijjpophhlcbidanppgjliblinjga"
#define MAX_MSG   (4u << 20)

static const wchar_t *BROWSER_KEYS[] = {
    L"Software\\Google\\Chrome\\NativeMessagingHosts\\" L"com.uloader.host",
    L"Software\\Microsoft\\Edge\\NativeMessagingHosts\\" L"com.uloader.host",
    L"Software\\Chromium\\NativeMessagingHosts\\" L"com.uloader.host",
    L"Software\\BraveSoftware\\Brave-Browser\\NativeMessagingHosts\\" L"com.uloader.host",
};

/* ------------------------------------------------------------------ */
static int read_exact(HANDLE h, void *buf, DWORD n)
{
    char *p = buf;
    while (n) {
        DWORD got = 0;
        if (!ReadFile(h, p, n, &got, NULL) || got == 0) return 0;
        p += got;
        n -= got;
    }
    return 1;
}

static int write_msg(HANDLE h, const char *json)
{
    uint32_t len = (uint32_t)strlen(json);
    DWORD w;
    return WriteFile(h, &len, 4, &w, NULL) && WriteFile(h, json, len, &w, NULL);
}

static char *exe_path(void)
{
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(NULL, buf, MAX_PATH * 4);
    if (!n || n >= MAX_PATH * 4) return NULL;
    return w_to_u8(buf);
}

/* Browser cookies -> Netscape cookies.txt in %TEMP% (deleted after the job). */
static char *write_cookie_file(const char *msg)
{
    const char *arr = json_find(msg, "cookies");
    if (!arr || *arr != '[') return NULL;
    wchar_t tdir[MAX_PATH + 1], tfile[MAX_PATH + 1];
    if (!GetTempPathW(MAX_PATH + 1, tdir) || !GetTempFileNameW(tdir, L"ulc", 0, tfile)) return NULL;
    char *path = w_to_u8(tfile);
    FILE *f = fopen_u8(path, "wb");
    if (!f) { free(path); return NULL; }
    fputs("# Netscape HTTP Cookie File\n", f);
    int count = 0;
    const char *p = arr + 1;
    for (;;) {
        p = json_skip_ws(p);
        if (*p != '{') break;
        const char *obj = p;
        char *domain = json_str(json_find(obj, "domain"));
        char *cpath = json_str(json_find(obj, "path"));
        char *name = json_str(json_find(obj, "name"));
        char *value = json_str(json_find(obj, "value"));
        char *host_only = json_str(json_find(obj, "hostOnly"));
        char *secure = json_str(json_find(obj, "secure"));
        char *exp = json_str(json_find(obj, "expirationDate"));
        int bad = !domain || !*domain || !name || !value ||
                  strpbrk(domain, "\t\r\n ") || strpbrk(name, "\t\r\n") || strpbrk(value, "\t\r\n") ||
                  (cpath && strpbrk(cpath, "\t\r\n"));
        if (!bad && count < 2000) {
            int ho = host_only && !strcmp(host_only, "true");
            const char *d = domain;
            if (ho && *d == '.') d++;
            fprintf(f, "%s%s\t%s\t%s\t%s\t%lld\t%s\t%s\n", (!ho && *d != '.') ? "." : "", d,
                    ho ? "FALSE" : "TRUE", cpath && *cpath ? cpath : "/",
                    secure && !strcmp(secure, "true") ? "TRUE" : "FALSE",
                    exp ? (long long)strtod(exp, NULL) : 0LL, name, value);
            count++;
        }
        free(domain); free(cpath); free(name); free(value); free(host_only); free(secure); free(exp);
        p = json_skip(obj);
        if (!p) break;
        p = json_skip_ws(p);
        if (*p != ',') break;
        p++;
    }
    fclose(f);
    if (!count) { file_delete(path); free(path); return NULL; }
    return path;
}

/* GUIDs not every MinGW import library carries */
static const GUID UL_CLSID_ShellWindows = { 0x9BA05972, 0xF6A8, 0x11CF, { 0xA4, 0x42, 0x00, 0xA0, 0xC9, 0x0A, 0x8F, 0x39 } };
static const GUID UL_IID_IShellWindows  = { 0x85CB6900, 0x4D95, 0x11CF, { 0x96, 0x0C, 0x00, 0x80, 0xC7, 0xF4, 0xEE, 0x85 } };
static const GUID UL_SID_STopLevelBrowser = { 0x4C96BE40, 0x915C, 0x11CF, { 0x99, 0xD3, 0x00, 0xAA, 0x00, 0x4A, 0xE8, 0x37 } };
static const GUID UL_IID_IShellBrowser  = { 0x000214E2, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const GUID UL_IID_IServiceProvider = { 0x6D5140C1, 0x7436, 0x11CE, { 0x80, 0x34, 0x00, 0xAA, 0x00, 0x60, 0x09, 0xFA } };
static const GUID UL_IID_IShellFolderViewDual = { 0xE7A1AF80, 0x4D96, 0x11CF, { 0x96, 0x0C, 0x00, 0x80, 0xC7, 0xF4, 0xEE, 0x85 } };
static const GUID UL_IID_IShellDispatch2 = { 0xA4C6892C, 0x3BA9, 0x11D2, { 0x9D, 0xEA, 0x00, 0xC0, 0x4F, 0xB1, 0x61, 0x62 } };
static const GUID UL_IID_IDispatch = { 0x00020400, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

/* Start a program through the desktop's Explorer (IShellDispatch2::ShellExecute).
   The new process is a child of explorer.exe, so it is outside the browser's
   job object and survives when the browser ends this native host. */
static int shell_execute_via_explorer(const char *exe, const char *args)
{
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    int need_uninit = SUCCEEDED(hr);
    int ok = 0;
    IShellWindows *sw = NULL;
    IDispatch *disp = NULL, *bg = NULL, *app = NULL;
    IServiceProvider *sp = NULL;
    IShellBrowser *sb = NULL;
    IShellView *sv = NULL;
    IShellFolderViewDual *fvd = NULL;
    IShellDispatch2 *sd = NULL;
    VARIANT loc, empty;
    long hwnd = 0;
    VariantInit(&loc);
    VariantInit(&empty);
    loc.vt = VT_I4;
    loc.lVal = CSIDL_DESKTOP;

    if (FAILED(CoCreateInstance(&UL_CLSID_ShellWindows, NULL, CLSCTX_LOCAL_SERVER, &UL_IID_IShellWindows, (void **)&sw))) goto out;
    if (sw->lpVtbl->FindWindowSW(sw, &loc, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &disp) != S_OK || !disp) goto out;
    if (FAILED(disp->lpVtbl->QueryInterface(disp, &UL_IID_IServiceProvider, (void **)&sp))) goto out;
    if (FAILED(sp->lpVtbl->QueryService(sp, &UL_SID_STopLevelBrowser, &UL_IID_IShellBrowser, (void **)&sb))) goto out;
    if (FAILED(sb->lpVtbl->QueryActiveShellView(sb, &sv))) goto out;
    if (FAILED(sv->lpVtbl->GetItemObject(sv, SVGIO_BACKGROUND, &UL_IID_IDispatch, (void **)&bg))) goto out;
    if (FAILED(bg->lpVtbl->QueryInterface(bg, &UL_IID_IShellFolderViewDual, (void **)&fvd))) goto out;
    if (FAILED(fvd->lpVtbl->get_Application(fvd, &app))) goto out;
    if (FAILED(app->lpVtbl->QueryInterface(app, &UL_IID_IShellDispatch2, (void **)&sd))) goto out;
    {
        wchar_t *wexe = u8_to_w(exe), *wargs = u8_to_w(args);
        BSTR file = SysAllocString(wexe);
        VARIANT vargs, vdir, vop, vshow;
        VariantInit(&vargs); VariantInit(&vdir); VariantInit(&vop); VariantInit(&vshow);
        vargs.vt = VT_BSTR; vargs.bstrVal = SysAllocString(wargs);
        vop.vt = VT_BSTR; vop.bstrVal = SysAllocString(L"open");
        vshow.vt = VT_I4; vshow.lVal = SW_SHOWNORMAL;
        ok = SUCCEEDED(sd->lpVtbl->ShellExecute(sd, file, vargs, vdir, vop, vshow));
        SysFreeString(file);
        VariantClear(&vargs);
        VariantClear(&vop);
        free(wexe);
        free(wargs);
    }
out:
    if (sd) sd->lpVtbl->Release(sd);
    if (app) app->lpVtbl->Release(app);
    if (fvd) fvd->lpVtbl->Release(fvd);
    if (bg) bg->lpVtbl->Release(bg);
    if (sv) sv->lpVtbl->Release(sv);
    if (sb) sb->lpVtbl->Release(sb);
    if (sp) sp->lpVtbl->Release(sp);
    if (disp) disp->lpVtbl->Release(disp);
    if (sw) sw->lpVtbl->Release(sw);
    if (need_uninit) CoUninitialize();
    return ok;
}

/* Start "exe args" so that it outlives this native host. */
static int spawn(const char *exe, const char *args, int new_console)
{
    sbuf cmd = { 0 };
    cmd_append_arg(&cmd, exe);
    sb_putc(&cmd, ' ');
    sb_puts(&cmd, args);
    wchar_t *w = u8_to_w(cmd.s);
    sb_free(&cmd);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    DWORD flags = (new_console ? CREATE_NEW_CONSOLE : DETACHED_PROCESS) | CREATE_NEW_PROCESS_GROUP;
    BOOL in_job = FALSE;
    IsProcessInJob(GetCurrentProcess(), NULL, &in_job);
    BOOL ok;
    if (!in_job) {
        ok = CreateProcessW(NULL, w, NULL, NULL, FALSE, flags, NULL, NULL, &si, &pi);
    } else {
        /* Browsers run native hosts in a job that is killed when we exit.
           Leave it if allowed; otherwise let Explorer start the program. */
        ok = CreateProcessW(NULL, w, NULL, NULL, FALSE, flags | CREATE_BREAKAWAY_FROM_JOB, NULL, NULL, &si, &pi);
        if (!ok) {
            free(w);
            return shell_execute_via_explorer(exe, args);
        }
    }
    free(w);
    if (!ok) return 0;
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

/* Hand a job file to the running GUI, or start the GUI (or a console). */
static const char *deliver_job(const char *job)
{
    HWND hw = FindWindowW(UL_GUI_CLASS, NULL);
    if (hw) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hw, &pid);
        AllowSetForegroundWindow(pid);
        COPYDATASTRUCT cds;
        cds.dwData = UL_COPYDATA_JOB;
        cds.cbData = (DWORD)strlen(job) + 1;
        cds.lpData = (void *)job;
        DWORD_PTR res = 0;
        if (SendMessageTimeoutW(hw, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG, 5000, &res) && res)
            return "gui";
    }
    char *dir = exe_dir();
    char *gui = path_join(dir, UL_GUI_EXE);
    char *self = exe_path();
    free(dir);
    sbuf args = { 0 };
    cmd_append_arg(&args, "--job");
    cmd_append_arg(&args, job);
    const char *via = NULL;
    if (file_exists(gui) && spawn(gui, args.s, 0)) via = "gui";
    if (!via && self && spawn(self, args.s, 1)) via = "console";
    sb_free(&args);
    free(gui);
    free(self);
    return via;
}

static int url_allowed(const char *u)
{
    return u && strlen(u) < 16384 && !strpbrk(u, "\r\n") &&
           (str_istarts(u, "http://") || str_istarts(u, "https://") ||
            str_istarts(u, "ftp://") || str_istarts(u, "magnet:?"));
}

static void handle(const char *msg, sbuf *resp)
{
    char *action = json_str(json_find(msg, "action"));
    if (action && !strcmp(action, "ping")) {
        char *yt = find_tool("yt-dlp.exe"), *ff = find_tool("ffmpeg.exe");
        sb_printf(resp, "{\"ok\":true,\"version\":\"%s\",\"ytdlp\":%s,\"ffmpeg\":%s,\"gui\":%s}",
                  UL_VERSION, yt ? "true" : "false", ff ? "true" : "false",
                  FindWindowW(UL_GUI_CLASS, NULL) ? "true" : "false");
        free(yt);
        free(ff);
    } else if (action && !strcmp(action, "download")) {
        char *url = json_str(json_find(msg, "url"));
        if (!url_allowed(url)) {
            sb_puts(resp, "{\"ok\":false,\"error\":\"unsupported link\"}");
        } else {
            char *mode = json_str(json_find(msg, "mode"));
            char *ref = json_str(json_find(msg, "referer"));
            char *ua = json_str(json_find(msg, "ua"));
            char *name = json_str(json_find(msg, "filename"));
            char *dir = json_str(json_find(msg, "dir"));
            char *conn = json_str(json_find(msg, "connections"));
            char *qual = json_str(json_find(msg, "quality"));
            char *cookies = write_cookie_file(msg);
            if (mode && strcmp(mode, "auto") && strcmp(mode, "direct") && strcmp(mode, "ytdlp")) {
                free(mode);
                mode = NULL;
            }
            char *job = job_write(url, mode, ref, ua, name, dir, conn ? atoi(conn) : 0,
                                  qual ? atoi(qual) : 0, cookies);
            const char *via = job ? deliver_job(job) : NULL;
            if (via) {
                sb_puts(resp, "{\"ok\":true,\"via\":");
                json_put_string(resp, via);
                sb_putc(resp, '}');
            } else {
                if (job) file_delete(job);
                if (cookies) file_delete(cookies);
                sb_puts(resp, "{\"ok\":false,\"error\":\"could not start U-loader\"}");
            }
            free(job); free(mode); free(ref); free(ua); free(name); free(dir);
            free(conn); free(qual); free(cookies);
        }
        free(url);
    } else if (action && !strcmp(action, "open")) {
        HWND hw = FindWindowW(UL_GUI_CLASS, NULL);
        int ok = 0;
        if (hw) {
            DWORD pid = 0;
            GetWindowThreadProcessId(hw, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(hw, WM_APP + 7, 0, 0);       /* "show yourself" */
            ok = 1;
        } else {
            char *dir = exe_dir();
            char *gui = path_join(dir, UL_GUI_EXE);
            ok = file_exists(gui) && spawn(gui, "", 0);
            free(gui);
            free(dir);
        }
        sb_puts(resp, ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"U-loader window not found\"}");
    } else {
        sb_puts(resp, "{\"ok\":false,\"error\":\"unknown action\"}");
    }
    free(action);
}

int native_host_main(void)
{
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE), out = GetStdHandle(STD_OUTPUT_HANDLE);
    for (;;) {
        uint32_t len;
        if (!read_exact(in, &len, 4)) return 0;            /* browser closed the pipe */
        if (len == 0 || len > MAX_MSG) return 1;
        char *msg = xmalloc((size_t)len + 1);
        if (!read_exact(in, msg, len)) { free(msg); return 1; }
        msg[len] = 0;
        sbuf resp = { 0 };
        handle(msg, &resp);
        int ok = write_msg(out, resp.s);
        sb_free(&resp);
        free(msg);
        if (!ok) return 1;
    }
}

/* ------------------------------------------------------------------ */
/* registration (per user, no admin rights needed)                     */
/* ------------------------------------------------------------------ */
int host_install(int uninstall)
{
    char *dir = exe_dir();
    char *manifest = path_join(dir, "uloader-host.json");
    char *self = exe_path();
    int rc = 0;

    if (uninstall) {
        for (size_t i = 0; i < sizeof BROWSER_KEYS / sizeof *BROWSER_KEYS; i++)
            RegDeleteKeyW(HKEY_CURRENT_USER, BROWSER_KEYS[i]);
        file_delete(manifest);
        ul_log(LOG_OK, "Browser integration removed.");
        goto done;
    }

    sbuf js = { 0 };
    sb_puts(&js, "{\n  \"name\": \"" UL_HOST_NAME "\",\n  \"description\": \"U-loader download manager\",\n  \"path\": ");
    json_put_string(&js, self ? self : "uloader.exe");
    sb_puts(&js, ",\n  \"type\": \"stdio\",\n  \"allowed_origins\": [\"chrome-extension://" UL_EXT_ID "/\"]\n}\n");
    FILE *f = fopen_u8(manifest, "wb");
    if (!f || fwrite(js.s, 1, js.len, f) != js.len) {
        if (f) fclose(f);
        ul_log(LOG_ERR, "cannot write %s", manifest);
        sb_free(&js);
        rc = 1;
        goto done;
    }
    fclose(f);
    sb_free(&js);

    wchar_t *wm = u8_to_w(manifest);
    int okc = 0;
    for (size_t i = 0; i < sizeof BROWSER_KEYS / sizeof *BROWSER_KEYS; i++) {
        HKEY hk;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, BROWSER_KEYS[i], 0, NULL, 0, KEY_SET_VALUE, NULL, &hk, NULL) == ERROR_SUCCESS) {
            if (RegSetValueExW(hk, NULL, 0, REG_SZ, (const BYTE *)wm, (DWORD)((wcslen(wm) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS)
                okc++;
            RegCloseKey(hk);
        }
    }
    free(wm);
    if (!okc) { ul_log(LOG_ERR, "could not register with any browser"); rc = 1; goto done; }

    char *ext = path_join(dir, "extension");
    ul_log(LOG_OK, "Browser integration registered for Chrome, Edge, Brave and Chromium.");
    ul_log(LOG_INFO, "\nNow add the extension to your browser (one time):");
    ul_log(LOG_INFO, "  1. Open  chrome://extensions  (Edge: edge://extensions)");
    ul_log(LOG_INFO, "  2. Turn on \"Developer mode\"");
    ul_log(LOG_INFO, "  3. Click \"Load unpacked\" and choose:  %s", dir_exists(ext) ? ext : "the 'extension' folder");
    ul_log(LOG_DIM, "\nTo remove it later:  uloader --uninstall");
    free(ext);
done:
    free(dir);
    free(manifest);
    free(self);
    return rc;
}
