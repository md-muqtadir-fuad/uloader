/*
 * U-loader - tasks: one download, run the same way by the console, the
 * GUI and the browser bridge. Dispatches each link to the right engine:
 * direct file (http/https/ftp), HLS stream, torrent, or video site.
 */
#include "uloader.h"
#include <process.h>

/* ------------------------------------------------------------------ */
/* output names in use by downloads running in this process            */
/* ------------------------------------------------------------------ */
typedef struct name_res { struct name_res *next; char *path; } name_res;
static name_res *g_names;
static CRITICAL_SECTION g_names_lock;
static INIT_ONCE g_names_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK names_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&g_names_lock);
    return TRUE;
}

static int name_taken_locked(const char *path)
{
    for (name_res *r = g_names; r; r = r->next)
        if (_stricmp(r->path, path) == 0) return 1;
    return 0;
}

/* Reserve `*path` for this download. If another running download (in this
   window or in another U-loader process) is already writing to that name,
   switch to "name (N).ext". */
static void name_reserve(char **path, int may_rename)
{
    InitOnceExecuteOnce(&g_names_once, names_init, NULL, NULL);
    EnterCriticalSection(&g_names_lock);
    for (int i = 0; i < 1000 && may_rename; i++) {
        char *part = xasprintf("%s.ulpart", *path);
        int busy = name_taken_locked(*path);
        if (!busy && file_exists(part)) {        /* held open by another process? */
            wchar_t *w = wpath(part);
            HANDLE h = CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            busy = h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION;
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            free(w);
        }
        free(part);
        if (!busy) break;
        char *u = unique_path(*path);
        if (name_taken_locked(u)) {             /* reserved here but not on disk yet */
            char *stem = path_strip_ext(u);
            const char *ext = path_ext(u);
            for (int k = 1; k < 100000; k++) {
                char *v = xasprintf("%s_%d%s", stem, k, ext);
                char *vp = xasprintf("%s.ulpart", v), *vs = xasprintf("%s.ulstate", v);
                /* free here AND on disk: never take over somebody's file */
                int unused = !name_taken_locked(v) && !file_exists(v) && !file_exists(vp) && !file_exists(vs);
                free(vp);
                free(vs);
                if (unused) { free(u); u = v; break; }
                free(v);
            }
            free(stem);
        }
        free(*path);
        *path = u;
    }
    name_res *r = xcalloc(1, sizeof *r);
    r->path = xstrdup(*path);
    r->next = g_names;
    g_names = r;
    LeaveCriticalSection(&g_names_lock);
}

static void name_release(const char *path)
{
    EnterCriticalSection(&g_names_lock);
    for (name_res **pp = &g_names; *pp; pp = &(*pp)->next) {
        if (_stricmp((*pp)->path, path) == 0) {
            name_res *r = *pp;
            *pp = r->next;
            free(r->path);
            free(r);
            break;
        }
    }
    LeaveCriticalSection(&g_names_lock);
}

/* ------------------------------------------------------------------ */
/* task lifecycle                                                      */
/* ------------------------------------------------------------------ */
ul_task *task_new(const char *url)
{
    ul_task *t = xcalloc(1, sizeof *t);
    t->url = xstrdup(url ? url : "");
    t->total = -1;
    t->eta = -1;
    t->state = TS_QUEUED;
    InitializeCriticalSection(&t->lock);
    return t;
}

void task_free(ul_task *t)
{
    if (!t) return;
    DeleteCriticalSection(&t->lock);
    free(t->url);
    free(t->name);
    free(t->path);
    free(t);
}

int task_aborted(const ul_task *t)
{
    if (t && t->abort) return 1;
    return g_abort && !(t && t->ignore_ctrl_c);   /* a final tracker "stopped" still goes out */
}

void task_sleep(const ul_task *t, DWORD ms)
{
    while (ms > 0 && !task_aborted(t)) {
        DWORD step = ms > 100 ? 100 : ms;
        Sleep(step);
        ms -= step;
    }
}

void task_set_file(ul_task *t, const char *name, const char *path)
{
    if (!t) return;
    EnterCriticalSection(&t->lock);
    if (name) { free(t->name); t->name = xstrdup(name); }
    if (path) { free(t->path); t->path = xstrdup(path); }
    LeaveCriticalSection(&t->lock);
}

