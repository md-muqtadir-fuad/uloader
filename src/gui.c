/*
 * U-loader - the window.
 *
 * Plain Win32 + common controls v6 (visual styles and Per-Monitor-V2 DPI
 * awareness come from the manifest embedded by gui.rc).
 *
 * Threading: every running download is task_run() on its own thread. The
 * UI thread never blocks on a download: it reads the task's volatile
 * progress fields from a 400 ms timer, copies the strings under t->lock,
 * and learns that a thread ended from WM_APP_TASKDONE. Log lines arrive
 * on the download threads and go into a per-download ring under its own
 * lock. Items that leave the list are never freed while the program runs
 * (a stuck engine worker could still reference the task), so no pointer
 * handed to a thread or posted in a message can dangle.
 *
 *   uloader-gui.exe [--job FILE] [--tray] [LINK ...]
 */
#define COBJMACROS
#include "uloader.h"
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <process.h>
#include <time.h>
#include "gui_res.h"

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef WM_CLIPBOARDUPDATE
#define WM_CLIPBOARDUPDATE 0x031D
#endif

#define WM_APP_TASKDONE  (WM_APP + 1)   /* wParam rc, lParam item*            */
#define WM_APP_TRAY      (WM_APP + 2)   /* notification icon callback        */
#define WM_APP_CLIPLINK  (WM_APP + 3)   /* lParam: malloc'ed UTF-8 link      */
#define WM_APP_DROP      (WM_APP + 4)   /* lParam: malloc'ed UTF-8 links     */
#define WM_APP_SELCHANGE (WM_APP + 5)
#define WM_APP_SHOW      (WM_APP + 7)   /* host.c: "show yourself"           */
#define WM_APP_TESTDPI   (WM_APP + 9)   /* tests only (ULOADER_TEST_DPI set): act as if the DPI became wParam */
#define WM_APP_COLSIZED  (WM_APP + 10)  /* the user resized column wParam */

#define TIMER_TICK   1
#define TIMER_SAVE   2
#define TICK_MS      400

#define ID_LIST      10
#define ID_TOOLBAR   11
#define ID_STATUS    12
#define ID_DETAIL    13
#define ID_LOG       14

#define LOG_LINES    300
#define DETAIL_CLASS L"ULoaderDetailPane"

/* colors */
#define C_TEXT    RGB(0x1B, 0x1B, 0x1B)
#define C_TEXT2   RGB(0x3C, 0x3C, 0x3C)
#define C_DIM     RGB(0x6E, 0x6E, 0x6E)
#define C_FAINT   RGB(0x9A, 0x9A, 0x9A)
#define C_LINE    RGB(0xE1, 0xE1, 0xE1)
#define C_FRAME   RGB(0xD6, 0xD6, 0xD6)
#define C_PANEL   RGB(0xF6, 0xF6, 0xF6)
#define C_WHITE   RGB(0xFF, 0xFF, 0xFF)
#define C_TRACK   RGB(0xE3, 0xE3, 0xE3)
#define C_BLUE    RGB(0x00, 0x78, 0xD4)
#define C_GREEN   RGB(0x10, 0x89, 0x3E)
#define C_AMBER   RGB(0xC8, 0x7A, 0x00)
#define C_RED     RGB(0xC4, 0x2B, 0x1C)
#define C_GRAY    RGB(0x8C, 0x8C, 0x8C)
#define C_SEL     RGB(0xCC, 0xE8, 0xFF)

/* ------------------------------------------------------------------ */
/* types                                                               */
/* ------------------------------------------------------------------ */
enum { COL_NAME, COL_SIZE, COL_PROGRESS, COL_SPEED, COL_ETA, COL_STATUS, COL_ADDED, NCOLS };

static const wchar_t *COL_TITLES[NCOLS] = { L"Name", L"Size", L"Progress", L"Speed", L"Time left",
                                            L"Status", L"Added" };
static const int COL_DEFW[NCOLS] = { 300, 78, 140, 86, 74, 136, 136 };
static const int COL_FMT[NCOLS] = { LVCFMT_LEFT, LVCFMT_RIGHT, LVCFMT_LEFT, LVCFMT_RIGHT,
                                    LVCFMT_RIGHT, LVCFMT_LEFT, LVCFMT_LEFT };

typedef struct {
    char *text;              /* "hh:mm:ss  message" */
    int   kind;              /* LOG_*               */
} logline;

typedef struct item item;
struct item {
    ul_task   *t;
    /* how to download it (persisted) */
    char      *dir, *out_name, *referer, *ua, *cookie_file;
    int        mode, conns, quality;
    int64_t    added;        /* unix time */
    unsigned   seq;

    /* UI thread only */
    int        state;        /* TS_* as shown in the list */
    HANDLE     thread;       /* non-NULL while task_run runs */
    ul_config *runcfg;       /* handed to (and freed by) the thread */
    volatile LONG rc;
    int        resume_after; /* Resume pressed while pausing */
    int        removing;     /* 1 remove, 2 remove + delete file, 3 finished */
    int        icon;         /* image list index, -1 = not looked up */
    char       icon_ext[16];

    /* snapshot of the task (UI thread) */
    int64_t    done, total, speed, eta;
    int        nconn, pausing;
    char       name[260];
    char       stage[96];
    char       status[512];
    char      *path;
    unsigned char map[UL_SEGVIEW];

    /* log, appended by download threads */
    CRITICAL_SECTION loglock;
    logline    log[LOG_LINES];
    int        log_head, log_count;
    unsigned   log_seq;
};

typedef struct {
    char *dir;
    int   conns, max_active, limit_kb, clip, tray, tray_hint;
    int   has_place, pl, pt, pr, pb, pmax;
    int   detail_h;          /* 96-dpi pixels */
    int   colw[NCOLS];       /* 96-dpi pixels */
    int   has_cols;          /* widths came from the settings file */
    int   order[NCOLS];
} settings;

/* ------------------------------------------------------------------ */
/* globals                                                             */
/* ------------------------------------------------------------------ */
static HINSTANCE g_inst;
static HWND      g_hwnd, g_lv, g_tb, g_sb, g_detail, g_log;
static HACCEL    g_accel;
static UINT      g_dpi = 96, g_dpi_override;
static HFONT     g_font, g_font_bold, g_font_title, g_font_mono;
static int       g_line_h = 16, g_title_h = 21, g_mono_h = 15;
static HIMAGELIST g_il_tb, g_il_tb_dis, g_il_files;
static HICON     g_icon_big, g_icon_small, g_icon_tray, g_icon_balloon;
static int       g_tb_area, g_tb_state[16];
static RECT      g_split_rc, g_log_frame;
static int       g_list_w = -1;          /* list width seen last (-1 until placed) */
static int       g_split_drag, g_split_grab;

static item    **g_items;  static int g_n, g_cap;
static item    **g_gone;   static int g_ngone, g_capgone;
static unsigned  g_seq;
static settings  g_set;
static char     *g_datadir;

static int       g_exiting, g_exit_pending, g_modal;
static HWND      g_dlg, g_add_dlg;
static int       g_sort_col = -1, g_sort_desc;
static int       g_tray_added, g_clip_listening, g_sel_posted;
static UINT      g_wm_taskbar;
static DWORD     g_own_clip_seq;
static char     *g_seen[64]; static int g_seen_pos;
static item     *g_detail_item, *g_notify_item;
static unsigned  g_log_shown_seq;
static int       g_detail_dirty;
static double    g_last_periodic_save;
static int       g_have_mdl2;

/* settings-dialog helpers (browser connect, yt-dlp) */
static CRITICAL_SECTION g_help_lock;
static sbuf      g_help_out;
static volatile LONG g_help_seq, g_connect_busy, g_yt_busy;
static HANDLE    g_connect_thread, g_yt_thread;
static ul_task  *g_yt_task;

/* DPI functions (Windows 10 1607+), loaded at run time */
static UINT (WINAPI *pGetDpiForWindow)(HWND);
static UINT (WINAPI *pGetDpiForSystem)(void);
static BOOL (WINAPI *pSystemParametersInfoForDpi)(UINT, UINT, PVOID, UINT, UINT);
static int  (WINAPI *pGetSystemMetricsForDpi)(int, UINT);
static BOOL (WINAPI *pSetProcessDpiAwarenessContext)(HANDLE);

/* COM ids (defined here so no libuuid is needed) */
static const CLSID CLSID_FileOpenDialog_ = { 0xdc1c5a9c, 0xe88a, 0x4dde, { 0xa5, 0xa1, 0x60, 0xf8, 0x2a, 0x20, 0xae, 0xf7 } };
static const IID IID_IFileOpenDialog_   = { 0xd57c7288, 0xd4ad, 0x4768, { 0xbe, 0x02, 0x9d, 0x96, 0x95, 0x32, 0xd9, 0x60 } };
static const IID IID_IShellItem_        = { 0x43826d1e, 0xe718, 0x42ee, { 0xbc, 0x55, 0xa1, 0xe2, 0x61, 0xc3, 0x7b, 0xfe } };
static const IID IID_IUnknown_          = { 0x00000000, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const IID IID_IDropTarget_       = { 0x00000122, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

static const int  QUALITY_VALUES[] = { 0, 2160, 1440, 1080, 720, 480, 360 };
static const wchar_t *QUALITY_NAMES[] = { L"Best available", L"2160p (4K)", L"1440p", L"1080p",
                                          L"720p", L"480p", L"360p" };
static const wchar_t *MODE_NAMES[] = { L"Auto (detect)", L"Direct file", L"Video page (yt-dlp)" };

/* forward declarations */
static void list_changed(void);
static void schedule(void);
static void detail_refresh(int force);
static void update_toolbar(void);
static void update_status(void);
static double g_status_note_until;     /* a status-bar note shows until then */
static void open_add_dialog(const char *prefill, int topmost);
static void do_exit(void);
static void layout(void);

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */
/* Optional trace of UI decisions: set ULOADER_TRACE=<file> (diagnostics). */
static FILE *g_trace;

static void trace(const char *fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
static void trace(const char *fmt, ...)
{
    if (!g_trace) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_trace, "%02u:%02u:%02u.%03u  ", (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
            (unsigned)st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_trace, fmt, ap);
    va_end(ap);
    fputc('\n', g_trace);
    fflush(g_trace);
}

static int S(int v) { return MulDiv(v, (int)g_dpi, 96); }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* UTF-8 -> fixed wide buffer, truncating */
static void u8w(const char *s, wchar_t *out, int n)
{
    if (n <= 0) return;
    wchar_t *w = u8_to_w(s ? s : "");
    lstrcpynW(out, w, n);
    free(w);
}

/* "C:\..." or "\\server\share": anything else would depend on our current
   folder. Backslashes only: the shell (Recycle Bin, Explorer selection)
   does not take "C:/x" - typed folders go through to_backslashes() first. */
static int is_abs_path(const char *p)
{
    return p && ((isalpha((unsigned char)p[0]) && p[1] == ':' && p[2] == '\\') ||
                 (p[0] == '\\' && p[1] == '\\'));
}

static char *to_backslashes(char *p)
{
    for (char *c = p; c && *c; c++) if (*c == '/') *c = '\\';
    return p;
}

static void set_str(char **dst, const char *src)
{
    free(*dst);
    *dst = (src && *src) ? xstrdup(src) : NULL;
}

static void fill(HDC dc, const RECT *r, COLORREF c)
{
    SetDCBrushColor(dc, c);
    FillRect(dc, r, (HBRUSH)GetStockObject(DC_BRUSH));
}

static void text_u8(HDC dc, const char *s, RECT *r, UINT fmt)
{
    wchar_t *w = u8_to_w(s ? s : "");
    DrawTextW(dc, w, -1, r, fmt | DT_NOPREFIX);
    free(w);
}

static int text_width(HDC dc, HFONT f, const wchar_t *s)
{
    HGDIOBJ o = SelectObject(dc, f);
    SIZE sz = { 0, 0 };
    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    SelectObject(dc, o);
    return sz.cx;
}

static COLORREF lerp(COLORREF a, COLORREF b, int t)   /* t = 0..255 */
{
    return RGB(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t / 255,
               GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t / 255,
               GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t / 255);
}

static char *dlg_text(HWND d, int id)
{
    HWND h = GetDlgItem(d, id);
    int n = GetWindowTextLengthW(h);
    wchar_t *w = xcalloc((size_t)n + 1, sizeof(wchar_t));
    GetWindowTextW(h, w, n + 1);
    char *u = w_to_u8(w);
    free(w);
    return u;
}

static void set_dlg_text(HWND d, int id, const char *s)
{
    wchar_t *w = u8_to_w(s ? s : "");
    SetDlgItemTextW(d, id, w);
    free(w);
}

/* "\n" -> "\r\n" for edit controls */
static char *crlf(const char *s)
{
    sbuf b = { 0 };
    for (; *s; s++) {
        if (*s == '\n' && (b.len == 0 || b.s[b.len - 1] != '\r')) sb_putc(&b, '\r');
        sb_putc(&b, *s);
    }
    return sb_steal(&b);
}

static int is_link_prefix(const char *p, size_t n)
{
    static const char *pre[] = { "http://", "https://", "ftp://", "magnet:?" };
    for (size_t i = 0; i < sizeof pre / sizeof *pre; i++) {
        size_t k = strlen(pre[i]);
        if (n > k && _strnicmp(p, pre[i], k) == 0) return 1;
    }
    return 0;
}

/* All http/https/ftp/magnet links in a text, one per line; NULL if none. */
static char *extract_links(const char *text)
{
    sbuf b = { 0 };
    int n = 0;
    const char *p = text;
    while (p && *p) {
        size_t len = strcspn(p, " \t\r\n<>\"'");
        if (len == 0) { p++; continue; }
        if (is_link_prefix(p, len) && len < 16384) {
            if (n++) sb_putc(&b, '\n');
            sb_putn(&b, p, len);
        }
        p += len;
    }
    if (!n) { sb_free(&b); return NULL; }
    return sb_steal(&b);
}

/* Links the user typed: one per line, or several per line separated by
   spaces. A line that is an existing .torrent file is kept whole. */
static int parse_links(const char *text, char ***out)
{
    char **v = NULL;
    int n = 0;
    char *copy = xstrdup(text ? text : ""), *cur = copy;
    while (*cur) {
        char *nl = strpbrk(cur, "\r\n");
        if (nl) *nl = 0;
        char *line = str_trim(cur);
        cur = nl ? nl + 1 : cur + strlen(cur);
        if (!*line) continue;
        if (str_iends(line, ".torrent") && !strstr(line, "://") && file_exists(line)) {
            v = xrealloc(v, sizeof *v * (size_t)(n + 1));
            v[n++] = xstrdup(line);
            continue;
        }
        char *save = NULL;
        for (char *tok = strtok_r(line, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
            while (*tok == '<' || *tok == '"' || *tok == '\'') tok++;
            size_t k = strlen(tok);
            while (k && (tok[k - 1] == '>' || tok[k - 1] == '"' || tok[k - 1] == '\'')) tok[--k] = 0;
            if (!k) continue;
            int ok = is_link_prefix(tok, k) ||
                     (!strstr(tok, "://") && strchr(tok, '.') && isalnum((unsigned char)tok[0]) && !strchr(tok, '\\'));
            if (!ok) continue;
            v = xrealloc(v, sizeof *v * (size_t)(n + 1));
            v[n++] = xstrdup(tok);
        }
    }
    free(copy);
    *out = v;
    return n;
}

static void free_strv(char **v, int n)
{
    for (int i = 0; i < n; i++) free(v[i]);
    free(v);
}

/* ------------------------------------------------------------------ */
/* names and texts                                                     */
/* ------------------------------------------------------------------ */
static char *magnet_name(const char *url)
{
    const char *q = strchr(url, '?');
    for (const char *p = q; p && *p; p = strchr(p + 1, '&')) {
        const char *k = p + 1;
        if (_strnicmp(k, "dn=", 3) == 0) {
            size_t n = strcspn(k + 3, "&#");
            char *v = url_decode(k + 3, n);
            for (char *c = v; *c; c++) if (*c == '+') *c = ' ';
            if (*v) return v;
            free(v);
            return NULL;
        }
    }
    return NULL;
}

/* "youtube.com video (GpapVTX1umU)" for a video page link */
static char *video_page_name(const char *url)
{
    char *host = url_host(url);
    const char *h = host;
    if (str_istarts(h, "www.")) h += 4;
    else if (str_istarts(h, "m.")) h += 2;
    const char *q = strchr(url, '?');
    const char *v = NULL;                 /* the "v" parameter, not the end of "...v=" (nav=, dev=) */
    for (const char *p = q; p && (p = strstr(p, "v=")) != NULL; p += 2)
        if (p[-1] == '?' || p[-1] == '&') { v = p; break; }
    if (v) {
        size_t idl = strcspn(v + 2, "&#");
        if (idl > 0 && idl < 40) {
            char *r = xasprintf("%s video (%.*s)", h, (int)idl, v + 2);
            free(host);
            return r;
        }
    }
    char *r = xasprintf("%s video", h);
    free(host);
    return r;
}

/* name to show before the engine knows the real one */
static void guess_name(const item *it, char *buf, size_t n)
{
    const char *url = it->t->url;
    char *nm = NULL;
    if (it->out_name && *it->out_name) nm = xstrdup(it->out_name);
    else if (str_istarts(url, "magnet:")) nm = magnet_name(url);
    else if (strstr(url, "://") && is_video_site(url)) nm = video_page_name(url);
    else if (!strstr(url, "://")) nm = xstrdup(path_basename(url));
    else nm = url_filename(url);
    if (!nm || !*nm) {
        free(nm);
        nm = str_istarts(url, "magnet:") ? xstrdup("Magnet link") : url_host(url);
        if (!nm || !*nm) { free(nm); nm = xstrdup(url); }
    }
    snprintf(buf, n, "%s", nm);
    free(nm);
}

static double item_frac(const item *it)
{
    if (it->state == TS_DONE) return 1.0;
    if (it->total > 0) {
        double f = (double)it->done / (double)it->total;
        return f < 0 ? 0 : f > 1 ? 1 : f;
    }
    return 0.0;
}

static void status_text(const item *it, char *buf, size_t n)
{
    switch (it->state) {
    case TS_QUEUED:  snprintf(buf, n, "Queued"); break;
    case TS_PAUSED:  snprintf(buf, n, "Paused"); break;
    case TS_DONE:    snprintf(buf, n, "Complete"); break;
    case TS_ERROR:
        if (it->status[0]) snprintf(buf, n, "Error: %s", it->status);
        else snprintf(buf, n, "Error");
        break;
    default:
        if (it->pausing) snprintf(buf, n, "Pausing...");
        else if (!strcmp(it->stage, "finding video")) snprintf(buf, n, "Finding video...");
        else if (!strcmp(it->stage, "merging")) snprintf(buf, n, "Merging...");
        else if (!strcmp(it->stage, "joining")) snprintf(buf, n, "Joining...");
        else if (it->done == 0 && it->total < 0) snprintf(buf, n, "Connecting...");
        else snprintf(buf, n, "Downloading");
        break;
    }
}

static COLORREF state_color(const item *it)       /* bar / map fill */
{
    switch (it->state) {
    case TS_DONE:   return C_GREEN;
    case TS_ERROR:  return C_RED;
    case TS_PAUSED: return C_AMBER;
    case TS_QUEUED: return C_GRAY;
    default:        return it->pausing ? C_AMBER : C_BLUE;
    }
}

static COLORREF state_text_color(const item *it)
{
    switch (it->state) {
    case TS_DONE:   return RGB(0x0E, 0x7A, 0x36);
    case TS_ERROR:  return C_RED;
    case TS_PAUSED: return RGB(0x9A, 0x5B, 0x00);
    case TS_QUEUED: return C_DIM;
    default:        return RGB(0x00, 0x5F, 0xB8);
    }
}

static void fmt_added(int64_t t, char *buf, size_t n)
{
    if (t <= 0) { snprintf(buf, n, "%s", ""); return; }
    ULARGE_INTEGER u;
    u.QuadPart = ((uint64_t)t + 11644473600ULL) * 10000000ULL;
    FILETIME ft, lft;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&ft, &lft) || !FileTimeToSystemTime(&lft, &st)) { buf[0] = 0; return; }
    wchar_t d[64], tm[64];
    if (!GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &st, NULL, d, 64)) d[0] = 0;
    if (!GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &st, NULL, tm, 64)) tm[0] = 0;
    char *a = w_to_u8(d), *b = w_to_u8(tm);
    snprintf(buf, n, "%s %s", a, b);
    free(a);
    free(b);
}

/* ------------------------------------------------------------------ */
/* data folder, settings                                               */
/* ------------------------------------------------------------------ */
static const char *data_dir(void)
{
    if (g_datadir) return g_datadir;
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"ULOADER_DATA", buf, MAX_PATH * 2);   /* tests / portable use */
    if (n && n < MAX_PATH * 2) {
        g_datadir = w_to_u8(buf);
    } else {
        n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH * 2);
        if (n && n < MAX_PATH * 2) {
            char *a = w_to_u8(buf);
            g_datadir = path_join(a, "U-loader");
            free(a);
        } else {
            char *d = exe_dir();
            g_datadir = path_join(d, "U-loader-data");
            free(d);
        }
    }
    dir_create(g_datadir);
    return g_datadir;
}

/* "key=value" lines; values are stored literally (paths keep their
   backslashes, so the files stay hand-editable). Links, names and paths
   never contain line breaks; any in a status message become spaces. */
static void put_kv(FILE *f, const char *k, const char *v)
{
    if (!v || !*v) return;
    fprintf(f, "%s=", k);
    for (const char *p = v; *p; p++) fputc(*p == '\n' || *p == '\r' ? ' ' : *p, f);
    fputc('\n', f);
}

/* write-to-temp + rename */
static FILE *begin_write(const char *name, char **path, char **tmp)
{
    *path = path_join(data_dir(), name);
    *tmp = xasprintf("%s.tmp", *path);
    FILE *f = fopen_u8(*tmp, "wb");
    if (!f) { free(*path); free(*tmp); *path = *tmp = NULL; }
    return f;
}

static int end_write(FILE *f, char *path, char *tmp)
{
    /* any failed write (disk full) must keep the old file, never replace
       it with a cut-off one */
    int ok = fflush(f) == 0 && !ferror(f);
    if (fclose(f) != 0) ok = 0;
    if (ok) {
        /* the old file may be open in another program for a moment (virus
           scanner, backup, an editor): try a few times */
        wchar_t *a = wpath(tmp), *b = wpath(path);
        for (int i = 0; i < 4; i++) {
            ok = MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
            if (ok) break;
            Sleep(25);
        }
        free(a);
        free(b);
    }
    if (!ok) file_delete(tmp);
    free(path);
    free(tmp);
    return ok ? 0 : -1;
}

