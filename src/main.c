/*
 * U-loader - command line front-end.
 *
 *   uloader [options] URL [URL...]
 *   uloader                 (interactive: paste links one after another)
 */
#include "uloader.h"
#include <shellapi.h>
#include <errno.h>

static void usage(void)
{
    printf(
        UL_NAME " " UL_VERSION " - fast multi-connection downloader\n"
        "\n"
        "Usage:  uloader [options] LINK [LINK...]\n"
        "        uloader                       interactive mode (paste links)\n"
        "        uloader-gui                   the U-loader window\n"
        "\n"
        "LINK can be an http/https/ftp file, an HLS stream (.m3u8), a video page\n"
        "(YouTube, Facebook, TikTok, X, Vimeo... via yt-dlp), a magnet: link or a\n"
        ".torrent file/URL.\n"
        "\n"
        "Options:\n"
        "  -c, --connections N   connections per file (1-32, default 8)\n"
        "  -d, --dir DIR         save into DIR (default: current folder)\n"
        "  -o, --output NAME     file name (only with a single link)\n"
        "  -i, --input FILE      read links from a text file (one per line) - a queue\n"
        "  -l, --limit RATE      speed limit, e.g. 500K or 2M (bytes per second)\n"
        "  -q, --quality H       max video height, e.g. 720 or 1080 (default: best)\n"
        "  -f, --format FMT      yt-dlp format selector (advanced)\n"
        "      --playlist        download every video of a playlist link\n"
        "  -H, --header \"K: V\"   add an HTTP header (repeatable)\n"
        "  -U, --user-agent UA   set the User-Agent\n"
        "  -r, --referer URL     set the Referer\n"
        "  -b, --cookie \"a=b\"    send cookies (only to the link's own site)\n"
        "      --cookies FILE    use a cookies.txt file (Netscape format)\n"
        "  -x, --proxy HOST:PORT use a proxy (default: Windows proxy settings)\n"
        "  -k, --insecure        do not verify TLS certificates\n"
        "  -R, --retries N       retries per connection (default 10)\n"
        "  -t, --timeout SEC     network timeout (default 30)\n"
        "      --overwrite       replace existing files instead of renaming\n"
        "      --direct          treat the link as a plain file (skip site detection)\n"
        "      --ytdlp           force the site extractor for this link\n"
        "      --ytdlp-args \"..\"  extra yt-dlp arguments\n"
        "      --get-ytdlp       install / update yt-dlp next to uloader.exe\n"
        "      --install         connect U-loader to Chrome / Edge / Brave\n"
        "      --uninstall       remove the browser connection\n"
        "  -h, --help            show this help\n"
        "  -v, --version         show the version\n"
        "\n"
        "Ctrl+C pauses a download; run the same command again to resume.\n");
}

static BOOL WINAPI ctrl_handler(DWORD type)
{
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        if (!g_busy || g_abort) return FALSE;   /* idle or 2nd Ctrl+C: quit */
        InterlockedExchange(&g_abort, 1);
        return TRUE;
    case CTRL_CLOSE_EVENT:
        InterlockedExchange(&g_abort, 1);
        Sleep(4000);                            /* let the state be saved */
        return FALSE;
    default:
        return FALSE;
    }
}

/* one link with its own copy of the settings */
static int run_one(const ul_config *base, const char *link, const char *out_name)
{
    ul_config *cfg = cfg_clone(base);
    if (out_name) { free(cfg->out_name); cfg->out_name = xstrdup(out_name); }
    ul_task *t = task_new(link);
    InterlockedExchange(&g_busy, 1);
    int rc = task_run(t, cfg);
    InterlockedExchange(&g_busy, 0);
    con_progress_end();
    task_free(t);
    cfg_free(cfg);
    return rc;
}