void task_set_stage(ul_task *t, const char *fmt, ...)
{
    if (!t) return;
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    EnterCriticalSection(&t->lock);
    memcpy(t->stage, buf, sizeof buf);
    LeaveCriticalSection(&t->lock);
}

void task_reset_progress(ul_task *t)
{
    t->done = 0;
    t->total = -1;
    t->speed = 0;
    t->eta = -1;
    t->conns = 0;
    EnterCriticalSection(&t->lock);
    memset(t->segmap, 0, sizeof t->segmap);
    t->stage[0] = 0;
    LeaveCriticalSection(&t->lock);
}

void tlog(ul_task *t, int kind, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    char *msg = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(msg, (size_t)n + 1, fmt, ap);
    va_end(ap);
    if (t && (kind == LOG_ERR || kind == LOG_WARN || kind == LOG_OK)) {
        EnterCriticalSection(&t->lock);
        const char *m = msg;
        while (*m == ' ') m++;
        snprintf(t->status, sizeof t->status, "%s", m);
        LeaveCriticalSection(&t->lock);
    }
    if (t && t->on_log) t->on_log(t, kind, msg);
    else con_log(kind, msg);
    free(msg);
}

/* Console progress line: "45.2% ███▓░░ 120 MB/266 MB  12.5 MB/s  ETA 00:12  [8 conn]" */
static void console_progress(ul_task *t)
{
    unsigned char map[UL_SEGVIEW];
    char stage[96];
    EnterCriticalSection(&t->lock);
    memcpy(map, t->segmap, sizeof map);
    memcpy(stage, t->stage, sizeof stage);
    LeaveCriticalSection(&t->lock);
    int64_t done = t->done, total = t->total, speed = t->speed, eta = t->eta;

    int w = con_width() - 64;
    if (w < 10) w = 10;
    if (w > 40) w = 40;
    sbuf line = { 0 };
    char a[32], b[32], c[32], d[32];
    if (total > 0) sb_printf(&line, "%5.1f%% ", done >= total ? 100.0 : 100.0 * (double)done / (double)total);
    else sb_puts(&line, "  ... ");
    if (g_vt) sb_puts(&line, "\x1b[32m");
    if (total > 0) {
        for (int i = 0; i < w; i++) {
            int a0 = i * UL_SEGVIEW / w, b0 = (i + 1) * UL_SEGVIEW / w;
            if (b0 <= a0) b0 = a0 + 1;
            int sum = 0;
            for (int k = a0; k < b0; k++) sum += map[k];
            int avg = sum / (b0 - a0);
            sb_puts(&line, avg >= 250 ? "\xE2\x96\x88" : avg > 0 ? "\xE2\x96\x93" : "\xE2\x96\x91");
        }
    } else {
        int p = (int)(now_sec() * 4) % w;
        for (int i = 0; i < w; i++) sb_puts(&line, i == p ? "\xE2\x96\x88" : "\xE2\x96\x91");
    }
    if (g_vt) sb_puts(&line, "\x1b[0m");
    if (total > 0) sb_printf(&line, " %s/%s", fmt_size(done, a, sizeof a), fmt_size(total, b, sizeof b));
    else sb_printf(&line, " %s", fmt_size(done, a, sizeof a));
    sb_printf(&line, "  %s/s", fmt_size(speed, c, sizeof c));
    if (eta >= 0) sb_printf(&line, "  ETA %s", fmt_eta((double)eta, d, sizeof d));
    if (stage[0]) sb_printf(&line, "  [%s]", stage);
    else sb_printf(&line, "  [%ld conn]", (long)t->conns);
    con_progress(line.s);
    sb_free(&line);
}

void task_progress(ul_task *t)
{
    if (!t) return;
    if (t->on_progress) t->on_progress(t);
    else if (!t->on_log) console_progress(t);
}

/* ------------------------------------------------------------------ */
/* naming                                                              */
/* ------------------------------------------------------------------ */
/* Where the value of parameter `name` starts in a Content-Disposition
   header. Only a real parameter counts: "xfilename=" or a "filename="
   inside another parameter's quoted value does not. */