static int parse_ints(const char *v, int *out, int max)
{
    int n = 0;
    while (*v && n < max) {
        char *end;
        long x = strtol(v, &end, 10);
        if (end == v) break;
        out[n++] = (int)x;
        v = end;
        while (*v == ',' || *v == ' ') v++;
    }
    return n;
}

static void settings_defaults(settings *s)
{
    memset(s, 0, sizeof *s);
    s->dir = downloads_dir();
    s->conns = 8;
    s->max_active = 3;
    s->limit_kb = 0;
    s->clip = 1;
    s->tray = 1;
    s->detail_h = 250;
    for (int i = 0; i < NCOLS; i++) { s->colw[i] = COL_DEFW[i]; s->order[i] = i; }
}

static void settings_load(void)
{
    settings_defaults(&g_set);
    char *path = path_join(data_dir(), "settings.txt");
    size_t len;
    char *data = read_file(path, &len);
    free(path);
    if (!data) return;
    char *cur = data;
    while (*cur) {
        char *nl = strchr(cur, '\n');
        if (nl) *nl = 0;
        char *line = cur;
        cur = nl ? nl + 1 : cur + strlen(cur);
        size_t ll = strlen(line);
        if (ll && line[ll - 1] == '\r') line[--ll] = 0;
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        int iv[NCOLS + 1];
        if (!strcmp(k, "dir")) {
            char *dd = to_backslashes(xstrdup(v));
            if (is_abs_path(dd)) set_str(&g_set.dir, dd);
            free(dd);
        }
        else if (!strcmp(k, "connections")) g_set.conns = clampi(atoi(v), 1, UL_MAX_CONN);
        else if (!strcmp(k, "max_active")) g_set.max_active = clampi(atoi(v), 1, 20);
        else if (!strcmp(k, "speed_limit_kb")) g_set.limit_kb = clampi(atoi(v), 0, 10000000);
        else if (!strcmp(k, "clipboard")) g_set.clip = atoi(v) != 0;
        else if (!strcmp(k, "tray")) g_set.tray = atoi(v) != 0;
        else if (!strcmp(k, "tray_hint")) g_set.tray_hint = atoi(v) != 0;
        else if (!strcmp(k, "detail")) g_set.detail_h = clampi(atoi(v), 60, 3000);
        else if (!strcmp(k, "window") && parse_ints(v, iv, 5) == 5 && iv[2] > iv[0] + 100 && iv[3] > iv[1] + 100) {
            g_set.has_place = 1;
            g_set.pl = iv[0]; g_set.pt = iv[1]; g_set.pr = iv[2]; g_set.pb = iv[3]; g_set.pmax = iv[4] != 0;
        }
        else if (!strcmp(k, "columns") && parse_ints(v, iv, NCOLS) == NCOLS) {
            for (int i = 0; i < NCOLS; i++) g_set.colw[i] = clampi(iv[i], 24, 2000);
            g_set.has_cols = 1;
        }
        else if (!strcmp(k, "order") && parse_ints(v, iv, NCOLS) == NCOLS) {
            int seen = 0, ok = 1;
            for (int i = 0; i < NCOLS; i++) {
                if (iv[i] < 0 || iv[i] >= NCOLS || (seen & (1 << iv[i]))) ok = 0;
                else seen |= 1 << iv[i];
            }
            if (ok) memcpy(g_set.order, iv, sizeof g_set.order);
        }
    }
    free(data);
}

/* window placement, column widths and order -> g_set */
static void capture_layout(void)
{
    if (!g_hwnd) return;
    WINDOWPLACEMENT wp;
    memset(&wp, 0, sizeof wp);
    wp.length = sizeof wp;
    if (GetWindowPlacement(g_hwnd, &wp)) {
        g_set.has_place = 1;
        g_set.pl = wp.rcNormalPosition.left;
        g_set.pt = wp.rcNormalPosition.top;
        g_set.pr = wp.rcNormalPosition.right;
        g_set.pb = wp.rcNormalPosition.bottom;
        g_set.pmax = IsZoomed(g_hwnd) || (IsIconic(g_hwnd) && (wp.flags & WPF_RESTORETOMAXIMIZED));
    }
    if (g_lv) {
        /* column widths: g_set.colw already holds the widths the user chose
           (the visible ones may be squeezed to fit a narrow window) */
        int ord[NCOLS];
        if (ListView_GetColumnOrderArray(g_lv, NCOLS, ord)) memcpy(g_set.order, ord, sizeof ord);
    }
}

static void settings_save(void)
{
    capture_layout();
    char *path, *tmp;
    FILE *f = begin_write("settings.txt", &path, &tmp);
    if (!f) return;
    fputs("# U-loader window settings\n", f);
    put_kv(f, "dir", g_set.dir);
    fprintf(f, "connections=%d\nmax_active=%d\nspeed_limit_kb=%d\nclipboard=%d\ntray=%d\ntray_hint=%d\ndetail=%d\n",
            g_set.conns, g_set.max_active, g_set.limit_kb, g_set.clip, g_set.tray, g_set.tray_hint, g_set.detail_h);
    if (g_set.has_place)
        fprintf(f, "window=%d,%d,%d,%d,%d\n", g_set.pl, g_set.pt, g_set.pr, g_set.pb, g_set.pmax);
    fputs("columns=", f);
    for (int i = 0; i < NCOLS; i++) fprintf(f, "%s%d", i ? "," : "", g_set.colw[i]);
    fputs("\norder=", f);
    for (int i = 0; i < NCOLS; i++) fprintf(f, "%s%d", i ? "," : "", g_set.order[i]);
    fputc('\n', f);
    end_write(f, path, tmp);
}

/* a fresh heap config with the window's settings */
static ul_config *default_cfg(void)
{
    ul_config *c = xcalloc(1, sizeof *c);
    cfg_defaults(c);
    c->out_dir = xstrdup(g_set.dir);
    c->connections = g_set.conns;
    c->speed_limit = (int64_t)g_set.limit_kb * 1024;
    return c;
}

/* ------------------------------------------------------------------ */
/* items                                                               */
/* ------------------------------------------------------------------ */
static void log_push(item *it, int kind, char *line)
{
    int idx;
    if (it->log_count < LOG_LINES) {
        idx = (it->log_head + it->log_count) % LOG_LINES;
        it->log_count++;
    } else {
        idx = it->log_head;
        free(it->log[idx].text);
        it->log_head = (it->log_head + 1) % LOG_LINES;
    }
    it->log[idx].text = line;
    it->log[idx].kind = kind;
    it->log_seq++;
}

