/*
 * U-loader - HTTP(S) client built on WinHTTP (ships with Windows; handles
 * TLS and proxies without any third-party library). Redirects are followed
 * here, not by WinHTTP, so cookies are only ever sent to the hosts they
 * belong to. ftp:// URLs are handed to ftp.c behind the same interface.
 */
#include "uloader.h"

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#ifndef WINHTTP_OPTION_DECOMPRESSION
#define WINHTTP_OPTION_DECOMPRESSION 118
#endif
#ifndef WINHTTP_DECOMPRESSION_FLAG_ALL
#define WINHTTP_DECOMPRESSION_FLAG_ALL 3
#endif

#define MAX_REDIRECTS 12

/* Every open request is registered with its session so a pause can close
   it from another thread: that makes a worker blocked in a read return at
   once instead of after the network timeout. */
typedef struct live_req {
    struct live_req *next;
    http_session    *s;
    HINTERNET        h;
    int              closed;     /* closed by http_session_cancel */
} live_req;

struct http_session {
    HINTERNET        h;
    const ul_config *cfg;
    ul_task         *task;
    CRITICAL_SECTION lock;
    live_req        *live;
    char            *origin;     /* host the credentials belong to (see http_session_set_origin) */
};

void http_session_set_origin(http_session *s, const char *url)
{
    char *h = url ? url_host(url) : NULL;
    EnterCriticalSection(&s->lock);
    free(s->origin);
    s->origin = h;
    LeaveCriticalSection(&s->lock);
}

/* host whose requests may carry Authorization / Cookie headers */
static char *origin_host(http_session *s, const char *url)
{
    EnterCriticalSection(&s->lock);
    char *h = s->origin ? xstrdup(s->origin) : NULL;
    LeaveCriticalSection(&s->lock);
    return h ? h : url_host(url);
}

static void live_add(http_session *s, http_response *r)
{
    live_req *e = xcalloc(1, sizeof *e);
    e->s = s;
    e->h = r->hRequest;
    EnterCriticalSection(&s->lock);
    e->next = s->live;
    s->live = e;
    LeaveCriticalSection(&s->lock);
    r->live = e;
}

/* returns 1 if the request handle was already closed by a cancel */
static int live_remove(live_req *e)
{
    http_session *s = e->s;
    EnterCriticalSection(&s->lock);
    for (live_req **pp = &s->live; *pp; pp = &(*pp)->next)
        if (*pp == e) { *pp = e->next; break; }
    int closed = e->closed;
    LeaveCriticalSection(&s->lock);
    free(e);
    return closed;
}

void http_session_cancel(http_session *s)
{
    if (!s) return;
    EnterCriticalSection(&s->lock);
    for (live_req *e = s->live; e; e = e->next)
        if (!e->closed) { WinHttpCloseHandle(e->h); e->closed = 1; }
    LeaveCriticalSection(&s->lock);
}

http_session *http_session_new(const ul_config *cfg, ul_task *t)
{
    http_session *s = xcalloc(1, sizeof *s);
    s->cfg = cfg;
    s->task = t;
    InitializeCriticalSection(&s->lock);
    wchar_t *ua = u8_to_w(cfg->user_agent ? cfg->user_agent : UL_DEFAULT_UA);
    if (cfg->proxy) {
        wchar_t *px = u8_to_w(cfg->proxy);
        s->h = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_NAMED_PROXY, px, L"<local>", 0);
        free(px);
    } else {
        /* Windows 8.1+: follow the system proxy settings automatically */
        s->h = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!s->h)
            s->h = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    free(ua);
    if (!s->h) { DeleteCriticalSection(&s->lock); free(s); return NULL; }

    int t_ms = cfg->timeout_sec * 1000;
    WinHttpSetTimeouts(s->h, t_ms, t_ms, t_ms, t_ms);

    /* We want many parallel TCP connections to the same server. HTTP/2 is
       left off on purpose: it would multiplex everything over one socket. */
    DWORD v = 128;
    WinHttpSetOption(s->h, WINHTTP_OPTION_MAX_CONNS_PER_SERVER, &v, sizeof v);
    WinHttpSetOption(s->h, WINHTTP_OPTION_MAX_CONNS_PER_1_0_SERVER, &v, sizeof v);
    return s;
}

void http_session_free(http_session *s)
{
    if (!s) return;
    WinHttpCloseHandle(s->h);
    DeleteCriticalSection(&s->lock);
    free(s->origin);
    free(s);
}

ul_task         *http_task(http_session *s) { return s->task; }
const ul_config *http_cfg(http_session *s)  { return s->cfg; }
int              http_aborted(http_session *s) { return task_aborted(s->task); }

/* Header values arrive as UTF-16 widened from raw bytes; for
   Content-Disposition / Location recover UTF-8 when possible. */