static const char *cd_param(const char *cd, const char *name)
{
    size_t nl = strlen(name);
    int inq = 0, at_name = 1;
    for (const char *p = cd; *p; p++) {
        if (inq) {
            if (*p == '\\' && p[1]) p++;
            else if (*p == '"') inq = 0;
            continue;
        }
        if (*p == '"') { inq = 1; at_name = 0; continue; }
        if (*p == ';') { at_name = 1; continue; }
        if (*p == ' ' || *p == '\t') continue;
        if (at_name && _strnicmp(p, name, nl) == 0) {
            const char *q = p + nl;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '=') {
                for (q++; *q == ' ' || *q == '\t'; q++) {}
                return q;
            }
        }
        at_name = 0;
    }
    return NULL;
}

static char *disposition_filename(const char *cd)
{
    if (!cd) return NULL;
    const char *p = cd_param(cd, "filename*");
    if (p) {
        while (*p == ' ' || *p == '"') p++;
        size_t n = strcspn(p, ";\"");
        const char *q = memchr(p, '\'', n);              /* charset'lang'value */
        if (q) {
            const char *q2 = memchr(q + 1, '\'', (size_t)(p + n - q - 1));
            if (q2) {
                char *v = url_decode(q2 + 1, (size_t)(p + n - q2 - 1));
                if (*v) return v;
                free(v);
            }
        }
    }
    p = cd_param(cd, "filename");
    if (p) {
        sbuf b = { 0 };
        if (*p == '"') {
            for (p++; *p && *p != '"'; p++) {
                if (*p == '\\' && p[1]) p++;
                sb_putc(&b, *p);
            }
        } else {
            size_t n = strcspn(p, ";");
            sb_putn(&b, p, n);
        }
        char *raw = sb_steal(&b);
        char *t = str_trim(raw);
        char *v = strchr(t, '%') ? url_decode(t, strlen(t)) : xstrdup(t);
        free(raw);
        if (*v) return v;
        free(v);
    }
    return NULL;
}

static char *pick_filename(const ul_config *cfg, const char *url, const http_response *r)
{
    char *name = disposition_filename(r->content_disposition);
    if (name) {                     /* "dir/../x.zip" -> keep only the last part */
        char *b = xstrdup(path_basename(name));
        free(name);
        name = *b ? b : (free(b), NULL);
    }
    if (!name) name = url_filename(url);
    if ((!name || !*path_ext(name)) && r->final_url) {
        char *alt = url_filename(r->final_url);
        if (alt && *path_ext(alt)) { free(name); name = alt; }
        else free(alt);
    }
    if (!name) name = xstrdup("download");
    if (!*path_ext(name)) {
        const char *ext = ext_for_content_type(r->content_type);
        if (*ext) { char *t = xasprintf("%s%s", name, ext); free(name); name = t; }
    }
    char *clean = sanitize_filename(name);
    free(name);
    char *path = path_join(cfg->out_dir, clean);
    free(clean);
    return path;
}

/* An earlier run may have saved this link as "name (N).ext" (because
   "name.ext" already existed): find its resume state so that running the
   same command again continues it instead of starting yet another copy. */
static char *find_renamed_resume(const char *path, const char *url)
{
    char *stem = path_strip_ext(path);
    const char *ext = path_ext(path);
    char *found = NULL;
    for (int i = 1; i < 1000 && !found; i++) {
        char *cand = xasprintf("%s (%d)%s", stem, i, ext);
        char *st = xasprintf("%s.ulstate", cand);
        int has_state = file_exists(st), has_file = file_exists(cand);
        if (has_state) {
            size_t len;
            char *data = read_file(st, &len);
            const char *l = data ? strstr(data, "\nurl ") : NULL;
            if (l) {
                l += 5;
                size_t e = strcspn(l, "\r\n");
                if (strlen(url) == e && strncmp(l, url, e) == 0) found = cand;
            }
            free(data);
        }
        free(st);
        if (!found) free(cand);
        if (!has_state && !has_file) break;      /* numbering ends here */
    }
    free(stem);
    return found;
}