/* t->on_log: called on download threads */
static void gui_on_log(ul_task *t, int kind, const char *msg)
{
    item *it = t ? t->user : NULL;
    if (!it || !msg) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char tm[16];
    snprintf(tm, sizeof tm, "%02u:%02u:%02u", (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond);
    EnterCriticalSection(&it->loglock);
    if (it->removing != 3) {
        const char *p = msg;
        for (;;) {
            size_t n = strcspn(p, "\r\n"), k = 0;
            while (k < n && p[k] == ' ') k++;
            if (k < n) log_push(it, kind, xasprintf("%s  %.*s", tm, (int)n, p));
            if (!p[n]) break;
            p += n + 1;
        }
    }
    LeaveCriticalSection(&it->loglock);
}

/* t->on_progress: the timer polls instead */
static void gui_on_progress(ul_task *t) { (void)t; }

static void item_log(item *it, int kind, const char *fmt, ...) __attribute__((format(gnu_printf, 3, 4)));
static void item_log(item *it, int kind, const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    gui_on_log(it->t, kind, buf);
}

static item *item_new(const char *url)
{
    item *it = xcalloc(1, sizeof *it);
    it->t = task_new(url);
    it->t->user = it;
    it->t->on_log = gui_on_log;
    it->t->on_progress = gui_on_progress;
    InitializeCriticalSection(&it->loglock);
    it->added = (int64_t)time(NULL);
    it->seq = ++g_seq;
    it->state = TS_QUEUED;
    it->icon = -1;
    it->total = -1;
    it->eta = -1;
    it->mode = MODE_AUTO;
    it->conns = g_set.conns;
    it->dir = xstrdup(g_set.dir);
    return it;
}

/* Copy the task's fields into the item. Returns 1 if anything shown changed. */
static int snap_update(item *it)
{
    ul_task *t = it->t;
    int64_t done = t->done, total = t->total, speed = t->speed, eta = t->eta;
    int nconn = (int)t->conns;
    int pausing = it->thread && t->abort;
    char name[260], stage[96], status[512];
    unsigned char map[UL_SEGVIEW];
    int changed = 0;

    EnterCriticalSection(&t->lock);
    snprintf(name, sizeof name, "%s", t->name ? t->name : "");
    memcpy(stage, t->stage, sizeof stage);
    memcpy(status, t->status, sizeof status);
    memcpy(map, t->segmap, sizeof map);
    if (t->path && (!it->path || strcmp(t->path, it->path))) {
        free(it->path);
        it->path = xstrdup(t->path);
        changed = 1;
    }
    LeaveCriticalSection(&t->lock);
    stage[sizeof stage - 1] = 0;
    status[sizeof status - 1] = 0;
    if (!name[0]) guess_name(it, name, sizeof name);
    if (!it->thread) { speed = 0; eta = -1; nconn = 0; }

    if (strcmp(name, it->name)) {
        snprintf(it->name, sizeof it->name, "%s", name);
        const char *ext = path_ext(name);
        if (_stricmp(ext, it->icon_ext)) it->icon = -1;
        changed = 1;
    }
    if (done != it->done || total != it->total || speed != it->speed || eta != it->eta ||
        nconn != it->nconn || pausing != it->pausing || strcmp(stage, it->stage) ||
        strcmp(status, it->status) || memcmp(map, it->map, sizeof map)) changed = 1;
    it->done = done;
    it->total = total;
    it->speed = speed;
    it->eta = eta;
    it->nconn = nconn;
    it->pausing = pausing;
    memcpy(it->stage, stage, sizeof stage);
    memcpy(it->status, status, sizeof status);
    memcpy(it->map, map, sizeof map);
    return changed;
}

static int index_of(const item *it)
{
    for (int i = 0; i < g_n; i++) if (g_items[i] == it) return i;
    return -1;
}

static void redraw_item(item *it)
{
    int i = index_of(it);
    if (i >= 0) ListView_RedrawItems(g_lv, i, i);
    if (it == g_detail_item) g_detail_dirty = 1;
}

/* the engine's resume file -> segment map of a paused download */
static void restore_segmap(item *it)
{
    if (!it->path) return;
    char *st = xasprintf("%s.ulstate", it->path);
    size_t len;
    char *data = read_file(st, &len);
    free(st);
    if (!data) return;
    if (strncmp(data, "ULSTATE 1", 9) != 0) { free(data); return; }
    int64_t total = -1, a[256], b[256];
    int n = 0;
    for (char *line = strtok(data, "\n"); line; line = strtok(NULL, "\n")) {
        long long x, y;
        if (!strncmp(line, "total ", 6)) total = _strtoi64(line + 6, NULL, 10);
        else if (!strncmp(line, "seg ", 4) && n < 256 && sscanf(line + 4, "%lld %lld", &x, &y) == 2 && y > x && x >= 0) {
            a[n] = x;
            b[n] = y;
            n++;
        }
    }
    free(data);
    if (total <= 0) return;
    unsigned char map[UL_SEGVIEW];
    int64_t missing = 0;
    for (int k = 0; k < n; k++) missing += (b[k] > total ? total : b[k]) - a[k];
    for (int i = 0; i < UL_SEGVIEW; i++) {
        int64_t lo0 = share_of(total, i, UL_SEGVIEW), hi0 = share_of(total, i + 1, UL_SEGVIEW), miss = 0;
        for (int k = 0; k < n; k++) {
            int64_t lo = a[k] > lo0 ? a[k] : lo0, hi = b[k] < hi0 ? b[k] : hi0;
            if (hi > lo) miss += hi - lo;
        }
        map[i] = hi0 > lo0 ? (unsigned char)(255 - (miss * 255) / (hi0 - lo0)) : 255;
    }
    EnterCriticalSection(&it->t->lock);
    memcpy(it->t->segmap, map, sizeof map);
    LeaveCriticalSection(&it->t->lock);
    it->t->total = total;
    it->t->done = missing < total ? total - missing : 0;
}

/* ------------------------------------------------------------------ */
/* the download list on disk                                           */
/* ------------------------------------------------------------------ */
static const char *STATE_NAMES[] = { "queued", "running", "paused", "done", "error" };

static void save_list(void)
{
    if (g_hwnd) KillTimer(g_hwnd, TIMER_SAVE);
    char *path, *tmp;
    FILE *f = begin_write("downloads.txt", &path, &tmp);
    if (!f) {
        if (g_hwnd && !g_exiting) SetTimer(g_hwnd, TIMER_SAVE, 2000, NULL);     /* try again */
        return;
    }
    fputs("ULOADER-LIST 1\n", f);
    for (int i = 0; i < g_n; i++) {
        item *it = g_items[i];
        int st = it->state;          /* anything but done/error comes back paused */
        if (st < TS_QUEUED || st > TS_ERROR) st = TS_PAUSED;
        char *name = NULL;
        EnterCriticalSection(&it->t->lock);
        if (it->t->name) name = xstrdup(it->t->name);
        LeaveCriticalSection(&it->t->lock);
        fputs("[download]\n", f);
        put_kv(f, "url", it->t->url);
        put_kv(f, "name", name);
        put_kv(f, "path", it->path);
        put_kv(f, "dir", it->dir);
        put_kv(f, "out_name", it->out_name);
        put_kv(f, "referer", it->referer);
        put_kv(f, "ua", it->ua);
        put_kv(f, "cookies", it->cookie_file);
        fprintf(f, "mode=%d\nconnections=%d\nquality=%d\nstate=%s\ntotal=%lld\ndone=%lld\nadded=%lld\n",
                it->mode, it->conns, it->quality, STATE_NAMES[st], (long long)it->total,
                (long long)it->done, (long long)it->added);
        if (st == TS_ERROR) put_kv(f, "status", it->status);
        fputs("end\n", f);
        free(name);
    }
    if (end_write(f, path, tmp) != 0 && g_hwnd && !g_exiting)
        SetTimer(g_hwnd, TIMER_SAVE, 2000, NULL);                              /* try again */
    g_last_periodic_save = now_sec();
}

typedef struct {
    char *url, *name, *path, *dir, *out_name, *referer, *ua, *cookies, *status;
    int mode, conns, quality, state;
    long long total, done, added;
} record;

static void record_clear(record *r)
{
    free(r->url); free(r->name); free(r->path); free(r->dir); free(r->out_name);
    free(r->referer); free(r->ua); free(r->cookies); free(r->status);
    memset(r, 0, sizeof *r);
    r->total = -1;
    r->state = TS_PAUSED;
}

static void list_append(item *it)
{
    if (g_n == g_cap) {
        g_cap = g_cap ? g_cap * 2 : 64;
        g_items = xrealloc(g_items, sizeof *g_items * (size_t)g_cap);
    }
    g_items[g_n++] = it;
}

static void record_finish(record *r)
{
    if (!r->url || !*r->url) return;
    item *it = item_new(r->url);
    set_str(&it->dir, r->dir ? r->dir : g_set.dir);
    set_str(&it->out_name, r->out_name);
    set_str(&it->referer, r->referer);
    set_str(&it->ua, r->ua);
    set_str(&it->cookie_file, r->cookies);
    it->mode = clampi(r->mode, MODE_AUTO, MODE_YTDLP);
    it->conns = clampi(r->conns > 0 ? r->conns : g_set.conns, 1, UL_MAX_CONN);
    it->quality = r->quality > 0 ? r->quality : 0;
    if (r->added > 0) it->added = r->added;
    it->state = r->state;
    ul_task *t = it->t;
    if (r->name || r->path) task_set_file(t, r->name, r->path);
    if (r->path) it->path = xstrdup(r->path);
    t->total = r->total;
    t->done = r->done;
    t->state = it->state;
    if (it->state == TS_DONE) {
        memset(t->segmap, 255, sizeof t->segmap);
        if (t->total > 0) t->done = t->total;
    } else {
        restore_segmap(it);
    }
    if (r->status) snprintf(t->status, sizeof t->status, "%s", r->status);
    else if (it->state == TS_PAUSED) snprintf(t->status, sizeof t->status, "Paused");
    char a[32], b[32];
    if (it->state == TS_DONE) item_log(it, LOG_DIM, "Completed in an earlier session.");
    else if (it->state == TS_ERROR) item_log(it, LOG_DIM, "Failed in an earlier session: %s", r->status ? r->status : "error");
    else if (t->total > 0)
        item_log(it, LOG_DIM, "Paused in an earlier session at %s of %s. Resume to continue.",
                 fmt_size(t->done, a, sizeof a), fmt_size(t->total, b, sizeof b));
    else item_log(it, LOG_DIM, "Paused in an earlier session. Resume to continue.");
    snap_update(it);
    list_append(it);
}

static void load_list(void)
{
    char *path = path_join(data_dir(), "downloads.txt");
    size_t len;
    char *data = read_file(path, &len);
    if (!data) {
        char *tmp = xasprintf("%s.tmp", path);
        data = read_file(tmp, &len);
        free(tmp);
    }
    free(path);
    if (!data) return;
    if (strncmp(data, "ULOADER-LIST 1", 14) != 0) { free(data); return; }
    record r;
    memset(&r, 0, sizeof r);
    record_clear(&r);
    int open = 0;
    char *cur = data;
    while (*cur) {
        char *nl = strchr(cur, '\n');
        if (nl) *nl = 0;
        char *line = cur;
        cur = nl ? nl + 1 : cur + strlen(cur);
        size_t ll = strlen(line);
        if (ll && line[ll - 1] == '\r') line[--ll] = 0;
        if (!strcmp(line, "[download]")) {
            if (open) record_finish(&r);
            record_clear(&r);
            open = 1;
            continue;
        }
        if (!strcmp(line, "end")) {
            if (open) record_finish(&r);
            record_clear(&r);
            open = 0;
            continue;
        }
        char *eq = strchr(line, '=');
        if (!open || !eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        char **s = NULL;
        if (!strcmp(k, "url")) s = &r.url;
        else if (!strcmp(k, "name")) s = &r.name;
        else if (!strcmp(k, "path")) s = &r.path;
        else if (!strcmp(k, "dir")) s = &r.dir;
        else if (!strcmp(k, "out_name")) s = &r.out_name;
        else if (!strcmp(k, "referer")) s = &r.referer;
        else if (!strcmp(k, "ua")) s = &r.ua;
        else if (!strcmp(k, "cookies")) s = &r.cookies;
        else if (!strcmp(k, "status")) s = &r.status;
        else if (!strcmp(k, "mode")) r.mode = atoi(v);
        else if (!strcmp(k, "connections")) r.conns = atoi(v);
        else if (!strcmp(k, "quality")) r.quality = atoi(v);
        else if (!strcmp(k, "total")) r.total = _strtoi64(v, NULL, 10);
        else if (!strcmp(k, "done")) r.done = _strtoi64(v, NULL, 10);
        else if (!strcmp(k, "added")) r.added = _strtoi64(v, NULL, 10);
        else if (!strcmp(k, "state")) r.state = !strcmp(v, "done") ? TS_DONE : !strcmp(v, "error") ? TS_ERROR : TS_PAUSED;
        if (s) set_str(s, v);
    }
    if (open) record_finish(&r);      /* tolerate a missing final "end" */
    record_clear(&r);
    free(data);
}

static void list_changed(void)
{
    if (g_hwnd && !g_exiting) SetTimer(g_hwnd, TIMER_SAVE, 600, NULL);
}

/* ------------------------------------------------------------------ */
/* files that belong to a download                                     */
/* ------------------------------------------------------------------ */
static int shell_delete(const char *path, int recycle)
{
    wchar_t *w = u8_to_w(path);
    size_t n = wcslen(w);
    wchar_t *dbl = xcalloc(n + 2, sizeof(wchar_t));    /* double-NUL terminated */
    memcpy(dbl, w, n * sizeof(wchar_t));
    SHFILEOPSTRUCTW op;
    memset(&op, 0, sizeof op);
    op.hwnd = g_hwnd;
    op.wFunc = FO_DELETE;
    op.pFrom = dbl;
    op.fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI | (recycle ? FOF_ALLOWUNDO : 0);
    int r = SHFileOperationW(&op);
    free(dbl);
    free(w);
    return (r == 0 && !op.fAnyOperationsAborted) ? 0 : -1;
}

static void delete_partials(const char *path)
{
    const char *suffix[] = { ".ulpart", ".ulstate", ".ulstate.tmp" };
    for (size_t i = 0; i < sizeof suffix / sizeof *suffix; i++) {
        char *p = xasprintf("%s%s", path, suffix[i]);
        file_delete(p);
        free(p);
    }
    char *base = path_strip_ext(path);
    char *dirs[2] = { xasprintf("%s.ulhls", path), xasprintf("%s.ulhls", base) };
    for (int i = 0; i < 2; i++) {
        if (dir_exists(dirs[i])) shell_delete(dirs[i], 0);
        free(dirs[i]);
    }
    free(base);
}

/* Only cookie files the browser bridge wrote (%TEMP%\ulc*.tmp) are deleted. */
static int is_bridge_cookie_file(const char *p)
{
    wchar_t tdir[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, tdir);
    if (!n || n > MAX_PATH) return 0;
    char *td = w_to_u8(tdir);
    size_t tl = strlen(td);
    while (tl && (td[tl - 1] == '\\' || td[tl - 1] == '/')) td[--tl] = 0;
    char *dir = path_dirname(p);
    size_t dl = strlen(dir);              /* "C:\" (TEMP at a drive root) compares as "C:" too */
    while (dl && (dir[dl - 1] == '\\' || dir[dl - 1] == '/')) dir[--dl] = 0;
    const char *b = path_basename(p);
    int ok = _stricmp(dir, td) == 0 && _strnicmp(b, "ulc", 3) == 0 && str_iends(b, ".tmp");
    free(dir);
    free(td);
    return ok;
}

static void drop_cookie_file(item *it)
{
    if (!it->cookie_file) return;
    if (is_bridge_cookie_file(it->cookie_file)) file_delete(it->cookie_file);
    free(it->cookie_file);
    it->cookie_file = NULL;
}

/* ------------------------------------------------------------------ */
/* running downloads                                                   */
/* ------------------------------------------------------------------ */
static ul_config *build_cfg(const item *it)
{
    ul_config *c = default_cfg();
    if (is_abs_path(it->dir)) set_str(&c->out_dir, it->dir);
    if (it->out_name) c->out_name = xstrdup(it->out_name);
    if (it->referer) c->referer = xstrdup(it->referer);
    if (it->ua) c->user_agent = xstrdup(it->ua);
    if (it->cookie_file && file_exists(it->cookie_file)) {
        c->cookie_file = xstrdup(it->cookie_file);
        cookies_load(c, it->cookie_file);
    }
    c->force_direct = it->mode == MODE_DIRECT;
    c->force_ytdlp = it->mode == MODE_YTDLP;
    c->connections = clampi(it->conns > 0 ? it->conns : g_set.conns, 1, UL_MAX_CONN);
    c->max_height = it->quality > 0 ? it->quality : 0;
    return c;
}

static unsigned __stdcall run_thread(void *arg)
{
    item *it = arg;
    ul_config *cfg = it->runcfg;
    int rc = task_run(it->t, cfg);
    cfg_free(cfg);
    InterlockedExchange(&it->rc, rc);
    PostMessageW(g_hwnd, WM_APP_TASKDONE, (WPARAM)rc, (LPARAM)it);
    return 0;
}

static void start_item(item *it)
{
    if (it->thread) return;
    ul_task *t = it->t;
    trace("start %s (state %d)", it->name, it->state);
    item_log(it, LOG_DIM, "%s %s", (it->path || it->done > 0) ? "Resuming" : "Starting", t->url);
    it->runcfg = build_cfg(it);
    t->abort = 0;
    t->state = TS_RUNNING;
    it->state = TS_RUNNING;
    it->resume_after = 0;
    it->rc = 1;
    it->thread = (HANDLE)_beginthreadex(NULL, 0, run_thread, it, 0, NULL);
    if (!it->thread) {
        cfg_free(it->runcfg);
        it->runcfg = NULL;
        it->state = t->state = TS_ERROR;
        EnterCriticalSection(&t->lock);
        snprintf(t->status, sizeof t->status, "cannot start a download thread");
        LeaveCriticalSection(&t->lock);
        item_log(it, LOG_ERR, "cannot start a download thread");
    }
    snap_update(it);
    redraw_item(it);
}

static void schedule(void)
{
    if (g_exiting) return;
    int active = 0;
    for (int i = 0; i < g_n; i++) if (g_items[i]->thread) active++;
    for (int i = 0; i < g_n && active < g_set.max_active; i++) {
        item *it = g_items[i];
        if (it->state == TS_QUEUED && !it->thread) {
            start_item(it);
            if (it->thread) active++;
        }
    }
}

static void tray_balloon(const wchar_t *title, const char *text, DWORD flags);
static void modal_begin(void);
static void modal_end(void);

static void finalize_removal(item *it)
{
    char *path = NULL;
    EnterCriticalSection(&it->t->lock);
    if (it->t->path) path = xstrdup(it->t->path);
    LeaveCriticalSection(&it->t->lock);
    int complete = it->state == TS_DONE;
    /* plain Del only takes the row off the list: a paused download's
       partial data stays (it can be added again and resumes). Shift+Del
       ("delete the file too") removes it. */
    if (path && it->removing == 2) {
        if (!complete) delete_partials(path);
        else if (file_exists(path)) shell_delete(path, 1);   /* to the Recycle Bin */
    }
    free(path);
    drop_cookie_file(it);
    EnterCriticalSection(&it->loglock);
    for (int i = 0; i < LOG_LINES; i++) { free(it->log[i].text); it->log[i].text = NULL; }
    it->log_count = it->log_head = 0;
    it->removing = 3;
    LeaveCriticalSection(&it->loglock);
}

/* A download thread ended (WM_APP_TASKDONE, or noticed by the timer). */
static void task_finished(item *it)
{
    if (!it || !it->thread) return;
    /* Never wait on the UI thread. A WM_APP_TASKDONE from an earlier run
       (reaped by the timer, item restarted since) finds the new thread
       still running and is ignored; a thread that posted but has not quite
       returned yet is reaped by the next timer tick. */
    if (WaitForSingleObject(it->thread, 0) != WAIT_OBJECT_0) return;
    CloseHandle(it->thread);
    it->thread = NULL;
    int rc = (int)it->rc;
    trace("finished %s rc=%d removing=%d resume_after=%d", it->name, rc, it->removing, it->resume_after);
    it->state = rc == 0 ? TS_DONE : rc == 2 ? TS_PAUSED : TS_ERROR;
    snap_update(it);
    if (it->removing) {
        finalize_removal(it);
        schedule();
        return;
    }
    if (rc == 0) {
        drop_cookie_file(it);
        if (!g_exiting) {
            tray_balloon(L"Download complete", it->name, NIIF_USER | NIIF_LARGE_ICON);
            g_notify_item = it;
        }
    } else if (rc == 1 && !g_exiting && !it->resume_after) {
        char msg[800];
        snprintf(msg, sizeof msg, "%s\n%s", it->name, it->status[0] ? it->status : "unknown error");
        tray_balloon(L"Download failed", msg, NIIF_ERROR);
        g_notify_item = it;
    }
    if (it->resume_after) {
        it->resume_after = 0;
        if (rc != 0) it->state = TS_QUEUED;
    }
    redraw_item(it);
    list_changed();
    schedule();
}

static void pause_item(item *it)
{
    trace("pause %s (state %d, thread %d)", it->name, it->state, it->thread != NULL);
    if (it->thread) {
        it->t->abort = 1;
        it->resume_after = 0;
    } else if (it->state == TS_QUEUED) {
        it->state = TS_PAUSED;
    }
    snap_update(it);
    redraw_item(it);
}

static void resume_item(item *it)
{
    trace("resume %s (state %d, thread %d)", it->name, it->state, it->thread != NULL);
    if (it->thread) {
        if (it->t->abort) it->resume_after = 1;
    } else if (it->state == TS_PAUSED || it->state == TS_ERROR) {
        it->state = TS_QUEUED;
    }
    redraw_item(it);
}

/* ------------------------------------------------------------------ */
/* selection                                                           */
/* ------------------------------------------------------------------ */
static int selection(item ***out)
{
    int cnt = (int)ListView_GetSelectedCount(g_lv), n = 0;
    item **v = xcalloc((size_t)cnt + 1, sizeof *v);
    for (int i = ListView_GetNextItem(g_lv, -1, LVNI_SELECTED); i >= 0 && n < cnt;
         i = ListView_GetNextItem(g_lv, i, LVNI_SELECTED))
        if (i < g_n) v[n++] = g_items[i];
    *out = v;
    return n;
}

static item *focused_item(void)
{
    int i = ListView_GetNextItem(g_lv, -1, LVNI_FOCUSED);
    if (i >= 0 && i < g_n && (ListView_GetItemState(g_lv, i, LVIS_SELECTED) & LVIS_SELECTED)) return g_items[i];
    i = ListView_GetNextItem(g_lv, -1, LVNI_SELECTED);
    return (i >= 0 && i < g_n) ? g_items[i] : NULL;
}

static void select_items(item **v, int n, item *focus)
{
    ListView_SetItemState(g_lv, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    for (int k = 0; k < n; k++) {
        int i = index_of(v[k]);
        if (i >= 0) ListView_SetItemState(g_lv, i, LVIS_SELECTED, LVIS_SELECTED);
    }
    int f = focus ? index_of(focus) : -1;
    if (f < 0 && n > 0) f = index_of(v[0]);
    if (f >= 0) {
        ListView_SetItemState(g_lv, f, LVIS_FOCUSED, LVIS_FOCUSED);
        ListView_SetSelectionMark(g_lv, f);
        ListView_EnsureVisible(g_lv, f, FALSE);
    }
}

static void add_items(item **v, int n, int select)
{
    for (int i = 0; i < n; i++) list_append(v[i]);
    ListView_SetItemCountEx(g_lv, g_n, LVSICF_NOSCROLL);
    if (select && n > 0) select_items(v, n, v[0]);
    list_changed();
    update_status();
    PostMessageW(g_hwnd, WM_APP_SELCHANGE, 0, 0);
}

static void grave_add(item *it)
{
    if (g_ngone == g_capgone) {
        g_capgone = g_capgone ? g_capgone * 2 : 16;
        g_gone = xrealloc(g_gone, sizeof *g_gone * (size_t)g_capgone);
    }
    g_gone[g_ngone++] = it;
}

static void remove_items(item **v, int n, int with_files)
{
    int first = -1;
    item **now = xcalloc((size_t)n + 1, sizeof *now);   /* finalized after the list is consistent */
    int nnow = 0;
    for (int k = 0; k < n; k++) {
        item *it = v[k];
        int idx = index_of(it);
        if (idx < 0) continue;
        if (first < 0 || idx < first) first = idx;
        memmove(&g_items[idx], &g_items[idx + 1], sizeof *g_items * (size_t)(g_n - idx - 1));
        g_n--;
        EnterCriticalSection(&it->loglock);   /* gui_on_log reads it on download threads */
        it->removing = with_files ? 2 : 1;
        LeaveCriticalSection(&it->loglock);
        grave_add(it);
        if (it == g_notify_item) g_notify_item = NULL;
        if (it->thread) it->t->abort = 1;      /* finished when the thread ends */
        else now[nnow++] = it;
    }
    ListView_SetItemCountEx(g_lv, g_n, 0);
    /* Deleting files (SHFileOperation) runs a message loop: the list view
       may repaint meanwhile, so it must already know its new length. */
    for (int k = 0; k < nnow; k++) finalize_removal(now[k]);
    free(now);
    ListView_SetItemState(g_lv, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    if (g_n > 0 && first >= 0) {
        item *next = g_items[first < g_n ? first : g_n - 1];
        select_items(&next, 1, next);
    }
    InvalidateRect(g_lv, NULL, FALSE);
    list_changed();
    detail_refresh(1);
    update_toolbar();
    update_status();
    schedule();
}

/* ------------------------------------------------------------------ */
/* clipboard                                                           */
/* ------------------------------------------------------------------ */
static char *clipboard_text(void)
{
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return NULL;
    int opened = 0;
    for (int i = 0; i < 5 && !(opened = OpenClipboard(g_hwnd)); i++) Sleep(10);
    if (!opened) return NULL;
    char *r = NULL;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t *w = GlobalLock(h);
        if (w) {
            size_t max = GlobalSize(h) / sizeof(wchar_t), n = 0;
            while (n < max && n < 262144 && w[n]) n++;
            wchar_t *c = xmalloc((n + 1) * sizeof(wchar_t));
            memcpy(c, w, n * sizeof(wchar_t));
            c[n] = 0;
            GlobalUnlock(h);
            r = w_to_u8(c);
            free(c);
        }
    }
    CloseClipboard();
    return r;
}

static void clipboard_set(const char *u8)
{
    wchar_t *w = u8_to_w(u8);
    size_t n = (wcslen(w) + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n);
    if (h) {
        void *p = GlobalLock(h);
        if (p) { memcpy(p, w, n); GlobalUnlock(h); }
        if (p && OpenClipboard(g_hwnd)) {
            EmptyClipboard();
            if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
            CloseClipboard();
            g_own_clip_seq = GetClipboardSequenceNumber();
        } else {
            GlobalFree(h);
        }
    }
    free(w);
}

static int link_seen(const char *link, int remember)
{
    for (int i = 0; i < 64; i++) if (g_seen[i] && !strcmp(g_seen[i], link)) return 1;
    for (int i = 0; i < g_n; i++) if (!strcmp(g_items[i]->t->url, link)) return 1;
    if (remember) {
        free(g_seen[g_seen_pos]);
        g_seen[g_seen_pos] = xstrdup(link);
        g_seen_pos = (g_seen_pos + 1) % 64;
    }
    return 0;
}

static int looks_like_download(const char *u)
{
    static const char *exts[] = { "zip", "rar", "7z", "exe", "msi", "iso", "mp4", "mkv", "avi", "mov", "webm",
                                  "mp3", "m4a", "flac", "pdf", "apk", "torrent", "dmg", "gz", "tar" };
    if (str_istarts(u, "magnet:?")) return 1;
    if (!str_istarts(u, "http://") && !str_istarts(u, "https://") && !str_istarts(u, "ftp://")) return 0;
    const char *host = strstr(u, "://") + 3;
    size_t end = strcspn(host, "?#");
    const char *slash = NULL;
    for (size_t i = 0; i < end; i++) if (host[i] == '/') slash = host + i;
    if (!slash) return 0;
    const char *seg = slash + 1, *stop = host + end, *dot = NULL;
    for (const char *p = seg; p < stop; p++) if (*p == '.') dot = p;
    if (!dot) return 0;
    size_t el = (size_t)(stop - dot - 1);
    for (size_t i = 0; i < sizeof exts / sizeof *exts; i++)
        if (el == strlen(exts[i]) && _strnicmp(dot + 1, exts[i], el) == 0) return 1;
    return 0;
}

static void on_clipboard_update(void)
{
    if (!g_set.clip || g_exiting) return;
    if (GetClipboardSequenceNumber() == g_own_clip_seq || GetClipboardOwner() == g_hwnd) return;
    char *s = clipboard_text();
    if (!s) return;
    char *t = str_trim(s);
    /* only looked up here: it counts as seen once it was really offered
       (a link copied while a dialog is open must come back later) */
    if (*t && strlen(t) < 8192 && !strpbrk(t, " \t\r\n") && looks_like_download(t) && !link_seen(t, 0)) {
        char *copy = xstrdup(t);
        if (!PostMessageW(g_hwnd, WM_APP_CLIPLINK, 0, (LPARAM)copy)) free(copy);
    }
    free(s);
}

static void clip_listen(int on)
{
    if (on && !g_clip_listening) g_clip_listening = AddClipboardFormatListener(g_hwnd) != 0;
    else if (!on && g_clip_listening) { RemoveClipboardFormatListener(g_hwnd); g_clip_listening = 0; }
}

/* ------------------------------------------------------------------ */
/* notification area                                                   */
/* ------------------------------------------------------------------ */
static void tray_base(NOTIFYICONDATAW *nid)
{
    memset(nid, 0, sizeof *nid);
    nid->cbSize = sizeof *nid;
    nid->hWnd = g_hwnd;
    nid->uID = 1;
}

static void tray_add(void)
{
    NOTIFYICONDATAW nid;
    tray_base(&nid);
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = g_icon_tray;
    lstrcpynW(nid.szTip, L"U-loader", 128);
    if (Shell_NotifyIconW(NIM_ADD, &nid)) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
        g_tray_added = 1;
    }
}

static void tray_remove(void)
{
    if (!g_tray_added) return;
    NOTIFYICONDATAW nid;
    tray_base(&nid);
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_tray_added = 0;
}

static void tray_tip(const wchar_t *tip)
{
    if (!g_tray_added) return;
    NOTIFYICONDATAW nid;
    tray_base(&nid);
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    lstrcpynW(nid.szTip, tip, 128);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void tray_balloon(const wchar_t *title, const char *text, DWORD flags)
{
    if (!g_tray_added) return;
    NOTIFYICONDATAW nid;
    tray_base(&nid);
    nid.uFlags = NIF_INFO;
    lstrcpynW(nid.szInfoTitle, title, 64);
    u8w(text, nid.szInfo, 256);
    nid.dwInfoFlags = flags;
    if ((flags & NIIF_ICON_MASK) == NIIF_USER) nid.hBalloonIcon = g_icon_balloon;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void show_main(void)
{
    if (g_exiting) return;
    if (!IsWindowVisible(g_hwnd)) ShowWindow(g_hwnd, IsIconic(g_hwnd) ? SW_RESTORE : SW_SHOW);
    else if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
    HWND top = g_dlg ? g_dlg : g_hwnd;
    SetForegroundWindow(top);
    BringWindowToTop(top);
}

static void hide_to_tray(void)
{
    if (!g_tray_added) tray_add();
    if (!g_tray_added) { ShowWindow(g_hwnd, SW_MINIMIZE); return; }
    ShowWindow(g_hwnd, SW_HIDE);
    if (!g_set.tray_hint) {
        g_set.tray_hint = 1;
        tray_balloon(L"U-loader is still running",
                     "Downloads continue in the background. Click the icon to open the window.", NIIF_INFO);
        settings_save();
    }
}

/* ------------------------------------------------------------------ */
/* DPI, fonts, images                                                  */
/* ------------------------------------------------------------------ */
static void load_dpi_api(void)
{
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (!u) return;
    pGetDpiForWindow = (UINT (WINAPI *)(HWND))(void (*)(void))GetProcAddress(u, "GetDpiForWindow");
    pGetDpiForSystem = (UINT (WINAPI *)(void))(void (*)(void))GetProcAddress(u, "GetDpiForSystem");
    pSystemParametersInfoForDpi = (BOOL (WINAPI *)(UINT, UINT, PVOID, UINT, UINT))(void (*)(void))
        GetProcAddress(u, "SystemParametersInfoForDpi");
    pGetSystemMetricsForDpi = (int (WINAPI *)(int, UINT))(void (*)(void))GetProcAddress(u, "GetSystemMetricsForDpi");
    pSetProcessDpiAwarenessContext = (BOOL (WINAPI *)(HANDLE))(void (*)(void))
        GetProcAddress(u, "SetProcessDpiAwarenessContext");
    /* the manifest normally does this already; harmless if it did */
    if (pSetProcessDpiAwarenessContext) pSetProcessDpiAwarenessContext((HANDLE)(INT_PTR)-4);
}

static UINT system_dpi(void)
{
    if (pGetDpiForSystem) return pGetDpiForSystem();
    HDC dc = GetDC(NULL);
    UINT d = (UINT)GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    return d ? d : 96;
}

static UINT window_dpi(HWND h)
{
    if (g_dpi_override) return g_dpi_override;
    if (pGetDpiForWindow && h) { UINT d = pGetDpiForWindow(h); if (d) return d; }
    return system_dpi();
}

static int metric(int idx)
{
    if (pGetSystemMetricsForDpi) return pGetSystemMetricsForDpi(idx, g_dpi);
    return MulDiv(GetSystemMetrics(idx), (int)g_dpi, (int)system_dpi());
}

static void message_font(LOGFONTW *lf)
{
    NONCLIENTMETRICSW ncm;
    memset(&ncm, 0, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    if (!g_dpi_override && pSystemParametersInfoForDpi &&
        pSystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, g_dpi)) {
        *lf = ncm.lfMessageFont;
        return;
    }
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0)) {
        *lf = ncm.lfMessageFont;
        lf->lfHeight = MulDiv(lf->lfHeight, (int)g_dpi, (int)system_dpi());
    } else {
        memset(lf, 0, sizeof *lf);
        lf->lfHeight = -MulDiv(9, (int)g_dpi, 72);
        lf->lfWeight = FW_NORMAL;
        lstrcpynW(lf->lfFaceName, L"Segoe UI", LF_FACESIZE);
    }
}

static int font_height(HFONT f)
{
    HDC dc = GetDC(NULL);
    HGDIOBJ o = SelectObject(dc, f);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, o);
    ReleaseDC(NULL, dc);
    return tm.tmHeight;
}

/* returns the old fonts packed in *old (to delete after the controls switched) */
static void make_fonts(HFONT old[4])
{
    old[0] = g_font; old[1] = g_font_bold; old[2] = g_font_title; old[3] = g_font_mono;
    LOGFONTW lf;
    message_font(&lf);
    if (lf.lfHeight > 0) lf.lfHeight = -lf.lfHeight;
    g_font = CreateFontIndirectW(&lf);
    LOGFONTW b = lf;
    b.lfWeight = FW_SEMIBOLD;
    g_font_bold = CreateFontIndirectW(&b);
    LOGFONTW t = lf;
    t.lfHeight = MulDiv(lf.lfHeight, 4, 3);
    t.lfWeight = FW_SEMIBOLD;
    g_font_title = CreateFontIndirectW(&t);
    LOGFONTW m;
    memset(&m, 0, sizeof m);
    m.lfHeight = lf.lfHeight;
    m.lfWeight = FW_NORMAL;
    m.lfCharSet = DEFAULT_CHARSET;
    m.lfQuality = lf.lfQuality;
    m.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    lstrcpynW(m.lfFaceName, L"Consolas", LF_FACESIZE);
    g_font_mono = CreateFontIndirectW(&m);
    g_line_h = font_height(g_font);
    g_title_h = font_height(g_font_title);
    g_mono_h = font_height(g_font_mono);
}

static void delete_fonts(HFONT old[4])
{
    for (int i = 0; i < 4; i++) if (old[i]) DeleteObject(old[i]);
}

static int CALLBACK font_exists_cb(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM lp)
{
    *(int *)lp = 1;
    return 0;
}

static int font_exists(const wchar_t *face)
{
    LOGFONTW lf;
    memset(&lf, 0, sizeof lf);
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(lf.lfFaceName, face, LF_FACESIZE);
    int found = 0;
    HDC dc = GetDC(NULL);
    EnumFontFamiliesExW(dc, &lf, font_exists_cb, (LPARAM)&found, 0);
    ReleaseDC(NULL, dc);
    return found;
}

/* 32-bit straight-alpha DIB of w x h with its pixel pointer */
static HBITMAP make_dib(int w, int h, DWORD **bits)
{
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *p = NULL;
    HBITMAP b = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &p, NULL, 0);
    *bits = p;
    return b;
}

static HICON icon_from_dib(HBITMAP color, int w, int h)
{
    /* an all-zero mask (the alpha channel does the work); CreateBitmap
       with NULL bits leaves the memory undefined. Rows are WORD aligned. */
    size_t stride = (size_t)((w + 15) / 16) * 2;
    void *bits = xcalloc(stride * (size_t)(h > 0 ? h : 1), 1);
    HBITMAP mask = CreateBitmap(w, h, 1, 1, bits);
    free(bits);
    ICONINFO ii;
    memset(&ii, 0, sizeof ii);
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(mask);
    return ic;
}