static char *wide_header_to_u8(const wchar_t *w, int raw)
{
    if (raw) {
        size_t n = wcslen(w);
        int bytes_only = 1;
        for (size_t i = 0; i < n; i++) if (w[i] > 0xFF) { bytes_only = 0; break; }
        if (bytes_only) {
            char *b = xmalloc(n + 1);
            for (size_t i = 0; i < n; i++) b[i] = (char)w[i];
            b[n] = 0;
            if (utf8_valid((const unsigned char *)b, n)) return b;
            free(b);
        }
    }
    return w_to_u8(w);
}

static char *query_header(HINTERNET h, DWORD what, int raw)
{
    DWORD len = 0;
    WinHttpQueryHeaders(h, what, WINHTTP_HEADER_NAME_BY_INDEX, NULL, &len, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || len == 0 || len > 1 << 20) return NULL;
    wchar_t *w = xmalloc(len + sizeof(wchar_t));
    if (!WinHttpQueryHeaders(h, what, WINHTTP_HEADER_NAME_BY_INDEX, w, &len, WINHTTP_NO_HEADER_INDEX)) {
        free(w);
        return NULL;
    }
    w[len / sizeof(wchar_t)] = 0;
    char *r = wide_header_to_u8(w, raw);
    free(w);
    return r;
}

static void add_header_line(sbuf *b, const char *name, const char *value)
{
    if (!value || strpbrk(value, "\r\n")) return;
    sb_printf(b, "%s: %s\r\n", name, value);
}

/* One request, no redirect handling. */
/* Credentials that must not follow a redirect to another host (like curl). */
static int is_credential_header(const char *name, size_t n)
{
    static const char *names[] = { "Authorization", "Cookie", "Proxy-Authorization" };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (strlen(names[i]) == n && _strnicmp(name, names[i], n) == 0) return 1;
    return 0;
}