/* Same size is not enough to call a file "already downloaded". A finished
   download remembers the server's ETag / Last-Modified (file_meta_write),
   and those decide. Without that record (a file from elsewhere, an older
   U-loader, a drive without NTFS streams) the first and last bytes are
   compared with the server. *probe_used is set once the probe response
   was read from: it can then no longer be streamed into the file. */
static int is_same_file(http_session *s, const char *url, const ul_headers *hd, http_response *probe,
                        const char *path, int64_t total, int *probe_used)
{
    int m = file_meta_matches(path, probe->etag, probe->last_modified);
    if (m >= 0) return m;
    if (total == 0) return 1;
    enum { N = 65536 };
    FILE *f = fopen_u8(path, "rb");
    if (!f) return 0;
    char *a = xmalloc(N), *b = xmalloc(N);
    size_t n = total < N ? (size_t)total : N, got = 0;
    *probe_used = 1;
    while (got < n) {
        DWORD k = 0;
        if (http_read(probe, b + got, (DWORD)(n - got), &k) != 0 || k == 0) break;
        got += k;
    }
    int same = got == n && fread(a, 1, n, f) == n && memcmp(a, b, n) == 0;
    if (same && probe->status == 206 && total > (int64_t)n) {
        int64_t from = total - N > (int64_t)n ? total - N : (int64_t)n;
        size_t want = (size_t)(total - from), rg = 0;
        char err[256];
        char *tail = http_get_body(s, url, hd, from, total - 1, 0, &rg, NULL, 1, err, sizeof err, NULL);
        if (tail) {                                   /* no answer: the start matched, keep it */
            same = rg == want && _fseeki64(f, from, SEEK_SET) == 0 && fread(a, 1, want, f) == want &&
                   memcmp(a, tail, want) == 0;
            free(tail);
        }
    }
    free(a);
    free(b);
    fclose(f);
    return same;
}

static int is_hls_url(const char *url)
{
    size_t q = strcspn(url, "?#");
    return q >= 5 && _strnicmp(url + q - 5, ".m3u8", 5) == 0;
}

static int is_torrent_url(const char *url)
{
    size_t q = strcspn(url, "?#");
    return q >= 8 && _strnicmp(url + q - 8, ".torrent", 8) == 0;
}