/* An icon-font glyph (Segoe MDL2 Assets) as a tinted, anti-aliased icon. */
static HICON glyph_icon(HFONT font, wchar_t ch, int size, COLORREF col)
{
    DWORD *px;
    HBITMAP bmp = make_dib(size, size, &px);
    if (!bmp) return NULL;
    HDC dc = CreateCompatibleDC(NULL);
    HGDIOBJ ob = SelectObject(dc, bmp), of = SelectObject(dc, font);
    memset(px, 0, (size_t)size * (size_t)size * 4);
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    RECT r = { 0, 0, size, size };
    DrawTextW(dc, &ch, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
    GdiFlush();
    for (int i = 0; i < size * size; i++) {
        DWORD v = px[i];
        unsigned a = (v & 0xFF) > ((v >> 8) & 0xFF) ? (v & 0xFF) : ((v >> 8) & 0xFF);
        if (((v >> 16) & 0xFF) > a) a = (v >> 16) & 0xFF;
        px[i] = (a << 24) | ((DWORD)GetRValue(col) << 16) | ((DWORD)GetGValue(col) << 8) | GetBValue(col);
    }
    SelectObject(dc, of);
    SelectObject(dc, ob);
    DeleteDC(dc);
    HICON ic = icon_from_dib(bmp, size, size);
    DeleteObject(bmp);
    return ic;
}

/* A shell icon drawn centered into a w x h cell (taller than the icon,
   which makes the list rows roomier), alpha recovered from two renders. */
static HICON cell_icon(HICON src, int isz, int w, int h)
{
    DWORD *pb, *pw;
    HBITMAP bb = make_dib(w, h, &pb), bw = make_dib(w, h, &pw);
    if (!bb || !bw) { if (bb) DeleteObject(bb); if (bw) DeleteObject(bw); return NULL; }
    HDC dc = CreateCompatibleDC(NULL);
    int x = (w - isz) / 2, y = (h - isz) / 2;
    HGDIOBJ o = SelectObject(dc, bb);
    memset(pb, 0, (size_t)w * (size_t)h * 4);
    DrawIconEx(dc, x, y, src, isz, isz, 0, NULL, DI_NORMAL);
    SelectObject(dc, bw);
    memset(pw, 0xFF, (size_t)w * (size_t)h * 4);
    DrawIconEx(dc, x, y, src, isz, isz, 0, NULL, DI_NORMAL);
    GdiFlush();
    SelectObject(dc, o);
    DeleteDC(dc);
    for (int i = 0; i < w * h; i++) {
        int b0 = pb[i] & 0xFF, g0 = (pb[i] >> 8) & 0xFF, r0 = (pb[i] >> 16) & 0xFF;
        int g1 = (pw[i] >> 8) & 0xFF;
        int a = 255 - (g1 - g0);
        if (a < 0) a = 0;
        if (a > 255) a = 255;
        if (a == 0) { pb[i] = 0; continue; }
        int r = r0 * 255 / a, g = g0 * 255 / a, b = b0 * 255 / a;
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        pb[i] = ((DWORD)a << 24) | ((DWORD)r << 16) | ((DWORD)g << 8) | (DWORD)b;
    }
    HICON ic = icon_from_dib(bb, w, h);
    DeleteObject(bb);
    DeleteObject(bw);
    return ic;
}

static int row_height(void)
{
    int icon = metric(SM_CXSMICON);
    int a = icon + S(9), b = g_line_h + S(11);
    return a > b ? a : b;
}

static struct { char ext[16]; int index; } g_icache[96];
static int g_nicache;

static void make_file_imagelist(void)
{
    HIMAGELIST old = g_il_files;
    int isz = metric(SM_CXSMICON);
    g_il_files = ImageList_Create(isz + S(4), row_height(), ILC_COLOR32, 16, 16);
    g_nicache = 0;
    if (g_lv) ListView_SetImageList(g_lv, g_il_files, LVSIL_SMALL);
    if (old) ImageList_Destroy(old);
    for (int i = 0; i < g_n; i++) g_items[i]->icon = -1;
}

static int icon_for(item *it)
{
    if (it->icon >= 0) return it->icon;
    const char *ext = path_ext(it->name);
    if (!*ext || strlen(ext) >= sizeof it->icon_ext) ext = ".bin";
    snprintf(it->icon_ext, sizeof it->icon_ext, "%s", path_ext(it->name));
    for (int i = 0; i < g_nicache; i++)
        if (!_stricmp(g_icache[i].ext, ext)) return it->icon = g_icache[i].index;
    int isz = metric(SM_CXSMICON), cw = isz, chh = isz;
    if (!ImageList_GetIconSize(g_il_files, &cw, &chh)) cw = chh = isz;
    wchar_t name[32] = L"file";
    u8w(ext, name + 4, 28);
    SHFILEINFOW sfi;
    memset(&sfi, 0, sizeof sfi);
    UINT flags = SHGFI_ICON | SHGFI_USEFILEATTRIBUTES |
                 (isz > GetSystemMetrics(SM_CXSMICON) ? SHGFI_LARGEICON : SHGFI_SMALLICON);
    int index = -1;
    if (SHGetFileInfoW(name, FILE_ATTRIBUTE_NORMAL, &sfi, sizeof sfi, flags) && sfi.hIcon) {
        HICON ic = cell_icon(sfi.hIcon, isz, cw, chh);
        if (ic) {
            index = ImageList_AddIcon(g_il_files, ic);
            DestroyIcon(ic);
        }
        DestroyIcon(sfi.hIcon);
    }
    if (g_nicache < (int)(sizeof g_icache / sizeof *g_icache)) {
        snprintf(g_icache[g_nicache].ext, sizeof g_icache[0].ext, "%s", ext);
        g_icache[g_nicache++].index = index;
    }
    return it->icon = index;
}

static void set_window_icons(void)
{
    int big = metric(SM_CXICON), small = metric(SM_CXSMICON);
    HICON b = LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, big, big, 0);
    HICON s = LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, small, small, 0);
    if (b) SendMessageW(g_hwnd, WM_SETICON, ICON_BIG, (LPARAM)b);
    if (s) SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)s);
    /* an open dialog shows the old icons too: hand it the new ones before
       the old ones are destroyed */
    if (g_dlg && b) SendMessageW(g_dlg, WM_SETICON, ICON_BIG, (LPARAM)b);
    if (g_dlg && s) SendMessageW(g_dlg, WM_SETICON, ICON_SMALL, (LPARAM)s);
    if (b && g_icon_big) DestroyIcon(g_icon_big);
    if (s && g_icon_small) DestroyIcon(g_icon_small);
    if (b) g_icon_big = b;
    if (s) g_icon_small = s;
}

/* ------------------------------------------------------------------ */
/* toolbar                                                             */
/* ------------------------------------------------------------------ */
static const struct {
    int cmd;
    wchar_t glyph;
    const wchar_t *text, *tip;
    COLORREF color;
} TOOLS[] = {
    { IDM_ADD,        0xE710, L"Add link",    L"Add a download (Ctrl+N)",               RGB(0x00, 0x67, 0xC0) },
    { 0, 0, NULL, NULL, 0 },
    { IDM_RESUME,     0xE768, L"Resume",      L"Resume the selected downloads",         RGB(0x10, 0x7C, 0x10) },
    { IDM_PAUSE,      0xE769, L"Pause",       L"Pause the selected downloads",          RGB(0xB8, 0x6E, 0x00) },
    { IDM_PAUSEALL,   0xE71A, L"Pause all",   L"Pause every download",                  RGB(0xB8, 0x6E, 0x00) },
    { 0, 0, NULL, NULL, 0 },
    { IDM_DELETE,     0xE74D, L"Delete",      L"Remove from the list (Del); Shift+Del also deletes the file",
                                                                                         RGB(0xC4, 0x2B, 0x1C) },
    { 0, 0, NULL, NULL, 0 },
    { IDM_OPEN,       0xE8E5, L"Open",        L"Open the downloaded file (Enter)",      RGB(0x00, 0x67, 0xC0) },
    { IDM_OPENFOLDER, 0xE838, L"Open folder", L"Show the file in its folder",           RGB(0xB0, 0x80, 0x00) },
    { 0, 0, NULL, NULL, 0 },
    { IDM_SETTINGS,   0xE713, L"Settings",    L"Settings, browser integration and yt-dlp", RGB(0x55, 0x55, 0x55) },
};
#define NTOOLS ((int)(sizeof TOOLS / sizeof *TOOLS))

static void create_toolbar(void)
{
    if (g_tb) DestroyWindow(g_tb);
    if (g_il_tb) ImageList_Destroy(g_il_tb);
    if (g_il_tb_dis) ImageList_Destroy(g_il_tb_dis);
    g_il_tb = g_il_tb_dis = NULL;
    memset(g_tb_state, 0xFF, sizeof g_tb_state);

    g_tb = CreateWindowExW(0, TOOLBARCLASSNAMEW, NULL,
                           WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TBSTYLE_FLAT | TBSTYLE_LIST |
                           TBSTYLE_TOOLTIPS | TBSTYLE_TRANSPARENT | CCS_NODIVIDER | CCS_NORESIZE |
                           CCS_NOPARENTALIGN,
                           0, 0, 0, 0, g_hwnd, (HMENU)(INT_PTR)ID_TOOLBAR, g_inst, NULL);
    SendMessageW(g_tb, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    SendMessageW(g_tb, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_MIXEDBUTTONS | TBSTYLE_EX_DOUBLEBUFFER);
    SendMessageW(g_tb, WM_SETFONT, (WPARAM)g_font, FALSE);

    if (g_have_mdl2) {
        int sz = S(16);
        HFONT gf = CreateFontW(-sz, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe MDL2 Assets");
        g_il_tb = ImageList_Create(sz, sz, ILC_COLOR32, NTOOLS, 0);
        g_il_tb_dis = ImageList_Create(sz, sz, ILC_COLOR32, NTOOLS, 0);
        for (int i = 0; i < NTOOLS; i++) {
            if (!TOOLS[i].cmd) continue;
            HICON a = glyph_icon(gf, TOOLS[i].glyph, sz, TOOLS[i].color);
            HICON d = glyph_icon(gf, TOOLS[i].glyph, sz, RGB(0xB8, 0xB8, 0xB8));
            ImageList_AddIcon(g_il_tb, a);
            ImageList_AddIcon(g_il_tb_dis, d);
            DestroyIcon(a);
            DestroyIcon(d);
        }
        DeleteObject(gf);
        SendMessageW(g_tb, TB_SETIMAGELIST, 0, (LPARAM)g_il_tb);
        SendMessageW(g_tb, TB_SETDISABLEDIMAGELIST, 0, (LPARAM)g_il_tb_dis);
    } else {
        SendMessageW(g_tb, TB_SETBITMAPSIZE, 0, MAKELPARAM(0, 0));
    }
    SendMessageW(g_tb, TB_SETPADDING, 0, MAKELPARAM(S(14), S(12)));
    SendMessageW(g_tb, TB_SETLISTGAP, (WPARAM)S(6), 0);

    TBBUTTON bt[NTOOLS];
    memset(bt, 0, sizeof bt);
    int img = 0;
    for (int i = 0; i < NTOOLS; i++) {
        if (!TOOLS[i].cmd) {
            bt[i].fsStyle = BTNS_SEP;
            bt[i].iBitmap = S(8);
            continue;
        }
        bt[i].iBitmap = g_have_mdl2 ? img++ : I_IMAGENONE;
        bt[i].idCommand = TOOLS[i].cmd;
        bt[i].fsState = TBSTATE_ENABLED;
        bt[i].fsStyle = BTNS_BUTTON | BTNS_AUTOSIZE | BTNS_SHOWTEXT;
        bt[i].iString = (INT_PTR)TOOLS[i].text;
    }
    SendMessageW(g_tb, TB_ADDBUTTONSW, NTOOLS, (LPARAM)bt);
    SendMessageW(g_tb, TB_AUTOSIZE, 0, 0);
}

static void set_tool(int cmd, int on)
{
    int slot = -1;
    for (int i = 0, k = 0; i < NTOOLS; i++) {
        if (!TOOLS[i].cmd) continue;
        if (TOOLS[i].cmd == cmd) { slot = k; break; }
        k++;
    }
    if (slot < 0 || slot >= 16 || g_tb_state[slot] == on) return;
    g_tb_state[slot] = on;
    SendMessageW(g_tb, TB_ENABLEBUTTON, (WPARAM)cmd, MAKELPARAM(on ? TRUE : FALSE, 0));
}

static void update_toolbar(void)
{
    item **sel;
    int n = selection(&sel);
    int can_resume = 0, can_pause = 0, can_open = 0, any = 0;
    for (int k = 0; k < n; k++) {
        item *it = sel[k];
        if (!it->thread && (it->state == TS_PAUSED || it->state == TS_ERROR)) can_resume = 1;
        if (it->thread && it->t->abort) can_resume = 1;
        if ((it->thread && !it->t->abort) || it->state == TS_QUEUED) can_pause = 1;
        if (it->state == TS_DONE) can_open = 1;
    }
    for (int i = 0; i < g_n && !any; i++)
        if ((g_items[i]->thread && !g_items[i]->t->abort) || g_items[i]->state == TS_QUEUED) any = 1;
    free(sel);
    set_tool(IDM_RESUME, can_resume);
    set_tool(IDM_PAUSE, can_pause);
    set_tool(IDM_PAUSEALL, any);
    set_tool(IDM_DELETE, n > 0);
    set_tool(IDM_OPEN, can_open);
    set_tool(IDM_OPENFOLDER, n > 0);
}

/* ------------------------------------------------------------------ */
/* list view                                                           */
/* ------------------------------------------------------------------ */
static void create_list(void)
{
    g_lv = CreateWindowExW(0, WC_LISTVIEWW, L"",
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | LVS_REPORT | LVS_OWNERDATA |
                           LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS,
                           0, 0, 0, 0, g_hwnd, (HMENU)(INT_PTR)ID_LIST, g_inst, NULL);
    SetWindowTheme(g_lv, L"Explorer", NULL);
    ListView_SetExtendedListViewStyle(g_lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP |
                                            LVS_EX_LABELTIP);
    SendMessageW(g_lv, WM_SETFONT, (WPARAM)g_font, FALSE);
    for (int i = 0; i < NCOLS; i++) {
        LVCOLUMNW c;
        memset(&c, 0, sizeof c);
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT | LVCF_SUBITEM;
        c.fmt = COL_FMT[i];
        c.cx = S(g_set.colw[i]);
        c.pszText = (LPWSTR)COL_TITLES[i];
        c.iSubItem = i;
        ListView_InsertColumn(g_lv, i, &c);
    }
    ListView_SetColumnOrderArray(g_lv, NCOLS, g_set.order);
    make_file_imagelist();
}

static void list_dispinfo(NMLVDISPINFOW *di)
{
    int i = di->item.iItem;
    if (i < 0 || i >= g_n) return;
    item *it = g_items[i];
    if (di->item.mask & LVIF_IMAGE) di->item.iImage = di->item.iSubItem == 0 ? icon_for(it) : -1;
    if (!(di->item.mask & LVIF_TEXT) || !di->item.pszText || di->item.cchTextMax <= 0) return;
    char buf[700], a[32];
    buf[0] = 0;
    switch (di->item.iSubItem) {
    case COL_NAME:
        snprintf(buf, sizeof buf, "%s", it->name);
        break;
    case COL_SIZE:
        if (it->total > 0) fmt_size(it->total, buf, sizeof buf);
        else if (it->done > 0) snprintf(buf, sizeof buf, "%s", fmt_size(it->done, a, sizeof a));
        break;
    case COL_SPEED:
        if (it->thread && it->speed > 0) snprintf(buf, sizeof buf, "%s/s", fmt_size(it->speed, a, sizeof a));
        break;
    case COL_ETA:
        if (it->thread && it->eta >= 0 && !it->pausing) fmt_eta((double)it->eta, buf, sizeof buf);
        break;
    case COL_STATUS:
        status_text(it, buf, sizeof buf);
        break;
    case COL_ADDED:
        fmt_added(it->added, buf, sizeof buf);
        break;
    default:       /* COL_PROGRESS is drawn in the post-paint stage */
        break;
    }
    u8w(buf, di->item.pszText, di->item.cchTextMax);
}

static void draw_progress(HDC dc, const RECT *cell, const item *it)
{
    int h = cell->bottom - cell->top;
    int bh = g_line_h + S(3);
    if (bh > h - S(6)) bh = h - S(6);
    if (bh < S(6)) return;
    RECT bar = { cell->left + S(6), cell->top + (h - bh) / 2, cell->right - S(8), 0 };
    bar.bottom = bar.top + bh;
    if (bar.right - bar.left < S(20)) return;
    double f = item_frac(it);
    int fx = bar.left + (int)((double)(bar.right - bar.left) * f + 0.5);
    COLORREF col = state_color(it);
    fill(dc, &bar, C_TRACK);
    RECT done = bar;
    done.right = fx;
    if (fx > bar.left) fill(dc, &done, col);

    char txt[64];
    txt[0] = 0;
    if (it->total > 0 || it->state == TS_DONE) {
        if (it->state == TS_DONE) snprintf(txt, sizeof txt, "100%%");
        else snprintf(txt, sizeof txt, "%.1f%%", f * 100.0);
    } else if (it->done > 0) {
        fmt_size(it->done, txt, sizeof txt);
    }
    if (!txt[0]) return;
    wchar_t w[64];
    u8w(txt, w, 64);
    HGDIOBJ of = SelectObject(dc, g_font);
    SetBkMode(dc, TRANSPARENT);
    int saved = SaveDC(dc);
    IntersectClipRect(dc, fx, bar.top, bar.right, bar.bottom);
    SetTextColor(dc, C_TEXT2);
    RECT tr = bar;
    DrawTextW(dc, w, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RestoreDC(dc, saved);
    if (fx > bar.left) {
        saved = SaveDC(dc);
        IntersectClipRect(dc, bar.left, bar.top, fx, bar.bottom);
        SetTextColor(dc, C_WHITE);
        tr = bar;
        DrawTextW(dc, w, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        RestoreDC(dc, saved);
    }
    SelectObject(dc, of);
}

static LRESULT list_customdraw(NMLVCUSTOMDRAW *cd)
{
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT:
        return CDRF_NOTIFYSUBITEMDRAW | CDRF_NOTIFYPOSTPAINT;
    case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
        int i = (int)cd->nmcd.dwItemSpec;
        cd->clrText = cd->iSubItem == COL_NAME ? C_TEXT : C_TEXT2;
        if (cd->iSubItem == COL_STATUS && i >= 0 && i < g_n) cd->clrText = state_text_color(g_items[i]);
        if (cd->iSubItem == COL_ADDED) cd->clrText = C_DIM;
        return CDRF_NEWFONT;
    }
    case CDDS_ITEMPOSTPAINT: {
        int i = (int)cd->nmcd.dwItemSpec;
        RECT r;
        if (i >= 0 && i < g_n && ListView_GetSubItemRect(g_lv, i, COL_PROGRESS, LVIR_BOUNDS, &r))
            draw_progress(cd->nmcd.hdc, &r, g_items[i]);
        return CDRF_DODEFAULT;
    }
    default:
        return CDRF_DODEFAULT;
    }
}

static int g_cmp_col, g_cmp_desc;

static int cmp64(int64_t a, int64_t b) { return a < b ? -1 : a > b; }

static int cmp_items(const void *pa, const void *pb)
{
    const item *x = *(item *const *)pa, *y = *(item *const *)pb;
    int r = 0;
    switch (g_cmp_col) {
    case COL_NAME:     r = _stricmp(x->name, y->name); break;
    case COL_SIZE:     r = cmp64(x->total, y->total); break;
    case COL_PROGRESS: { double a = item_frac(x), b = item_frac(y); r = a < b ? -1 : a > b; break; }
    case COL_SPEED:    r = cmp64(x->thread ? x->speed : 0, y->thread ? y->speed : 0); break;
    case COL_ETA:      r = cmp64(x->eta < 0 ? INT64_MAX : x->eta, y->eta < 0 ? INT64_MAX : y->eta); break;
    case COL_STATUS:   r = x->state - y->state; break;
    case COL_ADDED:    r = cmp64(x->added, y->added); break;
    default: break;
    }
    if (!r) r = x->seq < y->seq ? -1 : x->seq > y->seq;
    return g_cmp_desc ? -r : r;
}

static void sort_by(int col)
{
    if (col < 0 || col >= NCOLS) return;
    if (g_sort_col == col) g_sort_desc = !g_sort_desc;
    else { g_sort_col = col; g_sort_desc = col == COL_SIZE || col == COL_SPEED || col == COL_ADDED; }
    item **sel;
    int n = selection(&sel);
    item *focus = focused_item();
    g_cmp_col = g_sort_col;
    g_cmp_desc = g_sort_desc;
    qsort(g_items, (size_t)g_n, sizeof *g_items, cmp_items);
    select_items(sel, n, focus);
    free(sel);
    InvalidateRect(g_lv, NULL, FALSE);
    HWND hdr = ListView_GetHeader(g_lv);
    for (int i = 0; i < NCOLS; i++) {
        HDITEMW hi;
        memset(&hi, 0, sizeof hi);
        hi.mask = HDI_FORMAT;
        Header_GetItem(hdr, i, &hi);
        hi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == g_sort_col) hi.fmt |= g_sort_desc ? HDF_SORTDOWN : HDF_SORTUP;
        Header_SetItem(hdr, i, &hi);
    }
    list_changed();
}

/* ------------------------------------------------------------------ */
/* actions                                                             */
/* ------------------------------------------------------------------ */
static void open_item(item *it)
{
    if (!it || it->state != TS_DONE) return;
    if (!it->path || !file_exists(it->path)) {
        modal_begin();                 /* no tray "Exit" underneath the message box */
        MessageBoxW(g_hwnd, L"The file was moved or deleted.", L"U-loader", MB_OK | MB_ICONINFORMATION);
        modal_end();
        return;
    }
    wchar_t *w = u8_to_w(it->path);
    INT_PTR r = (INT_PTR)ShellExecuteW(g_hwnd, NULL, w, NULL, NULL, SW_SHOWNORMAL);
    if (r == SE_ERR_NOASSOC) ShellExecuteW(g_hwnd, L"openas", w, NULL, NULL, SW_SHOWNORMAL);
    free(w);
}

static void open_folder_of(item *it)
{
    if (!it) return;
    if (it->path && file_exists(it->path)) {
        wchar_t *w = u8_to_w(it->path);
        PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(w);
        free(w);
        if (pidl) {
            HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, NULL, 0);
            ILFree(pidl);
            if (SUCCEEDED(hr)) return;
        }
    }
    char *folder = it->path ? path_dirname(it->path) : xstrdup(it->dir ? it->dir : g_set.dir);
    if (!dir_exists(folder)) dir_create(folder);
    wchar_t *w = u8_to_w(folder);
    ShellExecuteW(g_hwnd, L"open", w, NULL, NULL, SW_SHOWNORMAL);
    free(w);
    free(folder);
}