static int request_once(http_session *s, const char *url, const ul_headers *hd,
                        int64_t from, int64_t to, int flags, int foreign, http_response *r,
                        char *err, size_t errlen)
{
    wchar_t *wurl = u8_to_w(url);
    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.dwSchemeLength = uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc) || !uc.dwHostNameLength) {
        snprintf(err, errlen, "invalid URL");
        free(wurl);
        return -2;
    }
    if (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS) {
        snprintf(err, errlen, "unsupported link type");
        free(wurl);
        return -2;
    }
    wchar_t *host = xmalloc((uc.dwHostNameLength + 1) * sizeof(wchar_t));
    memcpy(host, uc.lpszHostName, uc.dwHostNameLength * sizeof(wchar_t));
    host[uc.dwHostNameLength] = 0;

    size_t pl = uc.dwUrlPathLength, el = uc.dwExtraInfoLength;
    wchar_t *path = xmalloc((pl + el + 2) * sizeof(wchar_t));
    size_t k = 0;
    if (pl == 0) path[k++] = L'/';
    else { memcpy(path, uc.lpszUrlPath, pl * sizeof(wchar_t)); k = pl; }
    for (size_t i = 0; i < el && uc.lpszExtraInfo[i] != L'#'; i++) path[k++] = uc.lpszExtraInfo[i];
    path[k] = 0;
    int secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
    INTERNET_PORT port = uc.nPort;
    free(wurl);

    r->hConnect = WinHttpConnect(s->h, host, port, 0);
    free(host);
    if (!r->hConnect) goto werr;
    r->hRequest = WinHttpOpenRequest(r->hConnect, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (!r->hRequest) goto werr;
    free(path);
    path = NULL;
    live_add(s, r);

    DWORD feat = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(r->hRequest, WINHTTP_OPTION_DISABLE_FEATURE, &feat, sizeof feat);
    if (s->cfg->insecure) {
        DWORD f = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                  SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(r->hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &f, sizeof f);
    }
    if (flags & HTTP_TEXT) {
        DWORD d = WINHTTP_DECOMPRESSION_FLAG_ALL;
        WinHttpSetOption(r->hRequest, WINHTTP_OPTION_DECOMPRESSION, &d, sizeof d);
    }

    /* request headers: later lines replace earlier ones */
    sbuf hb = { 0 };
    sb_puts(&hb, "Accept: */*\r\n");
    if (!(flags & HTTP_TEXT)) sb_puts(&hb, "Accept-Encoding: identity\r\n");
    /* after a redirect to another host, drop credentials meant for the first one */
    if (hd)
        for (int i = 0; i < hd->n; i++)
            if (!foreign || !is_credential_header(hd->name[i], strlen(hd->name[i])))
                add_header_line(&hb, hd->name[i], hd->value[i]);
    add_header_line(&hb, "Referer", s->cfg->referer);
    char *ck = cookie_header_for(s->cfg, url);      /* scoped per host by itself */
    add_header_line(&hb, "Cookie", ck);
    free(ck);
    for (int i = 0; i < s->cfg->nheaders; i++) {    /* explicit -H headers win */
        const char *h = s->cfg->headers[i];
        if (strpbrk(h, "\r\n")) continue;
        if (foreign && is_credential_header(h, strcspn(h, ":"))) continue;
        sb_printf(&hb, "%s\r\n", h);
    }
    add_header_line(&hb, "User-Agent", s->cfg->user_agent);
    if (from >= 0) {
        if (to >= 0) sb_printf(&hb, "Range: bytes=%lld-%lld\r\n", (long long)from, (long long)to);
        else sb_printf(&hb, "Range: bytes=%lld-\r\n", (long long)from);
    }
    wchar_t *wh = u8_to_w(hb.s);
    sb_free(&hb);
    WinHttpAddRequestHeaders(r->hRequest, wh, (DWORD)-1L,
                             WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    free(wh);

    if (!WinHttpSendRequest(r->hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        goto werr;
    if (!WinHttpReceiveResponse(r->hRequest, NULL)) goto werr;

    DWORD st = 0, sz = sizeof st;
    WinHttpQueryHeaders(r->hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &st, &sz, WINHTTP_NO_HEADER_INDEX);
    r->status = (int)st;

    char *cl = query_header(r->hRequest, WINHTTP_QUERY_CONTENT_LENGTH, 0);
    if (cl) {
        char *e;
        long long v = _strtoi64(cl, &e, 10);
        if (e != cl && v >= 0) r->content_length = v;
        free(cl);
    }
    char *cr = query_header(r->hRequest, WINHTTP_QUERY_CONTENT_RANGE, 0);
    if (cr) {
        /* "bytes 100-199/1000" or "bytes * /1000" (no space) */
        const char *p = cr;
        long long a = -1, b = -1, tot = -1;
        while (*p && !isdigit((unsigned char)*p) && *p != '*') p++;
        if (isdigit((unsigned char)*p)) {
            char *e;
            a = _strtoi64(p, &e, 10);
            if (*e == '-') b = _strtoi64(e + 1, NULL, 10);
        }
        const char *sl = strchr(cr, '/');
        if (sl && isdigit((unsigned char)sl[1])) tot = _strtoi64(sl + 1, NULL, 10);
        /* only trust a self-consistent range */
        if (tot > 0 && a >= 0 && b >= a && b < tot) { r->range_start = a; r->range_total = tot; }
        else if (tot >= 0 && a < 0) r->range_total = tot;
        free(cr);
    }
    r->content_type = query_header(r->hRequest, WINHTTP_QUERY_CONTENT_TYPE, 0);
    r->content_disposition = query_header(r->hRequest, WINHTTP_QUERY_CONTENT_DISPOSITION, 1);
    r->etag = query_header(r->hRequest, WINHTTP_QUERY_ETAG, 0);
    r->last_modified = query_header(r->hRequest, WINHTTP_QUERY_LAST_MODIFIED, 0);
    return 0;

werr:;
    DWORD e = GetLastError();
    win_errstr(e, err, errlen);
    free(path);
    http_close(r);
    return -1;
}

int http_request(http_session *s, const char *url, const ul_headers *hd,
                 int64_t from, int64_t to, int flags, http_response *r,
                 char *err, size_t errlen)
{
    memset(r, 0, sizeof *r);
    r->content_length = -1;
    r->range_start = -1;
    r->range_total = -1;
    err[0] = 0;
    if (str_istarts(url, "ftp://")) return ftp_request(s, url, from, to, r, err, errlen);
    if (!str_istarts(url, "http://") && !str_istarts(url, "https://")) {
        snprintf(err, errlen, "unsupported link type (use http, https or ftp)");
        return -2;
    }

    char *cur = xstrdup(url);
    char *home = origin_host(s, url);
    for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
        char *h = url_host(cur);
        int foreign = _stricmp(h, home) != 0;
        free(h);
        int rq = request_once(s, cur, hd, from, to, flags, foreign, r, err, errlen);
        if (rq != 0) { free(cur); free(home); return rq; }
        int st = r->status;
        if (st == 301 || st == 302 || st == 303 || st == 307 || st == 308) {
            char *loc = query_header(r->hRequest, WINHTTP_QUERY_LOCATION, 1);
            http_close(r);
            r->content_length = r->range_start = r->range_total = -1;
            if (!loc || !*loc) {
                free(loc); free(cur); free(home);
                snprintf(err, errlen, "redirect without a target");
                return -2;
            }
            char *next = url_resolve(cur, str_trim(loc));
            free(loc);
            free(cur);
            cur = next;
            if (str_istarts(cur, "ftp://")) {
                int rc = ftp_request(s, cur, from, to, r, err, errlen);
                free(cur);
                free(home);
                return rc;
            }
            if (!str_istarts(cur, "http://") && !str_istarts(cur, "https://")) {
                free(cur);
                free(home);
                snprintf(err, errlen, "redirected to an unsupported link type");
                return -2;
            }
            continue;
        }
        r->final_url = cur;
        free(home);
        return 0;
    }
    free(cur);
    free(home);
    snprintf(err, errlen, "too many redirects");
    return -2;
}

int http_read(http_response *r, void *buf, DWORD len, DWORD *got)
{
    if (r->ftp) return ftp_read(r, buf, len, got);
    *got = 0;
    return WinHttpReadData(r->hRequest, buf, len, got) ? 0 : -1;
}

void http_close(http_response *r)
{
    if (r->ftp) ftp_close(r);
    int already = r->live ? live_remove((live_req *)r->live) : 0;
    if (r->hRequest && !already) WinHttpCloseHandle(r->hRequest);
    if (r->hConnect) WinHttpCloseHandle(r->hConnect);
    free(r->content_type);
    free(r->content_disposition);
    free(r->etag);
    free(r->last_modified);
    free(r->final_url);
    memset(r, 0, sizeof *r);
}

/* Fetch a whole (small) resource into memory, with retries. */
char *http_get_body(http_session *s, const char *url, const ul_headers *hd,
                    int64_t from, int64_t to, int flags, size_t *outlen,
                    char **final_url, int retries, char *err, size_t errlen,
                    volatile LONG64 *counter)
{
    /* sanity limits: playlists/pages 32 MB, segments/keys/metadata 256 MB */
    const size_t max_body = (flags & HTTP_SMALL) ? (size_t)4 << 20 :
                            (flags & HTTP_TEXT) ? (size_t)32 << 20 : (size_t)256 << 20;
    char *chunk = xmalloc(65536);
    err[0] = 0;
    for (int attempt = 0; attempt <= retries && !http_aborted(s); attempt++) {
        if (attempt) task_sleep(s->task, (DWORD)(attempt < 10 ? 500 * attempt : 5000));
        if (http_aborted(s)) break;
        http_response r;
        int rq = http_request(s, url, hd, from, to, flags, &r, err, errlen);
        if (rq == -2) break;                      /* retrying cannot help */
        if (rq != 0) continue;
        if (r.status < 200 || r.status >= 300) {
            int st = r.status;
            snprintf(err, errlen, "HTTP %d", st);
            http_close(&r);
            if ((st == 401 || st == 403 || st == 404 || st == 410) && attempt >= 1) break;
            continue;
        }
        sbuf b = { 0 };
        int ok = 1;
        for (;;) {
            DWORD got;
            if (http_read(&r, chunk, 65536, &got) != 0) {
                ok = 0;
                win_errstr(GetLastError(), err, errlen);
                break;
            }
            if (!got) break;
            sb_putn(&b, chunk, got);
            if (counter) InterlockedExchangeAdd64(counter, got);
            if (b.len > max_body) { ok = 0; snprintf(err, errlen, "response too large"); break; }
            if (http_aborted(s)) { ok = 0; break; }
        }
        LONG64 counted = (LONG64)b.len;
        int too_large = b.len > max_body;
        /* (with decompression the body is at least as long as the compressed length) */
        if (ok && r.content_length >= 0 && (int64_t)b.len < r.content_length) {
            ok = 0;
            snprintf(err, errlen, "connection closed early");
        }
        if (ok && r.status == 206 && from >= 0 && r.range_start >= 0 && r.range_start != from) {
            ok = 0;
            snprintf(err, errlen, "server sent the wrong byte range");
        }
        /* server ignored our Range header: cut the slice out ourselves */
        if (ok && from >= 0 && (from > 0 || to >= 0) && r.status == 200) {
            if ((int64_t)b.len <= from) {
                ok = 0;
                snprintf(err, errlen, "byte range not available");
            } else {
                size_t n = b.len - (size_t)from;
                if (to >= from && (int64_t)n > to - from + 1) n = (size_t)(to - from + 1);
                memmove(b.s, b.s + from, n);
                b.len = n;
            }
        }
        if (ok && final_url) *final_url = xstrdup(r.final_url ? r.final_url : url);
        http_close(&r);
        if (counter && !ok) InterlockedExchangeAdd64(counter, -counted);   /* attempt discarded */
        if (ok) {
            free(chunk);
            *outlen = b.len;
            return sb_steal(&b);
        }
        sb_free(&b);
        if (too_large) break;                     /* the same thing would happen again */
    }
    free(chunk);
    return NULL;
}