/* ------------------------------------------------------------------ */
int download_url(http_session *s, ul_config *cfg, const char *url, const char *forced_path,
                 const ul_headers *hd, int64_t chunk, int allow_extract)
{
    ul_task *t = http_task(s);
    char err[256];
    http_session_set_origin(s, url);      /* credentials stay with this link's host */
    if (is_hls_url(url)) return hls_run(s, cfg, url, hd, forced_path);

    http_response probe;
    int ok = 0;
    err[0] = 0;
    int is_ftp = str_istarts(url, "ftp://");
    for (int attempt = 0; attempt <= 3 && !task_aborted(t); attempt++) {
        if (attempt) {
            task_sleep(t, 1000u * (DWORD)attempt);
            if (task_aborted(t)) break;            /* paused while waiting: no new request */
        }
        int rq = http_request(s, url, hd, 0, -1, 0, &probe, err, sizeof err);
        if (rq == -2) break;                       /* permanent: do not retry */
        if (rq != 0) continue;
        if (probe.status == 416) {   /* empty file or Range not accepted */
            http_close(&probe);
            rq = http_request(s, url, hd, -1, -1, 0, &probe, err, sizeof err);
            if (rq == -2) break;
            if (rq != 0) continue;
        }
        if (probe.status >= 500 || probe.status == 429 || probe.status == 408) {
            /* keep the FTP layer's own explanation ("too many connections...") */
            if (!(is_ftp && err[0])) snprintf(err, sizeof err, "server answered HTTP %d", probe.status);
            http_close(&probe);
            continue;
        }
        ok = 1;
        break;
    }
    if (!ok) {
        if (task_aborted(t)) return 2;
        tlog(t, LOG_ERR, "%s", err[0] ? err : "cannot connect");
        return 1;
    }
    if (probe.status >= 400 || probe.status < 200 || probe.status >= 300) {
        int st = probe.status;
        http_close(&probe);
        char *yt = NULL;
        if (allow_extract && !cfg->force_direct && (st == 403 || st == 401) && (yt = find_tool("yt-dlp.exe"))) {
            free(yt);
            return ytdlp_run(s, cfg, url);      /* page may need the extractor */
        }
        tlog(t, LOG_ERR, "server answered %s %d%s", str_istarts(url, "ftp") ? "FTP" : "HTTP", st,
             st == 404 ? " (not found)" : st == 403 ? " (access denied - the link may have expired)" :
             st == 401 ? " (login required)" : "");
        return 1;
    }

    const char *ct = probe.content_type ? probe.content_type : "";
    const char *final = probe.final_url ? probe.final_url : url;
    if (str_istr(ct, "mpegurl")) {
        char *f = xstrdup(final);
        http_close(&probe);
        int r = hls_run(s, cfg, f, hd, forced_path);
        free(f);
        return r;
    }
    if (!forced_path && (str_istarts(ct, "application/x-bittorrent") || is_torrent_url(final)) &&
        probe.content_length >= 0 && probe.content_length < (16 << 20)) {
        http_close(&probe);
        size_t len;
        char *body = http_get_body(s, url, hd, -1, -1, 0, &len, NULL, cfg->retries, err, sizeof err, NULL);
        if (!body) { tlog(t, LOG_ERR, "cannot fetch the .torrent file: %s", err); return task_aborted(t) ? 2 : 1; }
        if (len > 0 && body[0] == 'd') {
            int r = torrent_run(s, cfg, url, (const unsigned char *)body, len);
            free(body);
            return r;
        }
        free(body);
        tlog(t, LOG_ERR, "that does not look like a .torrent file");
        return 1;
    }
    if (allow_extract && !cfg->force_direct && str_istarts(ct, "text/html")) {
        http_close(&probe);
        char *yt = find_tool("yt-dlp.exe");
        if (yt) { free(yt); return ytdlp_run(s, cfg, url); }
        tlog(t, LOG_ERR, "this link is a web page, not a file.");
        tlog(t, LOG_INFO, "  To grab videos from web pages, install the site extractor once:  uloader --get-ytdlp");
        tlog(t, LOG_INFO, "  To save the page itself, use direct mode (--direct)");
        return 1;
    }

    char *path;
    if (forced_path) path = xstrdup(forced_path);
    else if (cfg->out_name) {
        char *clean = sanitize_filename(path_basename(cfg->out_name));
        path = path_join(cfg->out_dir, clean);
        free(clean);
    } else path = pick_filename(cfg, url, &probe);

    char *state = xasprintf("%s.ulstate", path);
    int resuming = file_exists(state);
    int probe_used = 0;                      /* read from by is_same_file */
    free(state);
    if (!resuming && file_exists(path)) {
        int64_t total = probe.status == 206 ? probe.range_total : probe.content_length;
        if (dir_exists(path) && forced_path) {
            tlog(t, LOG_ERR, "a folder is in the way: %s", path);
            http_close(&probe);
            free(path);
            return 1;
        } else if (dir_exists(path)) {
            char *u = unique_path(path);
            free(path);
            path = u;
        } else if (cfg->overwrite) {
            /* replaced when the download completes */
        } else if (total >= 0 && file_size(path) == total &&
                   is_same_file(s, url, hd, &probe, path, total, &probe_used)) {
            tlog(t, LOG_OK, "Already downloaded: %s", path);
            task_set_file(t, path_basename(path), path);
            t->done = t->total = total;
            http_close(&probe);
            free(path);
            return 0;
        } else if (forced_path) {
            /* The caller (a stream to merge, ...) uses exactly this name: an
               old leftover there is replaced when the download completes,
               never dodged with "name (1)" - that left the caller merging
               the stale file. */
        } else {
            char *u = find_renamed_resume(path, url);
            if (!u) u = unique_path(path);
            free(path);
            path = u;
        }
    }

    name_reserve(&path, !forced_path);       /* no two downloads into one file */
    dl_job job;
    job.url = url;
    job.path = path;
    job.headers = hd;
    job.connections = cfg->connections;
    job.chunk_size = chunk;
    if (probe_used) http_close(&probe);      /* partly read: the engine asks again */
    int rc = dl_run(s, cfg, &job, probe_used ? NULL : &probe);   /* takes ownership of probe */
    name_release(path);
    free(path);
    return rc;
}