static void interactive(ul_config *cfg)
{
    ul_log(LOG_HEAD, "%s %s - fast multi-connection downloader", UL_NAME, UL_VERSION);
    ul_log(LOG_DIM, "Saving to: %s", cfg->out_dir);
    ul_log(LOG_DIM, "Connections per file: %d    Ctrl+C pauses a download.", cfg->connections);
    char *yt = find_tool("yt-dlp.exe");
    if (!yt) ul_log(LOG_DIM, "Tip: type  get-ytdlp  to enable YouTube/Facebook/TikTok/... downloads.");
    free(yt);
    for (;;) {
        printf("\nPaste a link (q = quit): ");
        fflush(stdout);
        char *line = con_read_line();
        if (!line) break;
        char *t = str_trim(line);
        if (!_stricmp(t, "q") || !_stricmp(t, "quit") || !_stricmp(t, "exit")) { free(line); break; }
        if (!_stricmp(t, "get-ytdlp") || !_stricmp(t, "--get-ytdlp")) {
            ul_task *tk = task_new("");
            http_session *s = http_session_new(cfg, tk);
            InterlockedExchange(&g_busy, 1);
            if (s) ytdlp_install(s, cfg);
            InterlockedExchange(&g_busy, 0);
            http_session_free(s);
            task_free(tk);
        } else {
            /* A path with spaces (C:\My Files\x.torrent, maybe in quotes,
               as Explorer's "Copy as path" gives it) is one item;
               otherwise several links may be pasted at once. */
            size_t tl = strlen(t);
            char *whole = (tl >= 2 && t[0] == '"' && t[tl - 1] == '"') ? xstrndup(t + 1, tl - 2) : xstrdup(t);
            if (strchr(whole, ' ') && file_exists(whole)) {
                run_one(cfg, whole, NULL);
            } else {
                char *p = t;
                while (*p && !g_abort) {
                    while (*p == ' ' || *p == '\t') p++;
                    if (!*p) break;
                    char *tok;
                    if (*p == '"') {                      /* "quoted item" */
                        tok = ++p;
                        while (*p && *p != '"') p++;
                    } else {
                        tok = p;
                        while (*p && *p != ' ' && *p != '\t') p++;
                    }
                    if (*p) *p++ = 0;
                    if (*tok) run_one(cfg, tok, NULL);
                }
            }
            free(whole);
        }
        InterlockedExchange(&g_abort, 0);
        free(line);
    }
}

/* Job from the browser (when the window is not available). */
static int run_job(ul_config *cfg, const char *job)
{
    char *url = NULL;
    int lr = job_load(job, cfg, &url);
    if (lr != 0) { ul_log(LOG_ERR, "invalid job file: %s", job); return 1; }   /* left untouched */
    file_delete(job);                       /* one-shot hand-over file */
    if (!cfg->out_dir) cfg->out_dir = downloads_dir();
    wchar_t *title = u8_to_w(url);
    SetConsoleTitleW(title);
    free(title);
    ul_log(LOG_HEAD, "%s %s", UL_NAME, UL_VERSION);
    int rc = run_one(cfg, url, NULL);
    if (job_cookie_file_is_temp(cfg->cookie_file)) file_delete(cfg->cookie_file);
    free(url);
    if (rc == 0) {
        ul_log(LOG_OK, "\nFinished. This window closes in 5 seconds.");
        Sleep(5000);
    } else {
        printf("\nPress Enter to close this window.");
        fflush(stdout);
        free(con_read_line());
    }
    return rc;
}

/* ------------------------------------------------------------------ */
static const char *need_value(int argc, char **argv, int *i, const char *inline_val, const char *opt)
{
    if (inline_val) return inline_val;
    if (*i + 1 >= argc) {
        ul_log(LOG_ERR, "option %s needs a value", opt);
        exit(1);
    }
    return argv[++*i];
}

/* a whole number, nothing else ("8x", "" or "abc" are mistakes, not 0) */
static int int_value(const char *v, const char *opt, long lo, long hi)
{
    char *e;
    errno = 0;
    long x = strtol(v, &e, 10);
    while (*e == ' ') e++;
    if (e == v || *e || errno == ERANGE || x < lo || x > hi) {
        ul_log(LOG_ERR, "bad value for %s: \"%s\" (a number from %ld to %ld)", opt, v, lo, hi);
        exit(1);
    }
    return (int)x;
}

/* the last of repeated options wins; the earlier value is freed */
static void set_str(char **dst, const char *v)
{
    free(*dst);
    *dst = xstrdup(v);
}