static void activate(item *it)
{
    trace("activate %s", it ? it->name : "(none)");
    if (!it) return;
    if (it->state == TS_DONE) open_item(it);
    else if (!it->thread && (it->state == TS_PAUSED || it->state == TS_ERROR)) {
        resume_item(it);
        schedule();
        list_changed();
    }
}

static void paste_links(void)
{
    char *s = clipboard_text();
    char *links = s ? extract_links(s) : NULL;
    free(s);
    if (!links) {
        MessageBeep(MB_ICONWARNING);
        SendMessageW(g_sb, SB_SETTEXTW, 0, (LPARAM)L"The clipboard has no http, https, ftp or magnet links.");
        g_status_note_until = now_sec() + 4.0;       /* not for good: update_status takes over */
        return;
    }
    char **v;
    int n = parse_links(links, &v);
    free(links);
    item **items = xcalloc((size_t)n + 1, sizeof *items);
    for (int i = 0; i < n; i++) {
        items[i] = item_new(v[i]);
        link_seen(v[i], 1);
        snap_update(items[i]);
    }
    add_items(items, n, 1);
    free(items);
    free_strv(v, n);
    schedule();
}

static void copy_links(void)
{
    item **sel;
    int n = selection(&sel);
    sbuf b = { 0 };
    for (int k = 0; k < n; k++) {
        if (k) sb_puts(&b, "\r\n");
        sb_puts(&b, sel[k]->t->url);
        link_seen(sel[k]->t->url, 1);
    }
    if (b.len) clipboard_set(b.s);
    sb_free(&b);
    free(sel);
}

static void modal_begin(void) { g_modal++; }
static void modal_end(void)
{
    if (--g_modal == 0 && g_exit_pending) PostMessageW(g_hwnd, WM_COMMAND, IDM_EXIT, 0);
}

static void delete_selected(int with_files)
{
    item **sel;
    int n = selection(&sel);
    if (!n) { free(sel); return; }
    if (with_files) {
        wchar_t msg[400];
        int incomplete = 0;
        for (int k = 0; k < n; k++) if (sel[k]->state != TS_DONE) incomplete++;
        if (n == 1) {
            wchar_t nm[200];
            u8w(sel[0]->name, nm, 200);
            if (incomplete) _snwprintf(msg, 400, L"Delete \"%ls\" and the part downloaded so far?", nm);
            else _snwprintf(msg, 400, L"Delete \"%ls\" and move its file to the Recycle Bin?", nm);
        } else if (incomplete == n) {
            _snwprintf(msg, 400, L"Delete %d downloads and the parts downloaded so far?", n);
        } else if (incomplete) {
            _snwprintf(msg, 400, L"Delete %d downloads?\n\nFinished files go to the Recycle Bin; "
                                 L"unfinished parts are deleted.", n);
        } else {
            _snwprintf(msg, 400, L"Delete %d downloads and move their files to the Recycle Bin?", n);
        }
        msg[399] = 0;
        modal_begin();
        int r = MessageBoxW(g_hwnd, msg, L"U-loader", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2);
        modal_end();
        if (r != IDOK || g_exiting) { free(sel); return; }
        /* the selection may have changed meanwhile: keep only items still listed */
        int m = 0;
        for (int k = 0; k < n; k++) if (index_of(sel[k]) >= 0) sel[m++] = sel[k];
        n = m;
    }
    remove_items(sel, n, with_files);
    free(sel);
}

/* ------------------------------------------------------------------ */
/* detail pane                                                         */
/* ------------------------------------------------------------------ */
static int detail_row_h(void) { return g_line_h + S(6); }

static int info_height(void)
{
    return S(12) + g_title_h + S(10) + S(14) + S(12) + 6 * detail_row_h() + S(6);
}

static int log_item_h(void) { return g_mono_h + S(3); }

/* The log takes what is left under the details; on a short pane it goes first. */
static void detail_layout(void)
{
    if (!g_detail || !g_log) return;
    RECT rc;
    GetClientRect(g_detail, &rc);
    int pad = S(14);
    RECT f = { pad, info_height(), rc.right - pad, rc.bottom - S(10) };
    int room = f.bottom - f.top >= log_item_h() * 2 + 2;
    if (!room) SetRectEmpty(&f);
    g_log_frame = f;
    if (room) MoveWindow(g_log, f.left + 1, f.top + 1, f.right - f.left - 2, f.bottom - f.top - 2, TRUE);
    ShowWindow(g_log, room && g_detail_item ? SW_SHOWNA : SW_HIDE);
}

static void draw_segmap(HDC dc, const RECT *r, const item *it)
{
    fill(dc, r, C_TRACK);
    int w = r->right - r->left;
    if (w <= 0) return;
    COLORREF col = state_color(it);
    int gap = w / UL_SEGVIEW >= 5 ? 1 : 0;
    int unknown = it->total <= 0 && it->state != TS_DONE;
    for (int i = 0; i < UL_SEGVIEW; i++) {
        RECT c = { r->left + w * i / UL_SEGVIEW, r->top, r->left + w * (i + 1) / UL_SEGVIEW - gap, r->bottom };
        int v = it->state == TS_DONE ? 255 : unknown ? 0 : it->map[i];
        fill(dc, &c, v ? lerp(C_TRACK, col, 60 + v * 195 / 255) : C_TRACK);
        if (gap && i < UL_SEGVIEW - 1) {
            RECT g = { c.right, r->top, c.right + gap, r->bottom };
            fill(dc, &g, C_PANEL);
        }
    }
}

