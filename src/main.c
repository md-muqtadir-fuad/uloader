/*
 * U-loader - command line front-end.
 *
 *   uloader [options] URL [URL...]
 *   uloader                 (interactive: paste links one after another)
 */
#include "uloader.h"
#include <shellapi.h>

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
            /* several links may be pasted at once */
            char *save = NULL;
            for (char *tok = strtok_r(t, " \t\r\n", &save); tok; tok = strtok_r(NULL, " \t\r\n", &save)) {
                run_one(cfg, tok, NULL);
                if (g_abort) break;
            }
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
        if (OPT("-h", "--help")) { usage(); return 0; }
        else if (OPT("-v", "--version")) { printf("%s %s\n", UL_NAME, UL_VERSION); return 0; }
        else if (OPT("-c", "--connections")) {
            v = need_value(argc, argv, &i, eq, a);
            cfg.connections = atoi(v);
            if (cfg.connections < 1) cfg.connections = 1;
            if (cfg.connections > UL_MAX_CONN) cfg.connections = UL_MAX_CONN;
        }
        else if (OPT("-d", "--dir"))         { free(cfg.out_dir); cfg.out_dir = xstrdup(need_value(argc, argv, &i, eq, a)); }
        else if (OPT("-o", "--output"))      { free(cfg.out_name); cfg.out_name = xstrdup(need_value(argc, argv, &i, eq, a)); }
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
        else if (OPT("-q", "--quality"))     cfg.max_height = atoi(need_value(argc, argv, &i, eq, a));
        else if (OPT("-f", "--format"))      cfg.format = xstrdup(need_value(argc, argv, &i, eq, a));
        else if (OPT("-H", "--header")) {
            v = need_value(argc, argv, &i, eq, a);
            if (!strchr(v, ':') || strpbrk(v, "\r\n")) { ul_log(LOG_ERR, "header must look like \"Name: value\""); return 1; }
            if (cfg.nheaders < UL_MAX_HEADERS) cfg.headers[cfg.nheaders++] = xstrdup(v);
        }
        else if (OPT("-U", "--user-agent"))  cfg.user_agent = xstrdup(need_value(argc, argv, &i, eq, a));
        else if (OPT("-r", "--referer"))     cfg.referer = xstrdup(need_value(argc, argv, &i, eq, a));
        else if (OPT("-b", "--cookie"))      cfg.cookie = xstrdup(need_value(argc, argv, &i, eq, a));
        else if (!strcmp(a, "--cookies")) {
            v = need_value(argc, argv, &i, eq, a);
            if (cookies_load(&cfg, v) < 0) { ul_log(LOG_ERR, "cannot read cookies file %s", v); return 1; }
            cfg.cookie_file = xstrdup(v);
        }
        else if (OPT("-x", "--proxy"))       cfg.proxy = xstrdup(need_value(argc, argv, &i, eq, a));
        else if (OPT("-k", "--insecure"))    cfg.insecure = 1;
        else if (OPT("-R", "--retries"))     cfg.retries = atoi(need_value(argc, argv, &i, eq, a));
        else if (OPT("-t", "--timeout"))     cfg.timeout_sec = atoi(need_value(argc, argv, &i, eq, a));
        else if (!strcmp(a, "--playlist"))   cfg.playlist = 1;
        else if (!strcmp(a, "--overwrite"))  cfg.overwrite = 1;
        else if (!strcmp(a, "--direct"))     cfg.force_direct = 1;
        else if (!strcmp(a, "--ytdlp"))      cfg.force_ytdlp = 1;
        else if (!strcmp(a, "--ytdlp-args")) cfg.ytdlp_args = xstrdup(need_value(argc, argv, &i, eq, a));
        else if (!strcmp(a, "--get-ytdlp"))  get_ytdlp = 1;
        else if (!strcmp(a, "--install"))    install = 0;
        else if (!strcmp(a, "--uninstall"))  install = 1;
        else if (!strcmp(a, "--job"))        job = xstrdup(need_value(argc, argv, &i, eq, a));
        else { ul_log(LOG_ERR, "unknown option %s (see uloader --help)", a); return 1; }
#undef OPT
    }
    if (cfg.retries < 0) cfg.retries = 0;
    if (cfg.retries > 100) cfg.retries = 100;
    if (cfg.timeout_sec < 5) cfg.timeout_sec = 5;

    if (install >= 0) return host_install(install);
    if (job) return run_job(&cfg, job);

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
    return rc;
}