static int read_url_file(const char *path, char ***urls, int *n)
{
    size_t len;
    char *data = read_file(path, &len);
    if (!data) { ul_log(LOG_ERR, "cannot open %s", path); return -1; }
    char *cur = data;
    if ((unsigned char)cur[0] == 0xEF && (unsigned char)cur[1] == 0xBB && (unsigned char)cur[2] == 0xBF) cur += 3;
    while (*cur) {
        char *nl = strchr(cur, '\n');
        if (nl) *nl = 0;
        char *t = str_trim(cur);
        cur = nl ? nl + 1 : cur + strlen(cur);
        if (!*t || *t == '#') continue;
        *urls = xrealloc(*urls, sizeof(char *) * (size_t)(*n + 1));
        (*urls)[(*n)++] = xstrdup(t);
    }
    free(data);
    return 0;
}

int main(void)
{
    int argc = 0;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    char **argv = xcalloc((size_t)argc + 1, sizeof(char *));
    for (int i = 0; i < argc; i++) argv[i] = w_to_u8(wargv[i]);
    LocalFree(wargv);

    /* started by Chrome / Edge as the extension's native messaging host */
    if (argc >= 2 && str_istarts(argv[1], "chrome-extension://")) return native_host_main();

    con_init();
    SetConsoleCtrlHandler(NULL, FALSE);     /* a parent may have disabled Ctrl+C for us */
    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    ul_config cfg;
    cfg_defaults(&cfg);
    char **urls = NULL;
    char *job = NULL;
    int nurls = 0, get_ytdlp = 0, only_urls = 0, install = -1;

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (only_urls || a[0] != '-' || !a[1]) {
            urls = xrealloc(urls, sizeof(char *) * (size_t)(nurls + 1));
            urls[nurls++] = xstrdup(a);
            continue;
        }
        if (!strcmp(a, "--")) { only_urls = 1; continue; }
        char *eq = NULL;
        if (a[1] == '-' && (eq = strchr(a, '=')) != NULL) *eq++ = 0;
        const char *v;
#define OPT(s, l) (!strcmp(a, s) || !strcmp(a, l))
        /* a switch never takes "=value": "--insecure=0" must not switch it ON */
        static const char *switches[] = { "-h", "--help", "-v", "--version", "-k", "--insecure", "--playlist",
                                          "--overwrite", "--direct", "--ytdlp", "--get-ytdlp", "--install",
                                          "--uninstall" };
        for (size_t k = 0; eq && k < sizeof switches / sizeof *switches; k++)
            if (!strcmp(a, switches[k])) { ul_log(LOG_ERR, "option %s does not take a value", a); return 1; }
        if (OPT("-h", "--help")) { usage(); return 0; }
        else if (OPT("-v", "--version")) { printf("%s %s\n", UL_NAME, UL_VERSION); return 0; }
        else if (OPT("-c", "--connections")) {
            cfg.connections = int_value(need_value(argc, argv, &i, eq, a), a, 1, 1000000);
            if (cfg.connections > UL_MAX_CONN) cfg.connections = UL_MAX_CONN;
        }
        else if (OPT("-d", "--dir"))         set_str(&cfg.out_dir, need_value(argc, argv, &i, eq, a));
        else if (OPT("-o", "--output"))      set_str(&cfg.out_name, need_value(argc, argv, &i, eq, a));
        else if (OPT("-i", "--input")) {
            v = need_value(argc, argv, &i, eq, a);
            int before = nurls;
            if (read_url_file(v, &urls, &nurls) != 0) return 1;
            if (nurls == before) { ul_log(LOG_ERR, "%s contains no links", v); return 1; }
        }
        else if (OPT("-l", "--limit")) {
            v = need_value(argc, argv, &i, eq, a);
            cfg.speed_limit = parse_size(v);
            if (cfg.speed_limit < 0) { ul_log(LOG_ERR, "bad speed limit: %s", v); return 1; }
        }
        else if (OPT("-q", "--quality"))     cfg.max_height = int_value(need_value(argc, argv, &i, eq, a), a, 0, 100000);
        else if (OPT("-f", "--format"))      set_str(&cfg.format, need_value(argc, argv, &i, eq, a));
        else if (OPT("-H", "--header")) {
            v = need_value(argc, argv, &i, eq, a);
            if (!strchr(v, ':') || strpbrk(v, "\r\n")) { ul_log(LOG_ERR, "header must look like \"Name: value\""); return 1; }
            if (cfg.nheaders < UL_MAX_HEADERS) cfg.headers[cfg.nheaders++] = xstrdup(v);
        }
        else if (OPT("-U", "--user-agent"))  set_str(&cfg.user_agent, need_value(argc, argv, &i, eq, a));
        else if (OPT("-r", "--referer"))     set_str(&cfg.referer, need_value(argc, argv, &i, eq, a));
        else if (OPT("-b", "--cookie"))      set_str(&cfg.cookie, need_value(argc, argv, &i, eq, a));
        else if (!strcmp(a, "--cookies")) {
            v = need_value(argc, argv, &i, eq, a);
            if (cookies_load(&cfg, v) < 0) { ul_log(LOG_ERR, "cannot read cookies file %s", v); return 1; }
            set_str(&cfg.cookie_file, v);
        }
        else if (OPT("-x", "--proxy"))       set_str(&cfg.proxy, need_value(argc, argv, &i, eq, a));
        else if (OPT("-k", "--insecure"))    cfg.insecure = 1;
        else if (OPT("-R", "--retries"))     cfg.retries = int_value(need_value(argc, argv, &i, eq, a), a, 0, 1000000);
        else if (OPT("-t", "--timeout"))     cfg.timeout_sec = int_value(need_value(argc, argv, &i, eq, a), a, 1, 1000000);
        else if (!strcmp(a, "--playlist"))   cfg.playlist = 1;
        else if (!strcmp(a, "--overwrite"))  cfg.overwrite = 1;
        /* --direct / --ytdlp exclude each other: the later one wins */
        else if (!strcmp(a, "--direct"))     { cfg.force_direct = 1; cfg.force_ytdlp = 0; }
        else if (!strcmp(a, "--ytdlp"))      { cfg.force_ytdlp = 1; cfg.force_direct = 0; }
        else if (!strcmp(a, "--ytdlp-args")) set_str(&cfg.ytdlp_args, need_value(argc, argv, &i, eq, a));
        else if (!strcmp(a, "--get-ytdlp"))  get_ytdlp = 1;
        else if (!strcmp(a, "--install"))    install = 0;
        else if (!strcmp(a, "--uninstall"))  install = 1;
        else if (!strcmp(a, "--job"))        set_str(&job, need_value(argc, argv, &i, eq, a));
        else { ul_log(LOG_ERR, "unknown option %s (see uloader --help)", a); return 1; }
#undef OPT
    }
    if (cfg.retries < 0) cfg.retries = 0;
    if (cfg.retries > 100) cfg.retries = 100;
    if (cfg.timeout_sec < 5) cfg.timeout_sec = 5;

    if (install >= 0) return host_install(install);
    if (job) {
        int jr = run_job(&cfg, job);
        torrent_flush_announces(3000);
        return jr;
    }

    int interactive_mode = nurls == 0 && !get_ytdlp;
    if (!cfg.out_dir) cfg.out_dir = interactive_mode ? downloads_dir() : xstrdup(".");
    if (dir_create(cfg.out_dir) != 0) { ul_log(LOG_ERR, "cannot create folder %s", cfg.out_dir); return 1; }
    if (cfg.out_name && nurls > 1) {
        ul_log(LOG_WARN, "-o is ignored when downloading several links");
        free(cfg.out_name);
        cfg.out_name = NULL;
    }

    int rc = 0;
    if (get_ytdlp) {
        ul_task *tk = task_new("");
        http_session *s = http_session_new(&cfg, tk);
        InterlockedExchange(&g_busy, 1);
        rc = s ? ytdlp_install(s, &cfg) : 1;
        InterlockedExchange(&g_busy, 0);
        http_session_free(s);
        task_free(tk);
    }
    if (interactive_mode) {
        interactive(&cfg);
    } else {
        int ok = 0, failed = 0, paused = 0;
        for (int i = 0; i < nurls; i++) {
            if (nurls > 1) ul_log(LOG_INFO, "\n[%d/%d] %s", i + 1, nurls, urls[i]);
            int r = run_one(&cfg, urls[i], NULL);
            if (r == 0) ok++;
            else if (r == 2) { paused = 1; break; }
            else failed++;
        }
        if (nurls > 1)
            ul_log(failed ? LOG_WARN : LOG_OK, "\nQueue finished: %d ok, %d failed%s", ok, failed,
                   paused ? ", paused" : "");
        if (paused) rc = 2;
        else if (failed) rc = 1;
    }
    torrent_flush_announces(3000);          /* let a torrent's "stopped" reach its trackers */
    return rc;
}