static void detail_field(HDC dc, int x, int y, int lw, int right, const wchar_t *label, const char *value,
                         COLORREF vcol, UINT extra)
{
    int rh = detail_row_h();
    RECT lr = { x, y, x + lw, y + rh };
    SelectObject(dc, g_font);
    SetTextColor(dc, C_DIM);
    DrawTextW(dc, label, -1, &lr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    RECT vr = { x + lw, y, right, y + rh };
    SetTextColor(dc, vcol);
    text_u8(dc, value, &vr, DT_SINGLELINE | DT_VCENTER | extra);
}

static void detail_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    if (W <= 0 || H <= 0) { EndPaint(hwnd, &ps); return; }
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, W, H);
    HGDIOBJ ob = SelectObject(dc, bmp), of = SelectObject(dc, g_font);
    SetBkMode(dc, TRANSPARENT);
    fill(dc, &rc, C_PANEL);

    item *it = g_detail_item;
    int pad = S(14), y = S(12);
    if (!it) {
        RECT r = { pad, y, W - pad, y + g_title_h };
        SelectObject(dc, g_font_title);
        SetTextColor(dc, C_FAINT);
        DrawTextW(dc, L"No download selected", -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        r.top = r.bottom + S(6);
        r.bottom = r.top + g_line_h + S(4);
        SelectObject(dc, g_font);
        DrawTextW(dc, g_n ? L"Select a download to see its segments, details and log."
                          : L"Press Ctrl+N to add a link, or paste one with Ctrl+V.",
                  -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    } else {
        char buf[1024], a[32], b[32];
        /* title + percentage */
        SelectObject(dc, g_font_title);
        char pct[32] = "";
        if (it->state == TS_DONE) snprintf(pct, sizeof pct, "100%%");
        else if (it->total > 0) snprintf(pct, sizeof pct, "%.1f%%", item_frac(it) * 100.0);
        wchar_t wp[32];
        u8w(pct, wp, 32);
        int pw = pct[0] ? text_width(dc, g_font_title, wp) + S(16) : 0;
        RECT tr = { pad, y, W - pad - pw, y + g_title_h };
        SetTextColor(dc, C_TEXT);
        text_u8(dc, it->name, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        if (pct[0]) {
            RECT pr = { W - pad - pw, y, W - pad, y + g_title_h };
            SetTextColor(dc, state_text_color(it));
            DrawTextW(dc, wp, -1, &pr, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
        }
        y += g_title_h + S(10);

        /* segment map */
        RECT mr = { pad, y, W - pad, y + S(14) };
        draw_segmap(dc, &mr, it);
        y += S(14) + S(12);

        /* two columns of fields */
        const wchar_t *labels[] = { L"Status", L"Downloaded", L"Connections", L"Speed", L"Time left",
                                    L"Stage", L"Link", L"Saved to", L"Message" };
        int lw = 0;
        for (int i = 0; i < 9; i++) {
            int tw = text_width(dc, g_font, labels[i]);
            if (tw > lw) lw = tw;
        }
        lw += S(14);
        int colw = (W - 2 * pad) / 2, rh = detail_row_h();
        int x1 = pad, x2 = pad + colw;
        const char *dash = "\xE2\x80\x94";

        status_text(it, buf, sizeof buf);
        detail_field(dc, x1, y, lw, x2 - S(12), labels[0], buf, state_text_color(it), DT_END_ELLIPSIS);
        if (it->thread && it->speed > 0) snprintf(buf, sizeof buf, "%s/s", fmt_size(it->speed, a, sizeof a));
        else snprintf(buf, sizeof buf, "%s", dash);
        detail_field(dc, x2, y, lw, W - pad, labels[3], buf, C_TEXT, DT_END_ELLIPSIS);
        y += rh;

        if (it->total > 0 && it->state != TS_DONE)
            snprintf(buf, sizeof buf, "%s of %s", fmt_size(it->done, a, sizeof a), fmt_size(it->total, b, sizeof b));
        else if (it->total > 0)
            snprintf(buf, sizeof buf, "%s", fmt_size(it->total, a, sizeof a));
        else if (it->done > 0)
            snprintf(buf, sizeof buf, "%s (size unknown)", fmt_size(it->done, a, sizeof a));
        else
            snprintf(buf, sizeof buf, "%s", dash);
        detail_field(dc, x1, y, lw, x2 - S(12), labels[1], buf, C_TEXT, DT_END_ELLIPSIS);
        if (it->thread && it->eta >= 0 && !it->pausing) fmt_eta((double)it->eta, buf, sizeof buf);
        else snprintf(buf, sizeof buf, "%s", dash);
        detail_field(dc, x2, y, lw, W - pad, labels[4], buf, C_TEXT, DT_END_ELLIPSIS);
        y += rh;

        if (it->thread) snprintf(buf, sizeof buf, "%d active  (up to %d)", it->nconn, it->conns);
        else snprintf(buf, sizeof buf, "up to %d", it->conns);
        detail_field(dc, x1, y, lw, x2 - S(12), labels[2], buf, C_TEXT, DT_END_ELLIPSIS);
        snprintf(buf, sizeof buf, "%s", it->stage[0] ? it->stage : dash);
        detail_field(dc, x2, y, lw, W - pad, labels[5], buf, C_TEXT, DT_END_ELLIPSIS);
        y += rh;

        /* the engine's last message: "Done: 30 MB in 4.5s", a warning, or the error */
        snprintf(buf, sizeof buf, "%s", it->status[0] && strcmp(it->status, "Paused") ? it->status : dash);
        detail_field(dc, x1, y, lw, W - pad, labels[8], buf, it->state == TS_ERROR ? C_RED : C_TEXT, DT_END_ELLIPSIS);
        y += rh;

        detail_field(dc, x1, y, lw, W - pad, labels[6], it->t->url, C_TEXT, DT_END_ELLIPSIS);
        y += rh;
        if (it->path) snprintf(buf, sizeof buf, "%s", it->path);
        else snprintf(buf, sizeof buf, "%s  (folder)", it->dir ? it->dir : g_set.dir);
        detail_field(dc, x1, y, lw, W - pad, labels[7], buf, C_TEXT, DT_PATH_ELLIPSIS);

        /* frame around the log */
        RECT f = g_log_frame;
        if (!IsRectEmpty(&f)) {
        HPEN pen = CreatePen(PS_SOLID, 1, C_FRAME);
        HGDIOBJ op = SelectObject(dc, pen), obr = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, f.left, f.top, f.right, f.bottom);
        SelectObject(dc, obr);
        SelectObject(dc, op);
        DeleteObject(pen);
        }
    }
    BitBlt(wdc, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, of);
    SelectObject(dc, ob);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

/* bring the log list box up to date with g_detail_item's log */
static void log_sync(int reset)
{
    item *it = g_detail_item;
    if (reset) {
        SendMessageW(g_log, LB_RESETCONTENT, 0, 0);
        g_log_shown_seq = 0;
    }
    if (!it) return;
    EnterCriticalSection(&it->loglock);
    unsigned seq = it->log_seq;
    int count = it->log_count;
    unsigned want = seq - g_log_shown_seq;
    if (want == 0) { LeaveCriticalSection(&it->loglock); return; }
    if (want > (unsigned)count) {          /* lines were dropped: rebuild */
        reset = 1;
        want = (unsigned)count;
    }
    char **lines = xcalloc(want + 1, sizeof *lines);
    int *kinds = xcalloc(want + 1, sizeof *kinds);
    for (unsigned k = 0; k < want; k++) {
        int idx = (it->log_head + count - (int)want + (int)k) % LOG_LINES;
        lines[k] = xstrdup(it->log[idx].text ? it->log[idx].text : "");
        kinds[k] = it->log[idx].kind;
    }
    LeaveCriticalSection(&it->loglock);

    int total = (int)SendMessageW(g_log, LB_GETCOUNT, 0, 0);
    RECT cr;
    GetClientRect(g_log, &cr);
    int top = (int)SendMessageW(g_log, LB_GETTOPINDEX, 0, 0);
    int visible = log_item_h() > 0 ? (cr.bottom / log_item_h()) : 1;
    int at_end = reset || total == 0 || top + visible >= total;
    SendMessageW(g_log, WM_SETREDRAW, FALSE, 0);
    if (reset && total) SendMessageW(g_log, LB_RESETCONTENT, 0, 0);
    for (unsigned k = 0; k < want; k++) {
        wchar_t *w = u8_to_w(lines[k]);
        LRESULT idx = SendMessageW(g_log, LB_ADDSTRING, 0, (LPARAM)w);
        if (idx >= 0) SendMessageW(g_log, LB_SETITEMDATA, (WPARAM)idx, (LPARAM)kinds[k]);
        free(w);
        free(lines[k]);
    }
    free(lines);
    free(kinds);
    total = (int)SendMessageW(g_log, LB_GETCOUNT, 0, 0);
    while (total > LOG_LINES) {
        SendMessageW(g_log, LB_DELETESTRING, 0, 0);
        total--;
    }
    if (at_end && total > 0) SendMessageW(g_log, LB_SETTOPINDEX, (WPARAM)(total - 1), 0);
    SendMessageW(g_log, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_log, NULL, FALSE);
    g_log_shown_seq = seq;
}

static void detail_refresh(int force)
{
    if (!g_detail) return;
    item *it = focused_item();
    if (it != g_detail_item) {
        g_detail_item = it;
        force = 1;
        log_sync(1);
        detail_layout();
    } else if (it) {
        log_sync(0);
    }
    if (force || g_detail_dirty || (it && it->thread)) {
        RECT r;
        GetClientRect(g_detail, &r);
        r.bottom = info_height();
        InvalidateRect(g_detail, &r, FALSE);
        if (force) InvalidateRect(g_detail, NULL, FALSE);
    }
    g_detail_dirty = 0;
}

static void log_draw(const DRAWITEMSTRUCT *d)
{
    if (d->itemID == (UINT)-1) return;
    HDC dc = d->hDC;
    RECT r = d->rcItem;
    int sel = (d->itemState & ODS_SELECTED) != 0;
    fill(dc, &r, sel ? C_SEL : C_WHITE);
    int n = (int)SendMessageW(g_log, LB_GETTEXTLEN, d->itemID, 0);
    if (n <= 0) return;
    wchar_t *w = xcalloc((size_t)n + 1, sizeof(wchar_t));
    SendMessageW(g_log, LB_GETTEXT, d->itemID, (LPARAM)w);
    int kind = (int)d->itemData;
    HGDIOBJ of = SelectObject(dc, g_font_mono);
    SetBkMode(dc, TRANSPARENT);
    r.left += S(6);
    int pre = n >= 10 ? 8 : 0;                  /* "hh:mm:ss" */
    if (pre) {
        SetTextColor(dc, C_FAINT);
        RECT tr = r;
        DrawTextW(dc, w, pre, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        SIZE sz;
        GetTextExtentPoint32W(dc, w, pre, &sz);
        r.left += sz.cx + S(12);
    }
    COLORREF c = C_TEXT;
    switch (kind) {
    case LOG_ERR:  c = C_RED; break;
    case LOG_WARN: c = RGB(0x9A, 0x5B, 0x00); break;
    case LOG_OK:   c = RGB(0x0E, 0x7A, 0x36); break;
    case LOG_HEAD: c = RGB(0x00, 0x5F, 0xB8); break;
    case LOG_DIM:  c = C_DIM; break;
    default: break;
    }
    SetTextColor(dc, c);
    const wchar_t *msg = w + (pre ? 10 : 0);
    if (pre && n < 10) msg = w + n;
    DrawTextW(dc, msg, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, of);
    free(w);
}

static void log_copy(int all)
{
    int total = (int)SendMessageW(g_log, LB_GETCOUNT, 0, 0);
    sbuf b = { 0 };
    for (int i = 0; i < total; i++) {
        if (!all && SendMessageW(g_log, LB_GETSEL, (WPARAM)i, 0) <= 0) continue;
        int n = (int)SendMessageW(g_log, LB_GETTEXTLEN, (WPARAM)i, 0);
        if (n < 0) continue;
        wchar_t *w = xcalloc((size_t)n + 1, sizeof(wchar_t));
        SendMessageW(g_log, LB_GETTEXT, (WPARAM)i, (LPARAM)w);
        char *u = w_to_u8(w);
        sb_puts(&b, u);
        sb_puts(&b, "\r\n");
        free(u);
        free(w);
    }
    if (b.len) clipboard_set(b.s);
    sb_free(&b);
}

static LRESULT CALLBACK detail_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_log = CreateWindowExW(0, L"LISTBOX", NULL,
                                WS_CHILD | WS_VSCROLL | WS_TABSTOP | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS |
                                LBS_NOINTEGRALHEIGHT | LBS_EXTENDEDSEL | LBS_WANTKEYBOARDINPUT | LBS_NOTIFY,
                                0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_LOG, g_inst, NULL);
        SetWindowTheme(g_log, L"Explorer", NULL);
        SendMessageW(g_log, WM_SETFONT, (WPARAM)g_font_mono, FALSE);
        return 0;
    case WM_SIZE:
        detail_layout();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        detail_paint(hwnd);
        return 0;
    case WM_MEASUREITEM:
        ((MEASUREITEMSTRUCT *)lp)->itemHeight = (UINT)log_item_h();
        return TRUE;
    case WM_DRAWITEM:
        log_draw((const DRAWITEMSTRUCT *)lp);
        return TRUE;
    case WM_CTLCOLORLISTBOX:
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    case WM_VKEYTOITEM:
        if (LOWORD(wp) == 'C' && GetKeyState(VK_CONTROL) < 0) { log_copy(0); return -2; }
        return -1;
    case WM_CONTEXTMENU:
        if ((HWND)wp == g_log) {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (pt.x == -1 && pt.y == -1) { pt.x = pt.y = 0; ClientToScreen(g_log, &pt); }
            HMENU m = CreatePopupMenu();
            int any = (int)SendMessageW(g_log, LB_GETSELCOUNT, 0, 0) > 0;
            AppendMenuW(m, MF_STRING | (any ? 0 : MF_GRAYED), IDM_LOGCOPY, L"&Copy\tCtrl+C");
            AppendMenuW(m, MF_STRING, IDM_LOGCOPYALL, L"Copy &all");
            int cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, hwnd, NULL);
            DestroyMenu(m);
            if (cmd == IDM_LOGCOPY) log_copy(0);
            else if (cmd == IDM_LOGCOPYALL) log_copy(1);
            return 0;
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* main window layout and painting                                     */
/* ------------------------------------------------------------------ */
static int split_h(void) { return S(7); }

/* Make the columns fill the list exactly - never a horizontal scrollbar.
   The Name column takes what is left; on a narrow window the others shrink
   toward a minimum and then the least important ones are hidden (they come
   back when the window grows). g_set.colw holds the widths the user chose. */
static void fit_columns(void)
{
    static const int MINW[NCOLS] = { 120, 56, 84, 62, 58, 92, 96 };  /* 96-dpi pixels */
    static const int DROP[] = { COL_ETA, COL_SPEED, COL_SIZE };      /* last resorts */
    if (!g_lv) return;
    RECT wr;
    GetWindowRect(g_lv, &wr);
    /* always reserve the vertical scrollbar, so its coming and going
       does not make the columns jump */
    int avail = (wr.right - wr.left) - metric(SM_CXVSCROLL) - S(4);
    int w[NCOLS], others = 0, name_min = S(MINW[COL_NAME]);
    for (int i = 0; i < NCOLS; i++) {
        w[i] = i == COL_NAME ? 0 : S(g_set.colw[i]);
        others += w[i];
    }
    /* the name is what people read: it deserves about a third of the width */
    int name_want = avail * 32 / 100 > name_min ? avail * 32 / 100 : name_min;
    if (avail - others < name_want && w[COL_ADDED]) {      /* least useful column goes first */
        others -= w[COL_ADDED];
        w[COL_ADDED] = 0;
    }
    if (avail - others < name_want) {
        int need = name_want - (avail - others), slack = 0;
        for (int i = 0; i < NCOLS; i++)
            if (i != COL_NAME && w[i] && w[i] > S(MINW[i])) slack += w[i] - S(MINW[i]);
        if (slack > 0) {
            int take = need < slack ? need : slack;
            for (int i = 0; i < NCOLS; i++) {
                if (i == COL_NAME || !w[i] || w[i] <= S(MINW[i])) continue;
                int cut = (int)((int64_t)(w[i] - S(MINW[i])) * take / slack) + 1;
                if (cut > w[i] - S(MINW[i])) cut = w[i] - S(MINW[i]);
                w[i] -= cut;
                others -= cut;
            }
        }
        for (size_t k = 0; k < sizeof DROP / sizeof *DROP && avail - others < name_min; k++) {
            others -= w[DROP[k]];
            w[DROP[k]] = 0;
        }
    }
    w[COL_NAME] = avail - others > name_min ? avail - others : name_min;
    SendMessageW(g_lv, WM_SETREDRAW, FALSE, 0);
    for (int i = 0; i < NCOLS; i++)
        if (ListView_GetColumnWidth(g_lv, i) != w[i]) ListView_SetColumnWidth(g_lv, i, w[i]);
    int hpos = GetScrollPos(g_lv, SB_HORZ);
    if (hpos > 0) ListView_Scroll(g_lv, -hpos - S(2000), 0);     /* back to the left edge */
    SendMessageW(g_lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_lv, NULL, FALSE);
}

static void layout(void)
{
    if (!g_lv || !g_tb || !g_sb || !g_detail) return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    SendMessageW(g_sb, WM_SIZE, 0, 0);
    RECT sr;
    GetWindowRect(g_sb, &sr);
    int sbh = sr.bottom - sr.top;
    int parts[2] = { w - S(260) > S(120) ? w - S(260) : S(120), -1 };
    SendMessageW(g_sb, SB_SETPARTS, 2, (LPARAM)parts);

    DWORD bs = (DWORD)SendMessageW(g_tb, TB_GETBUTTONSIZE, 0, 0);
    int tbh = HIWORD(bs), m = S(4);
    g_tb_area = tbh + 2 * m;
    int top = g_tb_area + 1, bottom = h - sbh;
    int sh = split_h();
    int avail = bottom - top - sh;
    if (avail < 0) avail = 0;
    /* The detail pane always shows all of its rows (only the log below
       them gives way); the list keeps at least a few rows. */
    int list_min = S(96);
    int dh = S(g_set.detail_h);
    if (dh < info_height()) dh = info_height();
    if (dh > avail - list_min) dh = avail - list_min;
    if (dh < S(60)) dh = S(60) < avail ? S(60) : avail;
    int lh = avail - dh;
    if (g_list_w >= 0) g_list_w = w;

    HDWP dw = BeginDeferWindowPos(4);
    dw = DeferWindowPos(dw, g_tb, NULL, S(6), m, w - S(12), tbh, SWP_NOZORDER | SWP_NOACTIVATE);
    dw = DeferWindowPos(dw, g_lv, NULL, 0, top, w, lh, SWP_NOZORDER | SWP_NOACTIVATE);
    dw = DeferWindowPos(dw, g_detail, NULL, 0, top + lh + sh, w, dh, SWP_NOZORDER | SWP_NOACTIVATE);
    EndDeferWindowPos(dw);
    fit_columns();
    SetRect(&g_split_rc, 0, top + lh, w, top + lh + sh);
    RECT inv = { 0, 0, w, top };
    InvalidateRect(g_hwnd, &inv, TRUE);
    InvalidateRect(g_hwnd, &g_split_rc, TRUE);
}

static void paint_bg(HDC dc)
{
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    RECT r = { 0, 0, rc.right, g_tb_area };
    fill(dc, &r, C_WHITE);
    RECT l = { 0, g_tb_area, rc.right, g_tb_area + 1 };
    fill(dc, &l, C_LINE);
    if (g_split_rc.bottom > g_split_rc.top) {
        fill(dc, &g_split_rc, C_PANEL);
        RECT t = { g_split_rc.left, g_split_rc.top, g_split_rc.right, g_split_rc.top + 1 };
        fill(dc, &t, C_LINE);
        /* a small grip in the middle */
        int cx = (rc.right) / 2, cy = (g_split_rc.top + g_split_rc.bottom) / 2 + 1;
        for (int i = -2; i <= 2; i++) {
            RECT d = { cx + i * S(5) - 1, cy - 1, cx + i * S(5) + 1, cy + 1 };
            fill(dc, &d, C_FAINT);
        }
    }
}

/* ------------------------------------------------------------------ */
/* status bar and tray tip                                             */
/* ------------------------------------------------------------------ */
static void update_status(void)
{
    static wchar_t last_a[200], last_b[200], last_tip[128];
    int active = 0, queued = 0, done = 0;
    int64_t speed = 0;
    for (int i = 0; i < g_n; i++) {
        item *it = g_items[i];
        if (it->thread) { active++; speed += it->speed; }
        else if (it->state == TS_QUEUED) queued++;
        else if (it->state == TS_DONE) done++;
    }
    char a[200], b[200], s[32];
    snprintf(a, sizeof a, "%d download%s   \xC2\xB7   %d active   \xC2\xB7   %d queued   \xC2\xB7   %d complete",
             g_n, g_n == 1 ? "" : "s", active, queued, done);
    if (g_set.limit_kb > 0)
        snprintf(b, sizeof b, "\xE2\x86\x93 %s/s   (limit %d KB/s each)", fmt_size(speed, s, sizeof s), g_set.limit_kb);
    else
        snprintf(b, sizeof b, "\xE2\x86\x93 %s/s", fmt_size(speed, s, sizeof s));
    wchar_t wa[200], wb[200];
    u8w(a, wa, 200);
    u8w(b, wb, 200);
    /* a short note (status_note) owns the first part for a few seconds,
       then the counts come back */
    if (g_status_note_until > 0) {
        if (now_sec() < g_status_note_until) wcscpy(wa, last_a);
        else { g_status_note_until = 0; last_a[0] = 0; }
    }
    if (wcscmp(wa, last_a)) { SendMessageW(g_sb, SB_SETTEXTW, 0, (LPARAM)wa); wcscpy(last_a, wa); }
    if (wcscmp(wb, last_b)) { SendMessageW(g_sb, SB_SETTEXTW, 1, (LPARAM)wb); wcscpy(last_b, wb); }

    char tip[128];
    if (active) snprintf(tip, sizeof tip, "U-loader\n%d downloading \xC2\xB7 %s/s", active, fmt_size(speed, s, sizeof s));
    else if (queued) snprintf(tip, sizeof tip, "U-loader\n%d queued", queued);
    else snprintf(tip, sizeof tip, "U-loader");
    wchar_t wt[128];
    u8w(tip, wt, 128);
    if (wcscmp(wt, last_tip)) { tray_tip(wt); wcscpy(last_tip, wt); }
}

/* ------------------------------------------------------------------ */
/* the periodic refresh                                                */
/* ------------------------------------------------------------------ */
static void on_tick(void)
{
    /* threads that ended without their message being seen yet */
    for (int i = 0; i < g_n; i++) {
        item *it = g_items[i];
        if (it->thread && WaitForSingleObject(it->thread, 0) == WAIT_OBJECT_0) task_finished(it);
    }
    for (int i = 0; i < g_ngone; i++) {
        item *it = g_gone[i];
        if (it->thread && WaitForSingleObject(it->thread, 0) == WAIT_OBJECT_0) task_finished(it);
    }
    int any_running = 0;
    for (int i = 0; i < g_n; i++) {
        item *it = g_items[i];
        if (it->thread) any_running = 1;
        if (snap_update(it)) {
            ListView_RedrawItems(g_lv, i, i);
            if (it == g_detail_item) g_detail_dirty = 1;
        }
    }
    detail_refresh(0);
    update_status();
    update_toolbar();
    if (any_running && now_sec() - g_last_periodic_save > 30.0) save_list();
}

/* ------------------------------------------------------------------ */
/* folder picker                                                       */
/* ------------------------------------------------------------------ */
static char *pick_folder(HWND owner, const char *initial)
{
    IFileOpenDialog *d = NULL;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog_, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog_,
                                (void **)&d)) || !d)
        return NULL;
    FILEOPENDIALOGOPTIONS opt = 0;
    IFileOpenDialog_GetOptions(d, &opt);
    IFileOpenDialog_SetOptions(d, opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    IFileOpenDialog_SetTitle(d, L"Choose a folder for downloads");
    if (initial && *initial && dir_exists(initial)) {
        wchar_t *w = u8_to_w(initial);
        IShellItem *si = NULL;
        if (SUCCEEDED(SHCreateItemFromParsingName(w, NULL, &IID_IShellItem_, (void **)&si)) && si) {
            IFileOpenDialog_SetFolder(d, si);
            IShellItem_Release(si);
        }
        free(w);
    }
    char *out = NULL;
    if (SUCCEEDED(IFileOpenDialog_Show(d, owner))) {
        IShellItem *res = NULL;
        if (SUCCEEDED(IFileOpenDialog_GetResult(d, &res)) && res) {
            PWSTR p = NULL;
            if (SUCCEEDED(IShellItem_GetDisplayName(res, SIGDN_FILESYSPATH, &p)) && p) {
                out = w_to_u8(p);
                CoTaskMemFree(p);
            }
            IShellItem_Release(res);
        }
    }
    IFileOpenDialog_Release(d);
    return out;
}

/* ------------------------------------------------------------------ */
/* Add dialog                                                          */
/* ------------------------------------------------------------------ */
typedef struct {
    char *links;       /* in: prefill; out: what the user typed */
    int   topmost;
    char *dir;
    int   conns, quality, mode, paused;
} addreq;

static void add_dialog_append(const char *links)
{
    if (!g_add_dlg || !links) return;
    char *cur = dlg_text(g_add_dlg, IDC_LINKS);
    char *t = str_trim(cur);
    char *joined = *t ? xasprintf("%s\n%s", t, links) : xstrdup(links);
    char *c = crlf(joined);
    set_dlg_text(g_add_dlg, IDC_LINKS, c);
    free(c);
    free(joined);
    free(cur);
}

static INT_PTR CALLBACK add_proc(HWND d, UINT m, WPARAM w, LPARAM l)
{
    addreq *r = (addreq *)GetWindowLongPtrW(d, DWLP_USER);
    switch (m) {
    case WM_INITDIALOG: {
        r = (addreq *)l;
        SetWindowLongPtrW(d, DWLP_USER, (LONG_PTR)r);
        g_add_dlg = g_dlg = d;
        SendMessageW(d, WM_SETICON, ICON_SMALL, (LPARAM)g_icon_small);
        SendMessageW(d, WM_SETICON, ICON_BIG, (LPARAM)g_icon_big);
        if (r->links) {
            char *c = crlf(r->links);
            set_dlg_text(d, IDC_LINKS, c);
            free(c);
        }
        set_dlg_text(d, IDC_DIR, g_set.dir);
        SendDlgItemMessageW(d, IDC_CONNS_SPIN, UDM_SETRANGE32, 1, UL_MAX_CONN);
        SendDlgItemMessageW(d, IDC_CONNS_SPIN, UDM_SETPOS32, 0, g_set.conns);
        for (size_t i = 0; i < sizeof QUALITY_NAMES / sizeof *QUALITY_NAMES; i++)
            SendDlgItemMessageW(d, IDC_QUALITY, CB_ADDSTRING, 0, (LPARAM)QUALITY_NAMES[i]);
        SendDlgItemMessageW(d, IDC_QUALITY, CB_SETCURSEL, 0, 0);
        for (size_t i = 0; i < sizeof MODE_NAMES / sizeof *MODE_NAMES; i++)
            SendDlgItemMessageW(d, IDC_MODE, CB_ADDSTRING, 0, (LPARAM)MODE_NAMES[i]);
        SendDlgItemMessageW(d, IDC_MODE, CB_SETCURSEL, 0, 0);
        CheckRadioButton(d, IDC_START_NOW, IDC_ADD_PAUSED, IDC_START_NOW);
        SendDlgItemMessageW(d, IDC_LINKS, EM_SETLIMITTEXT, 1 << 20, 0);
        if (r->topmost) SetWindowPos(d, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetForegroundWindow(d);
        if (r->links && *r->links) {
            SetFocus(GetDlgItem(d, IDOK));
        } else {
            SetFocus(GetDlgItem(d, IDC_LINKS));
        }
        return FALSE;
    }
    case WM_CTLCOLORSTATIC:
        if ((HWND)l == GetDlgItem(d, IDC_LINKS_HINT)) {
            SetTextColor((HDC)w, C_DIM);
            SetBkMode((HDC)w, TRANSPARENT);
            return (INT_PTR)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDC_BROWSE: {
            char *cur = dlg_text(d, IDC_DIR);
            char *p = pick_folder(d, str_trim(cur));
            if (p) set_dlg_text(d, IDC_DIR, p);
            free(p);
            free(cur);
            return TRUE;
        }
        case IDOK: {
            char *links = dlg_text(d, IDC_LINKS);
            char **v;
            int n = parse_links(links, &v);
            free_strv(v, n);
            if (n == 0) {
                free(links);
                MessageBoxW(d, L"Enter at least one link (http, https, ftp or magnet), or the path of a .torrent file.",
                            L"Add download", MB_OK | MB_ICONINFORMATION);
                SetFocus(GetDlgItem(d, IDC_LINKS));
                return TRUE;
            }
            char *dir = dlg_text(d, IDC_DIR);
            char *dt = to_backslashes(str_trim(dir));
            if (!is_abs_path(dt)) {
                free(links);
                free(dir);
                MessageBoxW(d, L"Choose a folder to save into (a full path such as C:\\Downloads).", L"Add download",
                            MB_OK | MB_ICONINFORMATION);
                SetFocus(GetDlgItem(d, IDC_DIR));
                return TRUE;
            }
            BOOL ok = FALSE;
            UINT c = GetDlgItemInt(d, IDC_CONNS, &ok, FALSE);
            if (!ok || c < 1 || c > UL_MAX_CONN) {
                free(links);
                free(dir);
                MessageBoxW(d, L"Connections must be a number from 1 to 32.", L"Add download", MB_OK | MB_ICONINFORMATION);
                SetFocus(GetDlgItem(d, IDC_CONNS));
                return TRUE;
            }
            free(r->links);
            r->links = links;
            r->dir = xstrdup(dt);
            free(dir);
            r->conns = (int)c;
            int q = (int)SendDlgItemMessageW(d, IDC_QUALITY, CB_GETCURSEL, 0, 0);
            r->quality = (q >= 0 && q < (int)(sizeof QUALITY_VALUES / sizeof *QUALITY_VALUES)) ? QUALITY_VALUES[q] : 0;
            int md = (int)SendDlgItemMessageW(d, IDC_MODE, CB_GETCURSEL, 0, 0);
            r->mode = md == 1 ? MODE_DIRECT : md == 2 ? MODE_YTDLP : MODE_AUTO;
            r->paused = IsDlgButtonChecked(d, IDC_ADD_PAUSED) == BST_CHECKED;
            EndDialog(d, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(d, IDCANCEL);
            return TRUE;
        default:
            break;
        }
        break;
    case WM_DESTROY:
        if (g_add_dlg == d) g_add_dlg = NULL;
        if (g_dlg == d) g_dlg = NULL;
        break;
    default:
        break;
    }
    return FALSE;
}

static void open_add_dialog(const char *prefill, int topmost)
{
    if (g_exiting) return;
    if (g_add_dlg) {
        add_dialog_append(prefill);
        SetForegroundWindow(g_add_dlg);
        return;
    }
    if (g_modal) return;
    addreq r;
    memset(&r, 0, sizeof r);
    if (prefill) r.links = xstrdup(prefill);
    else {
        char *s = clipboard_text();
        r.links = s ? extract_links(s) : NULL;
        free(s);
    }
    r.topmost = topmost;
    modal_begin();
    INT_PTR res = DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_ADD), g_hwnd, add_proc, (LPARAM)&r);
    modal_end();
    if (res == IDOK && !g_exiting) {
        char **v;
        int n = parse_links(r.links, &v);
        item **items = xcalloc((size_t)n + 1, sizeof *items);
        for (int i = 0; i < n; i++) {
            item *it = item_new(v[i]);
            set_str(&it->dir, r.dir);
            it->conns = r.conns;
            it->quality = r.quality;
            it->mode = r.mode;
            it->state = r.paused ? TS_PAUSED : TS_QUEUED;
            if (r.paused) item_log(it, LOG_DIM, "Added (paused): %s", v[i]);
            link_seen(v[i], 1);
            snap_update(it);
            items[i] = it;
        }
        add_items(items, n, 1);
        free(items);
        free_strv(v, n);
        schedule();
    }
    free(r.links);
    free(r.dir);
}

/* ------------------------------------------------------------------ */
/* settings dialog and its helpers                                     */
/* ------------------------------------------------------------------ */
static void help_out(const char *s)
{
    EnterCriticalSection(&g_help_lock);
    sb_puts(&g_help_out, s);
    if (g_help_out.len > 65536) {          /* keep the tail */
        size_t cut = g_help_out.len - 49152;
        memmove(g_help_out.s, g_help_out.s + cut, g_help_out.len - cut + 1);
        g_help_out.len -= cut;
    }
    LeaveCriticalSection(&g_help_lock);
    InterlockedIncrement(&g_help_seq);
}

static void help_clear(void)
{
    EnterCriticalSection(&g_help_lock);
    sb_free(&g_help_out);
    LeaveCriticalSection(&g_help_lock);
    InterlockedIncrement(&g_help_seq);
}

static void help_on_log(ul_task *t, int kind, const char *msg)
{
    const char *pre = kind == LOG_ERR ? "Error: " : kind == LOG_WARN ? "Warning: " : "";
    char *line = xasprintf("%s%s\n", pre, msg);
    help_out(line);
    free(line);
}

static unsigned __stdcall connect_thread(void *arg)
{
    char *dir = exe_dir();
    char *exe = path_join(dir, "uloader.exe");
    if (!file_exists(exe)) {
        char *m = xasprintf("uloader.exe was not found in %s.\nIt must be in the same folder as uloader-gui.exe.\n", dir);
        help_out(m);
        free(m);
    } else {
        sbuf cmd = { 0 };
        cmd_append_arg(&cmd, exe);
        cmd_append_arg(&cmd, "--install");
        char *out = NULL;
        int rc = run_process(cmd.s, &out, NULL);
        sb_free(&cmd);
        if (out) {
            help_out(str_trim(out));
            help_out("\n");
        }
        free(out);
        if (rc == 0) help_out("\nDone. Load the extension folder below in your browser if you have not yet.\n");
        else {
            char *m = xasprintf("\nuloader.exe --install failed (exit code %d).\n", rc);
            help_out(m);
            free(m);
        }
    }
    free(exe);
    free(dir);
    InterlockedExchange(&g_connect_busy, 0);
    return 0;
}

static unsigned __stdcall ytdlp_thread(void *arg)
{
    ul_config *cfg = arg;
    http_session *s = http_session_new(cfg, g_yt_task);
    int rc = 1;
    if (s) {
        rc = ytdlp_install(s, cfg);
        http_session_free(s);
    } else {
        help_out("Error: cannot start WinHTTP\n");
    }
    cfg_free(cfg);
    help_out(rc == 0 ? "yt-dlp is ready: video sites can now be downloaded.\n" : "yt-dlp was not installed.\n");
    InterlockedExchange(&g_yt_busy, 0);
    return 0;
}

static void start_connect(void)
{
    if (g_connect_busy) return;
    if (g_connect_thread) { CloseHandle(g_connect_thread); g_connect_thread = NULL; }
    help_clear();
    help_out("Connecting U-loader to your browsers...\n");
    InterlockedExchange(&g_connect_busy, 1);
    g_connect_thread = (HANDLE)_beginthreadex(NULL, 0, connect_thread, NULL, 0, NULL);
    if (!g_connect_thread) { InterlockedExchange(&g_connect_busy, 0); help_out("Error: cannot start a thread\n"); }
}

static void start_ytdlp(void)
{
    if (g_yt_busy) return;
    if (g_yt_thread) { CloseHandle(g_yt_thread); g_yt_thread = NULL; }
    if (!g_yt_task) {
        g_yt_task = task_new("");
        g_yt_task->on_log = help_on_log;
        g_yt_task->on_progress = gui_on_progress;
    }
    g_yt_task->abort = 0;
    g_yt_task->done = 0;
    g_yt_task->total = -1;
    help_clear();
    InterlockedExchange(&g_yt_busy, 1);
    ul_config *cfg = default_cfg();
    cfg->speed_limit = 0;
    g_yt_thread = (HANDLE)_beginthreadex(NULL, 0, ytdlp_thread, cfg, 0, NULL);
    if (!g_yt_thread) {
        cfg_free(cfg);
        InterlockedExchange(&g_yt_busy, 0);
        help_out("Error: cannot start a thread\n");
    }
}

static void settings_refresh(HWND d, unsigned *shown, int *last_busy)
{
    unsigned seq = (unsigned)g_help_seq;
    if (seq != *shown) {
        *shown = seq;
        EnterCriticalSection(&g_help_lock);
        char *txt = xstrdup(g_help_out.s ? g_help_out.s : "");
        LeaveCriticalSection(&g_help_lock);
        char *c = crlf(txt);
        set_dlg_text(d, IDC_S_OUTPUT, c);
        HWND e = GetDlgItem(d, IDC_S_OUTPUT);
        int len = GetWindowTextLengthW(e);
        SendMessageW(e, EM_SETSEL, (WPARAM)len, (LPARAM)len);
        SendMessageW(e, EM_SCROLLCARET, 0, 0);
        free(c);
        free(txt);
    }
    EnableWindow(GetDlgItem(d, IDC_S_CONNECT), !g_connect_busy);
    EnableWindow(GetDlgItem(d, IDC_S_YTDLP), !g_yt_busy);
    if (g_yt_busy) {
        char buf[200], a[32], b[32];
        int64_t done = g_yt_task ? g_yt_task->done : 0, total = g_yt_task ? g_yt_task->total : -1;
        if (total > 0)
            snprintf(buf, sizeof buf, "Working... %.0f%%  (%s of %s)", 100.0 * (double)done / (double)total,
                     fmt_size(done, a, sizeof a), fmt_size(total, b, sizeof b));
        else snprintf(buf, sizeof buf, "Working...");
        set_dlg_text(d, IDC_S_YTSTATUS, buf);
        *last_busy = 1;
    } else if (*last_busy != 0) {
        *last_busy = 0;
        char *yt = find_tool("yt-dlp.exe");
        char *m = yt ? xasprintf("Installed: %s", yt) : xstrdup("Not installed (needed for YouTube and other video sites)");
        set_dlg_text(d, IDC_S_YTSTATUS, m);
        free(m);
        free(yt);
    }
}

static int settings_apply(HWND d)
{
    char *dir = dlg_text(d, IDC_S_DIR);
    char *dt = to_backslashes(str_trim(dir));
    BOOL ok1 = FALSE, ok2 = FALSE, ok3 = FALSE;
    UINT conns = GetDlgItemInt(d, IDC_S_CONNS, &ok1, FALSE);
    UINT maxa = GetDlgItemInt(d, IDC_S_MAXACT, &ok2, FALSE);
    UINT lim = GetDlgItemInt(d, IDC_S_LIMIT, &ok3, FALSE);
    if (GetWindowTextLengthW(GetDlgItem(d, IDC_S_LIMIT)) == 0) { ok3 = TRUE; lim = 0; }
    const wchar_t *err = NULL;
    int focus = 0;
    if (!is_abs_path(dt)) { err = L"Choose a default folder (a full path such as C:\\Downloads)."; focus = IDC_S_DIR; }
    else if (!ok1 || conns < 1 || conns > UL_MAX_CONN) { err = L"Connections must be a number from 1 to 32."; focus = IDC_S_CONNS; }
    else if (!ok2 || maxa < 1 || maxa > 20) { err = L"Simultaneous downloads must be a number from 1 to 20."; focus = IDC_S_MAXACT; }
    else if (!ok3 || lim > 10000000) { err = L"The speed limit must be a number of KB/s (0 = unlimited)."; focus = IDC_S_LIMIT; }
    if (err) {
        free(dir);
        MessageBoxW(d, err, L"Settings", MB_OK | MB_ICONINFORMATION);
        SetFocus(GetDlgItem(d, focus));
        return 0;
    }
    set_str(&g_set.dir, dt);
    free(dir);
    g_set.conns = (int)conns;
    g_set.max_active = (int)maxa;
    g_set.limit_kb = (int)lim;
    g_set.clip = IsDlgButtonChecked(d, IDC_S_CLIP) == BST_CHECKED;
    g_set.tray = IsDlgButtonChecked(d, IDC_S_TRAY) == BST_CHECKED;
    clip_listen(g_set.clip);
    settings_save();
    update_status();
    schedule();
    return 1;
}

static INT_PTR CALLBACK settings_proc(HWND d, UINT m, WPARAM w, LPARAM l)
{
    static unsigned shown;
    static int last_busy;
    switch (m) {
    case WM_INITDIALOG: {
        g_dlg = d;
        SendMessageW(d, WM_SETICON, ICON_SMALL, (LPARAM)g_icon_small);
        set_dlg_text(d, IDC_S_DIR, g_set.dir);
        SendDlgItemMessageW(d, IDC_S_CONNS_SPIN, UDM_SETRANGE32, 1, UL_MAX_CONN);
        SendDlgItemMessageW(d, IDC_S_CONNS_SPIN, UDM_SETPOS32, 0, g_set.conns);
        SendDlgItemMessageW(d, IDC_S_MAXACT_SPIN, UDM_SETRANGE32, 1, 20);
        SendDlgItemMessageW(d, IDC_S_MAXACT_SPIN, UDM_SETPOS32, 0, g_set.max_active);
        SetDlgItemInt(d, IDC_S_LIMIT, (UINT)g_set.limit_kb, FALSE);
        CheckDlgButton(d, IDC_S_CLIP, g_set.clip ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(d, IDC_S_TRAY, g_set.tray ? BST_CHECKED : BST_UNCHECKED);
        char *ed = exe_dir();
        char *ext = path_join(ed, "extension");
        set_dlg_text(d, IDC_S_EXTPATH, ext);
        free(ext);
        free(ed);
        set_dlg_text(d, IDC_S_VERSION, UL_NAME " " UL_VERSION);
        shown = (unsigned)-1;
        last_busy = -1;
        settings_refresh(d, &shown, &last_busy);
        SetTimer(d, 1, 250, NULL);
        SetFocus(GetDlgItem(d, IDOK));      /* not the folder box: it would open scrolled and selected */
        return FALSE;
    }
    case WM_TIMER:
        settings_refresh(d, &shown, &last_busy);
        return TRUE;
    case WM_NOTIFY: {
        NMHDR *h = (NMHDR *)l;
        if (h->idFrom == IDC_S_EXTLINK && (h->code == NM_CLICK || h->code == NM_RETURN)) {
            char *ed = exe_dir();
            char *ext = path_join(ed, "extension");
            if (dir_exists(ext)) {
                wchar_t *we = u8_to_w(ext);
                ShellExecuteW(d, L"open", we, NULL, NULL, SW_SHOWNORMAL);
                free(we);
            } else {
                MessageBoxW(d, L"The extension folder was not found next to uloader-gui.exe.", L"Settings",
                            MB_OK | MB_ICONINFORMATION);
            }
            free(ext);
            free(ed);
            return TRUE;
        }
        break;
    }
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDC_S_BROWSE: {
            char *cur = dlg_text(d, IDC_S_DIR);
            char *p = pick_folder(d, str_trim(cur));
            if (p) set_dlg_text(d, IDC_S_DIR, p);
            free(p);
            free(cur);
            return TRUE;
        }
        case IDC_S_CONNECT:
            start_connect();
            settings_refresh(d, &shown, &last_busy);
            return TRUE;
        case IDC_S_YTDLP:
            start_ytdlp();
            settings_refresh(d, &shown, &last_busy);
            return TRUE;
        case IDOK:
            if (settings_apply(d)) EndDialog(d, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(d, IDCANCEL);
            return TRUE;
        default:
            break;
        }
        break;
    case WM_DESTROY:
        KillTimer(d, 1);
        if (g_dlg == d) g_dlg = NULL;
        break;
    default:
        break;
    }
    return FALSE;
}

static void open_settings(void)
{
    if (g_modal || g_exiting) {
        if (g_dlg) SetForegroundWindow(g_dlg);
        return;
    }
    modal_begin();
    DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_SETTINGS), g_hwnd, settings_proc, 0);
    modal_end();
    InvalidateRect(g_lv, NULL, FALSE);
}

/* ------------------------------------------------------------------ */
/* browser / command line intake                                       */
/* ------------------------------------------------------------------ */
static int accept_job(const char *path)
{
    ul_config *cfg = default_cfg();
    char *url = NULL;
    if (job_load(path, cfg, &url) != 0 || !url || !*url) {
        cfg_free(cfg);
        free(url);
        return 0;
    }
    file_delete(path);
    item *it = item_new(url);
    to_backslashes(cfg->out_dir);
    set_str(&it->dir, is_abs_path(cfg->out_dir) ? cfg->out_dir : g_set.dir);
    set_str(&it->out_name, cfg->out_name);
    set_str(&it->referer, cfg->referer);
    set_str(&it->ua, cfg->user_agent);
    set_str(&it->cookie_file, cfg->cookie_file);
    it->mode = cfg->force_ytdlp ? MODE_YTDLP : cfg->force_direct ? MODE_DIRECT : MODE_AUTO;
    it->conns = clampi(cfg->connections, 1, UL_MAX_CONN);
    it->quality = cfg->max_height > 0 ? cfg->max_height : 0;
    link_seen(url, 1);
    snap_update(it);
    add_items(&it, 1, 1);
    schedule();
    cfg_free(cfg);
    free(url);
    return 1;
}

static void add_plain_links(char **links, int n)
{
    item **items = xcalloc((size_t)n + 1, sizeof *items);
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (!links[i]) continue;               /* already handed to another window */
        items[k] = item_new(links[i]);
        link_seen(links[i], 1);
        snap_update(items[k]);
        k++;
    }
    if (k) add_items(items, k, 1);
    free(items);
    schedule();
}

static LRESULT on_copydata(const COPYDATASTRUCT *cds)
{
    if (!cds || cds->dwData != UL_COPYDATA_JOB || !cds->lpData || cds->cbData < 2 || cds->cbData > 32768 ||
        g_exiting)
        return FALSE;
    char *path = xstrndup(cds->lpData, cds->cbData);
    int ok = accept_job(path);
    free(path);
    if (ok) show_main();
    /* 1 = taken, 2 = refused (unreadable job: retrying cannot help);
       0 (above) = closing, the sender should take over */
    return ok ? 1 : 2;
}

/* ------------------------------------------------------------------ */
/* drag and drop                                                       */
/* ------------------------------------------------------------------ */
typedef struct {
    IDropTarget iface;
    LONG ref;
    int ok;
} droptarget;

static droptarget g_drop;
static UINT g_cf_url;

static char *hdrop_torrents(HDROP h)
{
    sbuf b = { 0 };
    UINT n = DragQueryFileW(h, 0xFFFFFFFF, NULL, 0);
    for (UINT i = 0; i < n; i++) {
        UINT len = DragQueryFileW(h, i, NULL, 0);
        wchar_t *w = xcalloc((size_t)len + 1, sizeof(wchar_t));
        DragQueryFileW(h, i, w, len + 1);
        char *u = w_to_u8(w);
        free(w);
        if (str_iends(u, ".torrent")) {
            if (b.len) sb_putc(&b, '\n');
            sb_puts(&b, u);
        }
        free(u);
    }
    if (!b.len) { sb_free(&b); return NULL; }
    return sb_steal(&b);
}

static char *data_links(IDataObject *obj)
{
    FORMATETC f;
    STGMEDIUM sm;
    f.ptd = NULL;
    f.dwAspect = DVASPECT_CONTENT;
    f.lindex = -1;
    f.tymed = TYMED_HGLOBAL;
    char *res = NULL;

    f.cfFormat = CF_HDROP;
    if (obj->lpVtbl->GetData(obj, &f, &sm) == S_OK) {
        res = hdrop_torrents((HDROP)sm.hGlobal);
        ReleaseStgMedium(&sm);
        if (res) return res;
    }
    UINT fmts[2] = { g_cf_url, CF_UNICODETEXT };
    for (int k = 0; k < 2 && !res; k++) {
        if (!fmts[k]) continue;
        f.cfFormat = (CLIPFORMAT)fmts[k];
        if (obj->lpVtbl->GetData(obj, &f, &sm) != S_OK) continue;
        const wchar_t *w = GlobalLock(sm.hGlobal);
        if (w) {
            size_t max = GlobalSize(sm.hGlobal) / sizeof(wchar_t), n = 0;
            while (n < max && n < 262144 && w[n]) n++;
            wchar_t *c = xcalloc(n + 1, sizeof(wchar_t));
            memcpy(c, w, n * sizeof(wchar_t));
            GlobalUnlock(sm.hGlobal);
            char *u = w_to_u8(c);
            free(c);
            res = extract_links(u);
            free(u);
        }
        ReleaseStgMedium(&sm);
    }
    return res;
}

static int data_acceptable(IDataObject *obj)
{
    FORMATETC f;
    f.ptd = NULL;
    f.dwAspect = DVASPECT_CONTENT;
    f.lindex = -1;
    f.tymed = TYMED_HGLOBAL;
    UINT fmts[3] = { CF_HDROP, g_cf_url, CF_UNICODETEXT };
    for (int i = 0; i < 3; i++) {
        if (!fmts[i]) continue;
        f.cfFormat = (CLIPFORMAT)fmts[i];
        if (obj->lpVtbl->QueryGetData(obj, &f) == S_OK) return 1;
    }
    return 0;
}

static DWORD drop_effect(DWORD allowed)
{
    return (allowed & DROPEFFECT_COPY) ? DROPEFFECT_COPY : (allowed & DROPEFFECT_LINK) ? DROPEFFECT_LINK : DROPEFFECT_NONE;
}

static HRESULT STDMETHODCALLTYPE dt_QueryInterface(IDropTarget *This, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown_) || IsEqualIID(riid, &IID_IDropTarget_)) {
        *ppv = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE dt_AddRef(IDropTarget *This)
{
    return (ULONG)InterlockedIncrement(&((droptarget *)This)->ref);
}

static ULONG STDMETHODCALLTYPE dt_Release(IDropTarget *This)
{
    return (ULONG)InterlockedDecrement(&((droptarget *)This)->ref);    /* static object */
}

static HRESULT STDMETHODCALLTYPE dt_DragEnter(IDropTarget *This, IDataObject *obj, DWORD keys, POINTL pt, DWORD *eff)
{
    droptarget *d = (droptarget *)This;
    d->ok = !g_exiting && !g_modal && data_acceptable(obj);
    *eff = d->ok ? drop_effect(*eff) : DROPEFFECT_NONE;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dt_DragOver(IDropTarget *This, DWORD keys, POINTL pt, DWORD *eff)
{
    *eff = ((droptarget *)This)->ok ? drop_effect(*eff) : DROPEFFECT_NONE;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dt_DragLeave(IDropTarget *This)
{
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dt_Drop(IDropTarget *This, IDataObject *obj, DWORD keys, POINTL pt, DWORD *eff)
{
    char *links = (((droptarget *)This)->ok) ? data_links(obj) : NULL;
    *eff = links ? drop_effect(*eff) : DROPEFFECT_NONE;
    /* the dialog opens after the drag source has returned */
    if (links && !PostMessageW(g_hwnd, WM_APP_DROP, 0, (LPARAM)links)) free(links);
    return S_OK;
}

static IDropTargetVtbl g_drop_vtbl = {
    dt_QueryInterface, dt_AddRef, dt_Release, dt_DragEnter, dt_DragOver, dt_DragLeave, dt_Drop
};

/* ------------------------------------------------------------------ */
/* menus                                                               */
/* ------------------------------------------------------------------ */
static void list_menu(LPARAM lp)
{
    POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    if (pt.x == -1 && pt.y == -1) {
        int i = ListView_GetNextItem(g_lv, -1, LVNI_FOCUSED);
        RECT r = { 0, 0, 0, 0 };
        if (i >= 0) ListView_GetItemRect(g_lv, i, &r, LVIR_LABEL);
        pt.x = r.left + S(8);
        pt.y = r.bottom;
        ClientToScreen(g_lv, &pt);
    } else {
        LVHITTESTINFO ht;
        memset(&ht, 0, sizeof ht);
        ht.pt = pt;
        ScreenToClient(g_lv, &ht.pt);
        if (ListView_SubItemHitTest(g_lv, &ht) < 0 || ht.iItem < 0) {
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, IDM_ADD, L"&Add link...\tCtrl+N");
            AppendMenuW(m, MF_STRING, IDM_PASTE, L"&Paste links\tCtrl+V");
            AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            AppendMenuW(m, MF_STRING, IDM_RESUMEALL, L"&Resume all");
            AppendMenuW(m, MF_STRING, IDM_PAUSEALL, L"Pa&use all");
            TrackPopupMenuEx(m, TPM_RIGHTBUTTON, pt.x, pt.y, g_hwnd, NULL);
            DestroyMenu(m);
            return;
        }
    }
    item **sel;
    int n = selection(&sel);
    if (!n) { free(sel); return; }
    item *f = focused_item();
    if (!f) f = sel[0];
    int done = 0, can_pause = 0, can_resume = 0;
    for (int k = 0; k < n; k++) {
        if (sel[k]->state == TS_DONE) done = 1;
        if ((sel[k]->thread && !sel[k]->t->abort) || sel[k]->state == TS_QUEUED) can_pause = 1;
        if ((!sel[k]->thread && (sel[k]->state == TS_PAUSED || sel[k]->state == TS_ERROR)) ||
            (sel[k]->thread && sel[k]->t->abort)) can_resume = 1;
    }
    free(sel);
    int pause_first = (f->thread && !f->t->abort) || f->state == TS_QUEUED;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (done ? 0 : MF_GRAYED), IDM_OPEN, L"&Open\tEnter");
    AppendMenuW(m, MF_STRING, IDM_OPENFOLDER, L"Open &folder");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    if (pause_first || (!can_resume && can_pause))
        AppendMenuW(m, MF_STRING | (can_pause ? 0 : MF_GRAYED), IDM_PAUSE, L"&Pause");
    else
        AppendMenuW(m, MF_STRING | (can_resume ? 0 : MF_GRAYED), IDM_RESUME, L"&Resume");
    AppendMenuW(m, MF_STRING, IDM_COPYLINK, L"&Copy link");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_DELETE, L"&Delete\tDel");
    AppendMenuW(m, MF_STRING, IDM_DELETEFILE, L"Delete with &file\tShift+Del");
    if (done) SetMenuDefaultItem(m, IDM_OPEN, FALSE);
    TrackPopupMenuEx(m, TPM_RIGHTBUTTON, pt.x, pt.y, g_hwnd, NULL);
    DestroyMenu(m);
}

static void tray_menu(int x, int y)
{
    int any = 0;
    for (int i = 0; i < g_n && !any; i++)
        if ((g_items[i]->thread && !g_items[i]->t->abort) || g_items[i]->state == TS_QUEUED) any = 1;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_SHOW, L"&Show U-loader");
    AppendMenuW(m, MF_STRING | (g_modal ? MF_GRAYED : 0), IDM_ADD, L"&Add link...");
    AppendMenuW(m, MF_STRING | (any ? 0 : MF_GRAYED), IDM_PAUSEALL, L"&Pause all");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"E&xit");
    SetMenuDefaultItem(m, IDM_SHOW, FALSE);
    SetForegroundWindow(g_hwnd);
    UINT flags = TPM_RIGHTBUTTON | (GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN);
    TrackPopupMenuEx(m, flags, x, y, g_hwnd, NULL);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static void tray_event(WPARAM wp, LPARAM lp)
{
    switch (LOWORD(lp)) {
    case NIN_SELECT:
    case NIN_KEYSELECT:
    case WM_LBUTTONDBLCLK:
        show_main();
        break;
    case WM_CONTEXTMENU:
        tray_menu(GET_X_LPARAM(wp), GET_Y_LPARAM(wp));
        break;
    case NIN_BALLOONUSERCLICK:
        show_main();
        if (g_notify_item && index_of(g_notify_item) >= 0) select_items(&g_notify_item, 1, g_notify_item);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* commands                                                            */
/* ------------------------------------------------------------------ */
static void on_command(int id, int code)
{
    trace("command %d code %d", id, code);
    item **sel;
    int n;
    switch (id) {
    case IDM_ADD:
        if (g_modal) { if (g_dlg) SetForegroundWindow(g_dlg); break; }
        if (!IsWindowVisible(g_hwnd)) show_main();
        open_add_dialog(NULL, 0);
        break;
    case IDM_PASTE:
        paste_links();
        break;
    case IDM_RESUME:
        n = selection(&sel);
        for (int k = 0; k < n; k++) resume_item(sel[k]);
        free(sel);
        schedule();
        list_changed();
        break;
    case IDM_RESUMEALL:
        for (int i = 0; i < g_n; i++) resume_item(g_items[i]);
        schedule();
        list_changed();
        break;
    case IDM_PAUSE:
        n = selection(&sel);
        for (int k = 0; k < n; k++) pause_item(sel[k]);
        free(sel);
        list_changed();
        schedule();
        break;
    case IDM_PAUSEALL:
        for (int i = 0; i < g_n; i++) pause_item(g_items[i]);
        list_changed();
        break;
    case IDM_DELETE:
        if (code == 1 && GetFocus() != g_lv) break;      /* accelerator outside the list */
        delete_selected(0);
        break;
    case IDM_DELETEFILE:
        if (code == 1 && GetFocus() != g_lv) break;
        delete_selected(1);
        break;
    case IDM_OPEN: {
        /* "Open" is offered when any selected download is finished: open
           the focused one if it is, else the first finished one */
        item *o = focused_item();
        if (!o || o->state != TS_DONE) {
            item **sel;
            int n = selection(&sel);
            o = NULL;
            for (int k = 0; k < n && !o; k++) if (sel[k]->state == TS_DONE) o = sel[k];
            free(sel);
        }
        open_item(o);
        break;
    }
    case IDM_OPENFOLDER:
        open_folder_of(focused_item());
        break;
    case IDM_SETTINGS:
        open_settings();
        break;
    case IDM_COPYLINK:
        copy_links();
        break;
    case IDM_SELECTALL:
        if (GetFocus() == g_log) SendMessageW(g_log, LB_SELITEMRANGEEX, 0, (LPARAM)SendMessageW(g_log, LB_GETCOUNT, 0, 0) - 1);
        else ListView_SetItemState(g_lv, -1, LVIS_SELECTED, LVIS_SELECTED);
        break;
    case IDM_SHOW:
        show_main();
        break;
    case IDM_EXIT:
        do_exit();
        break;
    default:
        return;
    }
    update_toolbar();
    update_status();
}

static LRESULT on_notify(NMHDR *h)
{
    /* the user dragged (or double-clicked) a column divider */
    if (g_lv && h->hwndFrom == ListView_GetHeader(g_lv) &&
        (h->code == HDN_ENDTRACKW || h->code == HDN_DIVIDERDBLCLICKW)) {
        PostMessageW(g_hwnd, WM_APP_COLSIZED, (WPARAM)((NMHEADERW *)h)->iItem, 0);
        return 0;
    }
    if (h->hwndFrom == g_lv) {
        switch (h->code) {
        case LVN_GETDISPINFOW:
            list_dispinfo((NMLVDISPINFOW *)h);
            return 0;
        case NM_CUSTOMDRAW:
            return list_customdraw((NMLVCUSTOMDRAW *)h);
        case LVN_ITEMCHANGED:
        case LVN_ODSTATECHANGED:
            if (!g_sel_posted) {
                g_sel_posted = 1;
                PostMessageW(g_hwnd, WM_APP_SELCHANGE, 0, 0);
            }
            return 0;
        case NM_DBLCLK: {
            NMITEMACTIVATE *a = (NMITEMACTIVATE *)h;
            if (a->iItem >= 0 && a->iItem < g_n) activate(g_items[a->iItem]);
            return 0;
        }
        case NM_RETURN:
            activate(focused_item());
            return 0;
        case LVN_KEYDOWN: {
            NMLVKEYDOWN *k = (NMLVKEYDOWN *)h;
            if (k->wVKey == VK_DELETE) PostMessageW(g_hwnd, WM_COMMAND, GetKeyState(VK_SHIFT) < 0 ? IDM_DELETEFILE : IDM_DELETE, 0);
            return 0;
        }
        case LVN_COLUMNCLICK:
            sort_by(((NMLISTVIEW *)h)->iSubItem);
            return 0;
        case LVN_GETEMPTYMARKUP: {
            NMLVEMPTYMARKUP *em = (NMLVEMPTYMARKUP *)h;
            em->dwFlags = EMF_CENTERED;
            lstrcpynW(em->szMarkup, L"No downloads yet.\nPress Ctrl+N to add a link, copy a download link, "
                                    L"or send one from your browser.", L_MAX_URL_LENGTH);
            return TRUE;
        }
        case LVN_ODFINDITEMW: {
            NMLVFINDITEMW *f = (NMLVFINDITEMW *)h;
            if (!(f->lvfi.flags & (LVFI_STRING | LVFI_PARTIAL)) || !f->lvfi.psz || !g_n) return -1;
            char *needle = w_to_u8(f->lvfi.psz);
            size_t nl = strlen(needle);
            int start = f->iStart >= 0 && f->iStart < g_n ? f->iStart : 0;
            for (int k = 0; k < g_n; k++) {
                int i = (start + k) % g_n;
                if (_strnicmp(g_items[i]->name, needle, nl) == 0) { free(needle); return i; }
            }
            free(needle);
            return -1;
        }
        default:
            return 0;
        }
    }
    if (h->hwndFrom == g_tb && h->code == TBN_GETINFOTIPW) {
        NMTBGETINFOTIPW *tip = (NMTBGETINFOTIPW *)h;
        for (int i = 0; i < NTOOLS; i++)
            if (TOOLS[i].cmd && TOOLS[i].cmd == tip->iItem) lstrcpynW(tip->pszText, TOOLS[i].tip, tip->cchTextMax);
        return 0;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* DPI change                                                          */
/* ------------------------------------------------------------------ */
static void apply_dpi(UINT dpi, const RECT *suggested)
{
    UINT old = g_dpi;
    g_dpi = dpi;
    HFONT oldf[4];
    make_fonts(oldf);
    SendMessageW(g_lv, WM_SETFONT, (WPARAM)g_font, FALSE);
    SendMessageW(g_sb, WM_SETFONT, (WPARAM)g_font, FALSE);
    SendMessageW(g_log, WM_SETFONT, (WPARAM)g_font_mono, FALSE);
    SendMessageW(g_log, LB_SETITEMHEIGHT, 0, (LPARAM)log_item_h());
    if (old != dpi)
        for (int i = 0; i < NCOLS; i++) {
            int w = ListView_GetColumnWidth(g_lv, i);
            ListView_SetColumnWidth(g_lv, i, MulDiv(w, (int)dpi, (int)old));
        }
    create_toolbar();
    make_file_imagelist();
    delete_fonts(oldf);
    set_window_icons();
    int tracking = g_list_w >= 0;
    g_list_w = -1;                     /* the columns were scaled above: no width stretching now */
    if (suggested)
        SetWindowPos(g_hwnd, NULL, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
    layout();
    if (tracking) {
        RECT cr;
        GetClientRect(g_hwnd, &cr);
        g_list_w = cr.right;
    }
    detail_layout();
    update_toolbar();
    InvalidateRect(g_hwnd, NULL, TRUE);
    InvalidateRect(g_lv, NULL, TRUE);
    InvalidateRect(g_detail, NULL, TRUE);
}

/* ------------------------------------------------------------------ */
/* exit                                                                */
/* ------------------------------------------------------------------ */
static void stop_all_and_wait(DWORD ms)
{
    for (int i = 0; i < g_n; i++) if (g_items[i]->thread) g_items[i]->t->abort = 1;
    for (int i = 0; i < g_ngone; i++) if (g_gone[i]->thread) g_gone[i]->t->abort = 1;
    if (g_yt_task) g_yt_task->abort = 1;
    InterlockedExchange(&g_abort, 1);     /* also stops helper processes (run_process) */
    ULONGLONG deadline = GetTickCount64() + ms;
    for (;;) {
        HANDLE hs[MAXIMUM_WAIT_OBJECTS];
        DWORD n = 0;
        for (int i = 0; i < g_n && n < MAXIMUM_WAIT_OBJECTS; i++)
            if (g_items[i]->thread && WaitForSingleObject(g_items[i]->thread, 0) == WAIT_TIMEOUT) hs[n++] = g_items[i]->thread;
        for (int i = 0; i < g_ngone && n < MAXIMUM_WAIT_OBJECTS; i++)
            if (g_gone[i]->thread && WaitForSingleObject(g_gone[i]->thread, 0) == WAIT_TIMEOUT) hs[n++] = g_gone[i]->thread;
        if (g_yt_thread && n < MAXIMUM_WAIT_OBJECTS && WaitForSingleObject(g_yt_thread, 0) == WAIT_TIMEOUT) hs[n++] = g_yt_thread;
        if (g_connect_thread && n < MAXIMUM_WAIT_OBJECTS && WaitForSingleObject(g_connect_thread, 0) == WAIT_TIMEOUT)
            hs[n++] = g_connect_thread;
        ULONGLONG now = GetTickCount64();
        if (!n || now >= deadline) break;
        WaitForMultipleObjects(n, hs, TRUE, (DWORD)(deadline - now));
    }
    /* collect what finished (no notifications while exiting) */
    for (int i = 0; i < g_n; i++)
        if (g_items[i]->thread && WaitForSingleObject(g_items[i]->thread, 0) == WAIT_OBJECT_0) task_finished(g_items[i]);
    for (int i = 0; i < g_ngone; i++)
        if (g_gone[i]->thread && WaitForSingleObject(g_gone[i]->thread, 0) == WAIT_OBJECT_0) task_finished(g_gone[i]);
}

static void do_exit(void)
{
    if (g_exiting) return;
    if (g_modal) {
        g_exit_pending = 1;
        if (g_dlg) PostMessageW(g_dlg, WM_COMMAND, IDCANCEL, 0);
        return;
    }
    capture_layout();
    g_exiting = 1;
    KillTimer(g_hwnd, TIMER_TICK);
    KillTimer(g_hwnd, TIMER_SAVE);
    ShowWindow(g_hwnd, SW_HIDE);
    tray_remove();
    clip_listen(0);
    RevokeDragDrop(g_hwnd);
    stop_all_and_wait(5000);
    save_list();
    settings_save();
    DestroyWindow(g_hwnd);
}

/* ------------------------------------------------------------------ */
/* main window                                                         */
/* ------------------------------------------------------------------ */
static void on_create(HWND hwnd)
{
    g_hwnd = hwnd;
    g_dpi = window_dpi(hwnd);
    HFONT none[4];
    make_fonts(none);
    g_have_mdl2 = font_exists(L"Segoe MDL2 Assets");
    set_window_icons();
    create_list();
    g_sb = CreateWindowExW(0, STATUSCLASSNAMEW, NULL, WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                           0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_STATUS, g_inst, NULL);
    SendMessageW(g_sb, WM_SETFONT, (WPARAM)g_font, FALSE);
    g_detail = CreateWindowExW(0, DETAIL_CLASS, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                               0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_DETAIL, g_inst, NULL);
    create_toolbar();
    layout();

    int tray = GetSystemMetrics(SM_CXSMICON);
    g_icon_tray = LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, tray, tray, 0);
    int big = GetSystemMetrics(SM_CXICON);
    g_icon_balloon = LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, big, big, 0);
    g_wm_taskbar = RegisterWindowMessageW(L"TaskbarCreated");
    tray_add();
    clip_listen(g_set.clip);

    g_cf_url = RegisterClipboardFormatW(L"UniformResourceLocatorW");
    g_drop.iface.lpVtbl = &g_drop_vtbl;
    g_drop.ref = 1;
    if (RegisterDragDrop(hwnd, &g_drop.iface) != S_OK) DragAcceptFiles(hwnd, TRUE);

    SetTimer(hwnd, TIMER_TICK, TICK_MS, NULL);
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        on_create(hwnd);
        return 0;
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) {
            if (g_set.tray && !g_exiting) hide_to_tray();
            return 0;
        }
        layout();
        return 0;
    case WM_GETMINMAXINFO: {
        /* small enough to tuck away, but never so small that the toolbar,
           a few list rows and the whole detail block do not fit */
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        SIZE ts = { 0, 0 };                     /* every toolbar button must stay visible */
        if (g_tb) SendMessageW(g_tb, TB_GETIDEALSIZE, FALSE, (LPARAM)&ts);
        int minw = ts.cx + S(20) > S(560) ? ts.cx + S(20) : S(560);
        RECT need = { 0, 0, minw, (g_tb_area ? g_tb_area : S(44)) + 1 + S(96) + split_h() +
                                  info_height() + S(26) };
        AdjustWindowRectEx(&need, WS_OVERLAPPEDWINDOW, FALSE, 0);
        mm->ptMinTrackSize.x = need.right - need.left;
        mm->ptMinTrackSize.y = need.bottom - need.top;
        return 0;
    }
    case WM_DPICHANGED:
        trace("WM_DPICHANGED %u -> %u", g_dpi, (unsigned)HIWORD(wp));
        if (!g_dpi_override) apply_dpi(HIWORD(wp), (const RECT *)lp);
        return 0;
    case WM_SETTINGCHANGE:
        if (wp == SPI_SETNONCLIENTMETRICS && g_lv) apply_dpi(g_dpi, NULL);
        break;
    case WM_ERASEBKGND:
        paint_bg((HDC)wp);
        return 1;
    case WM_PRINTCLIENT:
        paint_bg((HDC)wp);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint_bg(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && (HWND)wp == hwnd) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (PtInRect(&g_split_rc, pt) || g_split_drag) {
                SetCursor(LoadCursorW(NULL, IDC_SIZENS));
                return TRUE;
            }
        }
        break;
    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (PtInRect(&g_split_rc, pt)) {
            g_split_drag = 1;
            g_split_grab = pt.y - g_split_rc.top;
            SetCapture(hwnd);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_split_drag) {
            RECT rc, sr;
            GetClientRect(hwnd, &rc);
            GetWindowRect(g_sb, &sr);
            int bottom = rc.bottom - (sr.bottom - sr.top);
            int y = GET_Y_LPARAM(lp) - g_split_grab;
            int dh = bottom - y - split_h();
            int dmax = bottom - split_h() - S(60);          /* the list keeps some rows */
            if (dh > dmax) dh = dmax;
            if (dh < info_height()) dh = info_height();     /* the details stay whole */
            g_set.detail_h = MulDiv(dh, 96, (int)g_dpi);
            layout();
            UpdateWindow(hwnd);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_split_drag) { g_split_drag = 0; ReleaseCapture(); }
        return 0;
    case WM_CAPTURECHANGED:
        g_split_drag = 0;
        return 0;
    case WM_SETFOCUS:
        if (g_lv) SetFocus(g_lv);
        return 0;
    case WM_NOTIFY:
        return on_notify((NMHDR *)lp);
    case WM_APP_COLSIZED: {
        /* remember the width the user picked; the Name column re-fills the rest */
        int col = (int)wp;
        if (col > COL_NAME && col < NCOLS) {
            int cw = ListView_GetColumnWidth(g_lv, col);
            g_set.colw[col] = cw > 0 ? MulDiv(cw, 96, (int)g_dpi) : COL_DEFW[col];
            if (g_set.colw[col] < 24) g_set.colw[col] = 24;
        }
        fit_columns();
        return 0;
    }
    case WM_COMMAND:
        on_command(LOWORD(wp), HIWORD(wp));
        return 0;
    case WM_CONTEXTMENU:
        if ((HWND)wp == g_lv) { list_menu(lp); return 0; }
        break;
    case WM_TIMER:
        if (wp == TIMER_TICK) on_tick();
        else if (wp == TIMER_SAVE) save_list();
        return 0;
    case WM_APP_TASKDONE:
        task_finished((item *)lp);
        update_toolbar();
        update_status();
        return 0;
    case WM_APP_SELCHANGE:
        g_sel_posted = 0;
        detail_refresh(0);
        update_toolbar();
        return 0;
    case WM_APP_TRAY:
        tray_event(wp, lp);
        return 0;
    case WM_APP_CLIPLINK: {
        char *link = (char *)lp;
        if (link_seen(link, 0)) { free(link); return 0; }   /* posted twice meanwhile */
        if (g_add_dlg) { add_dialog_append(link); link_seen(link, 1); }
        else if (!g_modal && !g_exiting) { link_seen(link, 1); open_add_dialog(link, 1); }
        free(link);
        return 0;
    }
    case WM_APP_DROP: {
        char *links = (char *)lp;
        if (g_add_dlg) add_dialog_append(links);
        else if (!g_modal && !g_exiting) open_add_dialog(links, 0);
        free(links);
        return 0;
    }
    case WM_APP_SHOW:
        show_main();
        return 0;
    case WM_APP_TESTDPI:
        if (g_dpi_override && wp >= 72 && wp <= 480 && wp != g_dpi) {
            RECT r;
            GetWindowRect(hwnd, &r);
            r.right = r.left + MulDiv(r.right - r.left, (int)wp, (int)g_dpi);
            r.bottom = r.top + MulDiv(r.bottom - r.top, (int)wp, (int)g_dpi);
            g_dpi_override = (UINT)wp;
            apply_dpi((UINT)wp, &r);
        }
        return 0;
    case WM_COPYDATA:
        return on_copydata((const COPYDATASTRUCT *)lp);
    case WM_CLIPBOARDUPDATE:
        on_clipboard_update();
        return 0;
    case WM_DROPFILES: {
        char *t = hdrop_torrents((HDROP)wp);
        DragFinish((HDROP)wp);
        if (t) open_add_dialog(t, 0);
        free(t);
        return 0;
    }
    case WM_CLOSE:
        if (g_set.tray && !g_exiting) { hide_to_tray(); return 0; }
        do_exit();
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wp && !g_exiting) {
            capture_layout();
            g_exiting = 1;
            tray_remove();
            stop_all_and_wait(3000);
            save_list();
            settings_save();
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        if (g_wm_taskbar && msg == g_wm_taskbar) {
            g_tray_added = 0;
            if (!g_exiting) tray_add();
            return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* single instance                                                     */
/* ------------------------------------------------------------------ */
static int send_job(HWND hw, const char *jobpath)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hw, &pid);
    AllowSetForegroundWindow(pid);
    COPYDATASTRUCT cds;
    cds.dwData = UL_COPYDATA_JOB;
    cds.cbData = (DWORD)strlen(jobpath) + 1;
    cds.lpData = (void *)jobpath;
    DWORD_PTR res = 0;
    if (!SendMessageTimeoutW(hw, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG, 8000, &res)) return 0;
    return res == 1 ? 1 : res == 2 ? 2 : 0;   /* see on_copydata */
}

/* Hand the job / links to the running window. Whatever it answered for
   is cleared (*job freed, links[i] freed and NULL), so after a hand-over
   that broke off half-way (the window was closing) only the rest is added
   here - never twice. Returns 1 when nothing is left to do. */
static int forward_to(HWND hw, char **job, char **links, int n)
{
    int left = 0;
    if (!*job && n == 0) {                   /* just "show yourself" */
        DWORD pid = 0;
        GetWindowThreadProcessId(hw, &pid);
        AllowSetForegroundWindow(pid);
        return PostMessageW(hw, WM_APP_SHOW, 0, 0) != 0;
    }
    if (*job) {
        if (send_job(hw, *job)) { free(*job); *job = NULL; }   /* taken, or refused for good */
        else left = 1;
    }
    for (int i = 0; i < n; i++) {
        if (!links[i]) continue;
        char *jp = job_write(links[i], NULL, NULL, NULL, NULL, NULL, 0, 0, NULL);
        int r = jp ? send_job(hw, jp) : 0;
        if (jp && r != 1) file_delete(jp);
        free(jp);
        if (r) { free(links[i]); links[i] = NULL; }
        else left = 1;
    }
    return !left;
}

static void place_window(int start_hidden)
{
    int shown = 0;
    if (g_set.has_place) {
        RECT r = { g_set.pl, g_set.pt, g_set.pr, g_set.pb };
        if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
            WINDOWPLACEMENT wp;
            memset(&wp, 0, sizeof wp);
            wp.length = sizeof wp;
            wp.showCmd = SW_HIDE;
            wp.rcNormalPosition = r;
            SetWindowPlacement(g_hwnd, &wp);
            shown = 1;
        }
    }
    if (!shown) {
        HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi;
        memset(&mi, 0, sizeof mi);
        mi.cbSize = sizeof mi;
        GetMonitorInfoW(mon, &mi);
        int ww = mi.rcWork.right - mi.rcWork.left, wh = mi.rcWork.bottom - mi.rcWork.top;
        int w = S(1060), h = S(680);
        if (w > ww * 9 / 10) w = ww * 9 / 10;
        if (h > wh * 9 / 10) h = wh * 9 / 10;
        SetWindowPos(g_hwnd, NULL, mi.rcWork.left + (ww - w) / 2, mi.rcWork.top + (wh - h) / 2, w, h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    RECT cr;
    GetClientRect(g_hwnd, &cr);
    fit_columns();                    /* the columns always fill the list exactly */
    g_list_w = cr.right;
    if (!start_hidden) {
        ShowWindow(g_hwnd, g_set.pmax ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
        UpdateWindow(g_hwnd);
        SetFocus(g_lv);
    } else if (g_set.pmax) {
        ShowWindow(g_hwnd, SW_MAXIMIZE);
        ShowWindow(g_hwnd, SW_HIDE);
    }
}

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE prev, PWSTR cmdline, int show)
{
    g_inst = hinst;
    load_dpi_api();
    wchar_t ov[16];
    if (GetEnvironmentVariableW(L"ULOADER_TEST_DPI", ov, 16)) {
        int v = _wtoi(ov);
        if (v >= 72 && v <= 480) g_dpi_override = (UINT)v;
    }
    InitializeCriticalSection(&g_help_lock);
    wchar_t tr[MAX_PATH];
    DWORD trn = GetEnvironmentVariableW(L"ULOADER_TRACE", tr, MAX_PATH);
    if (trn && trn < MAX_PATH) g_trace = _wfopen(tr, L"a");

    /* command line */
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    char *job = NULL, **links = NULL;
    int nlinks = 0, start_hidden = 0, only_links = 0;
    for (int i = 1; argv && i < argc; i++) {
        char *a = w_to_u8(argv[i]);
        if (!only_links && !strcmp(a, "--")) only_links = 1;
        else if (!only_links && !strcmp(a, "--job") && i + 1 < argc) { free(job); job = w_to_u8(argv[++i]); }
        else if (!only_links && !strcmp(a, "--tray")) start_hidden = 1;
        else if (!only_links && a[0] == '-' && a[1] == '-') { /* unknown option: ignore */ }
        else if (*a) {
            char *l = xstrdup(a);
            if (!strstr(l, "://") && file_exists(l)) {          /* a .torrent file: make it absolute */
                wchar_t full[MAX_PATH * 4];
                if (GetFullPathNameW(argv[i], MAX_PATH * 4, full, NULL)) { free(l); l = w_to_u8(full); }
            }
            links = xrealloc(links, sizeof *links * (size_t)(nlinks + 1));
            links[nlinks++] = l;
        }
        free(a);
    }
    if (argv) LocalFree(argv);
    if (job) {
        wchar_t *wj = u8_to_w(job), full[MAX_PATH * 4];
        if (GetFullPathNameW(wj, MAX_PATH * 4, full, NULL)) { free(job); job = w_to_u8(full); }
        free(wj);
    }
    {   /* do not keep the launching folder busy for as long as we run */
        char *ed = exe_dir();
        wchar_t *we = u8_to_w(ed);
        SetCurrentDirectoryW(we);
        free(we);
        free(ed);
    }

    /* one window per user session */
    HANDLE mtx = CreateMutexW(NULL, FALSE, L"Local\\ULoaderWindowSingleInstance");
    /* owned only when the wait says so (WAIT_ABANDONED: its owner died -
       ours now); after a timeout it must not be released */
    int owned = mtx && WaitForSingleObject(mtx, 15000) != WAIT_TIMEOUT;
    HWND other = FindWindowW(UL_GUI_CLASS, NULL);
    if (other) {
        int done = forward_to(other, &job, links, nlinks);
        if (owned) ReleaseMutex(mtx);
        owned = 0;
        if (done) return 0;
        /* the other window is closing: wait for it, then take over what it did not take */
        for (int k = 0; k < 80 && IsWindow(other); k++) Sleep(100);
        owned = mtx && WaitForSingleObject(mtx, 15000) != WAIT_TIMEOUT;
        if (FindWindowW(UL_GUI_CLASS, NULL)) {
            if (owned) ReleaseMutex(mtx);
            return 1;
        }
    }

    OleInitialize(NULL);
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_UPDOWN_CLASS | ICC_LINK_CLASS | ICC_STANDARD_CLASSES |
                ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);
    settings_load();

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = main_proc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = UL_GUI_CLASS;
    wc.hIcon = LoadImageW(hinst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                          GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = LoadImageW(hinst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                            GetSystemMetrics(SM_CYSMICON), 0);
    RegisterClassExW(&wc);

    WNDCLASSEXW dc;
    memset(&dc, 0, sizeof dc);
    dc.cbSize = sizeof dc;
    dc.lpfnWndProc = detail_proc;
    dc.hInstance = hinst;
    dc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    dc.lpszClassName = DETAIL_CLASS;
    RegisterClassExW(&dc);

    g_hwnd = CreateWindowExW(0, UL_GUI_CLASS, L"U-loader", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1000, 650, NULL, NULL, hinst, NULL);
    if (!g_hwnd) {
        if (owned) ReleaseMutex(mtx);
        MessageBoxW(NULL, L"Could not create the U-loader window.", L"U-loader", MB_OK | MB_ICONERROR);
        return 1;
    }
    load_list();
    list_changed();                  /* rewrite it as shown (running/queued came back paused) */
    g_last_periodic_save = now_sec();
    ListView_SetItemCountEx(g_lv, g_n, 0);
    for (int i = 0; i < g_n; i++) snap_update(g_items[i]);
    place_window(start_hidden);
    if (owned) ReleaseMutex(mtx);

    ACCEL acc[] = {
        { FCONTROL | FVIRTKEY, 'N', IDM_ADD },
        { FCONTROL | FVIRTKEY, 'V', IDM_PASTE },
        { FSHIFT | FVIRTKEY, VK_INSERT, IDM_PASTE },
        { FCONTROL | FVIRTKEY, 'A', IDM_SELECTALL },
    };
    g_accel = CreateAcceleratorTableW(acc, (int)(sizeof acc / sizeof *acc));

    if (job && !accept_job(job)) {
        modal_begin();
        MessageBoxW(g_hwnd, L"The download request could not be read (the job file is missing or invalid).",
                    L"U-loader", MB_OK | MB_ICONWARNING);
        modal_end();
    }
    if (nlinks) add_plain_links(links, nlinks);
    free_strv(links, nlinks);
    free(job);
    detail_refresh(1);
    update_toolbar();
    update_status();

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (g_accel && (m.hwnd == g_hwnd || IsChild(g_hwnd, m.hwnd)) && TranslateAcceleratorW(g_hwnd, g_accel, &m))
            continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    /* download threads that did not stop in time end with the process;
       a torrent's "stopped" to its trackers gets a moment to go out */
    torrent_flush_announces(2000);
    return 0;
}