/* Watches the pause flag for the whole run and closes in-flight requests
   as soon as it is set, so even a connection that never answers stops at
   once (the engines cancel too, but not during the first request). */
typedef struct {
    ul_task      *t;
    http_session *s;
    volatile LONG done;
} watcher;

static unsigned __stdcall watch_abort(void *arg)
{
    watcher *w = arg;
    while (!w->done) {
        if (task_aborted(w->t)) http_session_cancel(w->s);
        Sleep(100);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
int task_run(ul_task *t, ul_config *cfg)
{
    /* The caller clears t->abort before it starts this thread. Clearing it
       here again would lose a Pause / Delete / Exit clicked in between.
       The GUI already marks the task "running", so earlier progress - not
       the state - tells that this is a resume. */
    int resuming = t->state == TS_PAUSED || t->done > 0;
    t->state = TS_RUNNING;
    if (resuming) {           /* keep showing the old progress until the engine reports */
        t->speed = 0;
        t->eta = -1;
        t->conns = 0;
    } else {
        task_reset_progress(t);
    }
    EnterCriticalSection(&t->lock);
    t->status[0] = 0;
    LeaveCriticalSection(&t->lock);

    char *raw = str_trim(xstrdup(t->url)), *raw0 = raw;
    char *url;
    if (str_istarts(raw, "http://") || str_istarts(raw, "https://") || str_istarts(raw, "ftp://") ||
        is_magnet(raw)) url = xstrdup(raw);
    else if (str_iends(raw, ".torrent") && file_exists(raw)) url = xstrdup(raw);
    else if (strstr(raw, "://")) {
        tlog(t, LOG_ERR, "unsupported link type: %s (http, https, ftp, magnet or .torrent)", raw);
        free(raw0);
        t->state = TS_ERROR;
        return 1;
    } else url = xasprintf("https://%s", raw);
    free(raw0);

    if (!cfg->out_dir) cfg->out_dir = xstrdup(".");
    dir_create(cfg->out_dir);
    if (cfg->cookie && !cfg->cookie_host) cfg->cookie_host = url_host(url);

    int rc;
    http_session *s = http_session_new(cfg, t);
    watcher w = { t, s, 0 };
    HANDLE wt = s ? (HANDLE)_beginthreadex(NULL, 0, watch_abort, &w, 0, NULL) : NULL;
    if (!s) {
        char e[200];
        tlog(t, LOG_ERR, "cannot start WinHTTP: %s", win_errstr(GetLastError(), e, sizeof e));
        rc = 1;
    } else if (is_magnet(url) || (str_iends(url, ".torrent") && file_exists(url))) {
        rc = torrent_run(s, cfg, url, NULL, 0);
    } else if (str_istarts(url, "ftp://")) {
        rc = download_url(s, cfg, url, NULL, NULL, 0, 0);
    } else if (cfg->force_ytdlp || (!cfg->force_direct && is_video_site(url))) {
        rc = ytdlp_run(s, cfg, url);
    } else {
        rc = download_url(s, cfg, url, NULL, NULL, 0, 1);
    }
    if (wt) {
        InterlockedExchange(&w.done, 1);
        WaitForSingleObject(wt, INFINITE);
        CloseHandle(wt);
    }
    http_session_free(s);
    free(url);
    t->speed = 0;
    t->eta = -1;
    t->conns = 0;
    t->state = rc == 0 ? TS_DONE : rc == 2 ? TS_PAUSED : TS_ERROR;
    if (rc == 0) {
        EnterCriticalSection(&t->lock);
        memset(t->segmap, 255, sizeof t->segmap);
        t->stage[0] = 0;
        LeaveCriticalSection(&t->lock);
        if (t->total > 0) t->done = t->total;
    } else if (rc == 2) {
        EnterCriticalSection(&t->lock);
        snprintf(t->status, sizeof t->status, "Paused");
        LeaveCriticalSection(&t->lock);
    }
    /* the final state for the window; the console has already printed its
       last line (a progress bar after "failed" / "Saved to" is noise) */
    if (t->on_progress) task_progress(t);
    return rc;
}

/* ------------------------------------------------------------------ */
/* job files (browser extension -> native host -> uloader / GUI)       */
/* ------------------------------------------------------------------ */
int job_load(const char *path, ul_config *cfg, char **url)
{
    size_t len;
    char *data = read_file(path, &len);
    *url = NULL;
    if (!data) return -1;
    if (strncmp(data, "U-LOADER-JOB 1", 14) != 0) { free(data); return -1; }
    char *cur = data;
    while (*cur) {
        char *nl = strchr(cur, '\n');
        if (nl) *nl = 0;
        char *line = cur;
        cur = nl ? nl + 1 : cur + strlen(cur);
        size_t ll = strlen(line);
        if (ll && line[ll - 1] == '\r') line[--ll] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!*v) continue;
        if (!strcmp(k, "url")) { free(*url); *url = xstrdup(v); }
        else if (!strcmp(k, "mode")) {
            cfg->force_direct = !strcmp(v, "direct");
            cfg->force_ytdlp = !strcmp(v, "ytdlp");
        }
        else if (!strcmp(k, "referer")) { free(cfg->referer); cfg->referer = xstrdup(v); }
        else if (!strcmp(k, "ua")) { free(cfg->user_agent); cfg->user_agent = xstrdup(v); }
        else if (!strcmp(k, "name")) {
            char *clean = sanitize_filename(path_basename(v));
            free(cfg->out_name);
            cfg->out_name = clean;
        }
        else if (!strcmp(k, "dir")) { free(cfg->out_dir); cfg->out_dir = xstrdup(v); }
        else if (!strcmp(k, "connections")) {
            int c = atoi(v);
            if (c >= 1 && c <= UL_MAX_CONN) cfg->connections = c;
        }
        else if (!strcmp(k, "quality")) cfg->max_height = atoi(v);
        else if (!strcmp(k, "cookies")) {
            free(cfg->cookie_file);
            cfg->cookie_file = xstrdup(v);
            cookies_load(cfg, v);
        }
    }
    free(data);
    return *url ? 0 : -1;
}

/* Only cookie files the native host itself wrote (%TEMP%\ulc*.tmp) may be
   deleted after a job: a job file could name any file as its cookie jar. */
int job_cookie_file_is_temp(const char *path)
{
    if (!path) return 0;
    wchar_t tdir[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, tdir);
    if (!n || n > MAX_PATH) return 0;
    char *t = w_to_u8(tdir);
    size_t tl = strlen(t);
    const char *base = path_basename(path);
    int ok = _strnicmp(path, t, tl) == 0 && (size_t)(base - path) == tl && !strchr(path + tl, '\\') &&
             str_istarts(base, "ulc") && str_iends(base, ".tmp");
    free(t);
    return ok;
}

static void job_line(FILE *f, const char *k, const char *v)
{
    if (!v || !*v) return;
    fprintf(f, "%s=", k);
    for (const char *p = v; *p; p++) if (*p != '\r' && *p != '\n') fputc(*p, f);
    fputc('\n', f);
}

char *job_write(const char *url, const char *mode, const char *referer, const char *ua,
                const char *name, const char *dir, int connections, int quality,
                const char *cookie_file)
{
    wchar_t tdir[MAX_PATH + 1], tfile[MAX_PATH + 1];
    if (!GetTempPathW(MAX_PATH + 1, tdir) || !GetTempFileNameW(tdir, L"ulj", 0, tfile)) return NULL;
    char *path = w_to_u8(tfile);
    FILE *f = fopen_u8(path, "wb");
    if (!f) { file_delete(path); free(path); return NULL; }   /* GetTempFileName created it */
    fprintf(f, "U-LOADER-JOB 1\n");
    job_line(f, "url", url);
    job_line(f, "mode", mode);
    job_line(f, "referer", referer);
    job_line(f, "ua", ua);
    job_line(f, "name", name);
    job_line(f, "dir", dir);
    if (connections > 0) fprintf(f, "connections=%d\n", connections);
    if (quality > 0) fprintf(f, "quality=%d\n", quality);
    job_line(f, "cookies", cookie_file);
    int wok = !ferror(f);
    if (fclose(f) != 0 || !wok) { file_delete(path); free(path); return NULL; }
    return path;
}
