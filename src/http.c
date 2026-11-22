/*
 * U-loader - HTTP(S) client built on WinHTTP (ships with Windows; handles
 * TLS and proxies without any third-party library). Redirects are followed
 * here, not by WinHTTP, so cookies are only ever sent to the hosts they
 * belong to. ftp:// URLs are handed to ftp.c behind the same interface.
 */
#include "uloader.h"
#include <errno.h>

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
   it from another thread: a worker blocked in a read then returns at once
   instead of after the network timeout. After a cancel the worker never
   touches that handle again (it checks the flag before every call). */
typedef struct live_req {
    struct live_req *next;
    http_session    *s;
    HINTERNET        h;
    volatile LONG    closed;     /* closed by http_session_cancel */
} live_req;

/* A cookie a server set during this download (Set-Cookie). WinHTTP's own
   cookie store is per WinHTTP session - shared by every download here -
   so it is switched off and each download keeps its own little jar. */
typedef struct {
    char *domain, *name, *value;
    int   host_only, secure;
} jar_cookie;

#define JAR_MAX 64

struct http_session {
    HINTERNET        h;          /* shared, see shared_session() */
    int              own_h;      /* 1 = not shared: closed with the session */
    int              timeout_ms;
    const ul_config *cfg;
    ul_task         *task;
    CRITICAL_SECTION lock;
    live_req        *live;
    char            *origin;     /* "scheme://host:port" the credentials belong to */
    jar_cookie       jar[JAR_MAX];
    int              njar;
};

/* Year in an Expires date ("Wed, 21 Oct 2015 07:28:00 GMT"), 0 = none */
static int expires_year(const char *v)
{
    for (const char *p = v; *p; p++)
        if (isdigit((unsigned char)p[0]) && isdigit((unsigned char)p[1]) && isdigit((unsigned char)p[2]) &&
            isdigit((unsigned char)p[3]) && !isdigit((unsigned char)p[4]) && (p == v || !isdigit((unsigned char)p[-1])))
            return atoi(p);
    return 0;
}

/* remember one Set-Cookie answer from `url` */
static void jar_store(http_session *s, const char *url, const char *sc)
{
    size_t nl = strcspn(sc, ";");
    char *pair = str_trim(xstrndup(sc, nl));
    char *eq = strchr(pair, '=');
    if (!eq || eq == pair || strpbrk(pair, "\r\n")) { free(pair); return; }
    *eq = 0;
    char *name = xstrdup(str_trim(pair)), *value = xstrdup(str_trim(eq + 1));
    free(pair);
    char *host = url_host(url), *domain = NULL;
    int secure = 0, drop = 0;
    for (const char *p = sc + nl; *p == ';';) {
        p++;
        size_t al = strcspn(p, ";");
        char *attr = str_trim(xstrndup(p, al));
        if (!_strnicmp(attr, "domain=", 7)) {
            const char *d = attr + 7;
            while (*d == '.') d++;
            free(domain);
            domain = *d ? xstrdup(d) : NULL;
        } else if (!_stricmp(attr, "secure")) {
            secure = 1;
        } else if (!_strnicmp(attr, "max-age=", 8)) {
            if (atoi(attr + 8) <= 0) drop = 1;
        } else if (!_strnicmp(attr, "expires=", 8)) {
            SYSTEMTIME st;
            GetSystemTime(&st);
            int y = expires_year(attr + 8);
            if (y && y < st.wYear) drop = 1;             /* "delete this cookie" */
        }
        free(attr);
        p += al;
    }
    /* a site may set a cookie for itself or a parent domain, never another */
    int host_only = !domain;
    if (domain && !host_matches(host, domain)) { free(name); free(value); free(domain); free(host); return; }
    if (!domain) domain = xstrdup(host);
    free(host);
    EnterCriticalSection(&s->lock);
    int i = 0;
    while (i < s->njar && !(!_stricmp(s->jar[i].domain, domain) && !strcmp(s->jar[i].name, name))) i++;
    if (i < s->njar) {                                 /* replace (or delete) */
        free(s->jar[i].domain); free(s->jar[i].name); free(s->jar[i].value);
        memmove(&s->jar[i], &s->jar[i + 1], sizeof *s->jar * (size_t)(s->njar - i - 1));
        s->njar--;
    }
    if (!drop) {
        if (s->njar == JAR_MAX) {                      /* full: forget the oldest */
            free(s->jar[0].domain); free(s->jar[0].name); free(s->jar[0].value);
            memmove(&s->jar[0], &s->jar[1], sizeof *s->jar * (size_t)(JAR_MAX - 1));
            s->njar--;
        }
        jar_cookie *c = &s->jar[s->njar++];
        c->domain = domain; c->name = name; c->value = value;
        c->host_only = host_only; c->secure = secure;
        domain = name = value = NULL;
    }
    LeaveCriticalSection(&s->lock);
    free(domain); free(name); free(value);
}

/* "a=1; b=2" of the jar cookies for `url`, NULL if none */
static char *jar_header(http_session *s, const char *url)
{
    char *host = url_host(url);
    int https = str_istarts(url, "https://");
    sbuf b = { 0 };
    EnterCriticalSection(&s->lock);
    for (int i = 0; i < s->njar; i++) {
        const jar_cookie *c = &s->jar[i];
        int ok = c->host_only ? !_stricmp(host, c->domain) : host_matches(host, c->domain);
        if (!ok || (c->secure && !https)) continue;
        if (b.len) sb_puts(&b, "; ");
        sb_printf(&b, "%s=%s", c->name, c->value);
    }
    LeaveCriticalSection(&s->lock);
    free(host);
    return b.len ? sb_steal(&b) : (sb_free(&b), NULL);
}

/* One WinHTTP session per proxy setting for the whole process. Every
   WinHttpOpen / close pair leaks a few handles inside WinHTTP itself
   (about one per two downloads, never given back), which adds up in a
   window that runs for weeks. Downloads stay separate all the same:
   timeouts are set per request and cookies are kept per download. */
static SRWLOCK g_sess_lock = SRWLOCK_INIT;
static struct { char *proxy; HINTERNET h; } g_sess[8];
static int g_nsess;

static HINTERNET open_winhttp(const char *proxy)
{
    HINTERNET h;
    wchar_t *ua = u8_to_w(UL_DEFAULT_UA);             /* each request names its own User-Agent */
    if (proxy && *proxy) {
        wchar_t *px = u8_to_w(proxy);
        h = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_NAMED_PROXY, px, L"<local>", 0);
        free(px);
    } else {
        /* Windows 8.1+: follow the system proxy settings automatically */
        h = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!h) h = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    free(ua);
    if (h) {
        /* We want many parallel TCP connections to the same server. HTTP/2
           is left off on purpose: it would multiplex everything over one socket. */
        DWORD v = 128;
        WinHttpSetOption(h, WINHTTP_OPTION_MAX_CONNS_PER_SERVER, &v, sizeof v);
        WinHttpSetOption(h, WINHTTP_OPTION_MAX_CONNS_PER_1_0_SERVER, &v, sizeof v);
    }
    return h;
}

static HINTERNET shared_session(const char *proxy, int *own)
{
    const char *key = proxy && *proxy ? proxy : "";
    *own = 0;
    AcquireSRWLockExclusive(&g_sess_lock);
    for (int i = 0; i < g_nsess; i++)
        if (!strcmp(g_sess[i].proxy, key)) {
            HINTERNET h = g_sess[i].h;
            ReleaseSRWLockExclusive(&g_sess_lock);
            return h;
        }
    HINTERNET h = open_winhttp(key);
    if (h && g_nsess < (int)(sizeof g_sess / sizeof *g_sess)) {
        g_sess[g_nsess].proxy = xstrdup(key);
        g_sess[g_nsess].h = h;
        g_nsess++;
    } else if (h) {
        *own = 1;                                      /* table full: this one is private */
    }
    ReleaseSRWLockExclusive(&g_sess_lock);
    return h;
}

void http_session_set_origin(http_session *s, const char *url)
{
    char *o = url ? url_origin(url) : NULL;
    EnterCriticalSection(&s->lock);
    free(s->origin);
    s->origin = o;
    LeaveCriticalSection(&s->lock);
}

/* origin whose requests may carry Authorization / Cookie headers */
static char *origin_of(http_session *s, const char *url)
{
    EnterCriticalSection(&s->lock);
    char *o = s->origin ? xstrdup(s->origin) : NULL;
    LeaveCriticalSection(&s->lock);
    return o ? o : url_origin(url);
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

/* 1 if a pause closed this request: its handle must not be used again */
static int live_cancelled(const http_response *r)
{
    return r->live && ((live_req *)r->live)->closed;
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
    /* mark under the lock, close outside it (closing may wait for WinHTTP) */
    HINTERNET todo[256];
    int n = 0;
    EnterCriticalSection(&s->lock);
    for (live_req *e = s->live; e && n < 256; e = e->next)
        if (!e->closed) { InterlockedExchange(&e->closed, 1); todo[n++] = e->h; }
    LeaveCriticalSection(&s->lock);
    for (int i = 0; i < n; i++) WinHttpCloseHandle(todo[i]);
}

http_session *http_session_new(const ul_config *cfg, ul_task *t)
{
    http_session *s = xcalloc(1, sizeof *s);
    s->cfg = cfg;
    s->task = t;
    InitializeCriticalSection(&s->lock);
    s->h = shared_session(cfg->proxy, &s->own_h);
    if (!s->h) { DeleteCriticalSection(&s->lock); free(s); return NULL; }
    int secs = cfg->timeout_sec < 5 ? 5 : cfg->timeout_sec > 3600 ? 3600 : cfg->timeout_sec;
    s->timeout_ms = secs * 1000;                       /* applied to each request */
    return s;
}

void http_session_free(http_session *s)
{
    if (!s) return;
    if (s->own_h) WinHttpCloseHandle(s->h);
    for (int i = 0; i < s->njar; i++) { free(s->jar[i].domain); free(s->jar[i].name); free(s->jar[i].value); }
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

/* ULOADER_HTTP_TRACE=<file>: log every exchange (headers exactly as WinHTTP
   sent them, and the server's answer). Cookie / Authorization / Set-Cookie
   values are blanked, so the log can be shared when reporting a problem. */
static void trace_exchange(HINTERNET h, const char *url)
{
    static volatile LONG checked;
    static char tpath[1024];
    static CRITICAL_SECTION lock;
    if (InterlockedCompareExchange(&checked, 1, 0) == 0) {
        InitializeCriticalSection(&lock);
        DWORD n = GetEnvironmentVariableA("ULOADER_HTTP_TRACE", tpath, sizeof tpath);
        if (!n || n >= sizeof tpath) tpath[0] = 0;
        InterlockedExchange(&checked, 2);
    }
    while (checked == 1) Sleep(1);
    if (!tpath[0]) return;
    const DWORD kinds[2] = { WINHTTP_QUERY_RAW_HEADERS_CRLF | WINHTTP_QUERY_FLAG_REQUEST_HEADERS,
                             WINHTTP_QUERY_RAW_HEADERS_CRLF };
    sbuf out = { 0 };
    sb_printf(&out, "=== %s\n", url);
    for (int k = 0; k < 2; k++) {
        DWORD len = 0;
        WinHttpQueryHeaders(h, kinds[k], WINHTTP_HEADER_NAME_BY_INDEX, NULL, &len, WINHTTP_NO_HEADER_INDEX);
        if (!len || len > (1u << 20)) continue;
        wchar_t *w = xmalloc(len + sizeof(wchar_t));
        if (WinHttpQueryHeaders(h, kinds[k], WINHTTP_HEADER_NAME_BY_INDEX, w, &len, WINHTTP_NO_HEADER_INDEX)) {
            w[len / sizeof(wchar_t)] = 0;
            char *u = w_to_u8(w), *save = NULL;
            for (char *line = strtok_r(u, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
                const char *colon = strchr(line, ':');
                int secret = colon && (str_istarts(line, "Cookie:") || str_istarts(line, "Set-Cookie:") ||
                                       str_istarts(line, "Authorization:") || str_istarts(line, "Proxy-Authorization:"));
                if (secret) sb_printf(&out, "%s %.*s: <%d bytes hidden>\n", k ? "<" : ">", (int)(colon - line), line,
                                      (int)strlen(colon + 1));
                else sb_printf(&out, "%s %s\n", k ? "<" : ">", line);
            }
            free(u);
        }
        free(w);
    }
    EnterCriticalSection(&lock);
    FILE *f = fopen_u8(tpath, "ab");
    if (f) { fwrite(out.s, 1, out.len, f); fclose(f); }
    LeaveCriticalSection(&lock);
    sb_free(&out);
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
    DWORD cerr = GetLastError();           /* before free() can change it */
    free(host);
    if (!r->hConnect) { SetLastError(cerr); goto werr; }
    r->hRequest = WinHttpOpenRequest(r->hConnect, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (!r->hRequest) goto werr;
    free(path);
    path = NULL;
    live_add(s, r);

    /* redirects are followed here; cookies are kept per download (s->jar),
       not in WinHTTP's store that every download of this process shares */
    DWORD feat = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES;
    WinHttpSetOption(r->hRequest, WINHTTP_OPTION_DISABLE_FEATURE, &feat, sizeof feat);
    WinHttpSetTimeouts(r->hRequest, s->timeout_ms, s->timeout_ms, s->timeout_ms, s->timeout_ms);
    if (s->cfg->insecure) {
        DWORD f = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                  SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(r->hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &f, sizeof f);
    }
    if (flags & HTTP_TEXT) {
        DWORD d = WINHTTP_DECOMPRESSION_FLAG_ALL;
        WinHttpSetOption(r->hRequest, WINHTTP_OPTION_DECOMPRESSION, &d, sizeof d);
    }

    /* Request headers. Merged first (a later one replaces an earlier one with
       the same name), then added one by one: a single empty or malformed
       header can no longer make WinHTTP drop all the others (Range, Cookie...). */
    ul_headers hs;
    memset(&hs, 0, sizeof hs);
    headers_set(&hs, "Accept", "*/*");
    if (!(flags & HTTP_TEXT)) headers_set(&hs, "Accept-Encoding", "identity");
    if (hd)                                  /* site headers (yt-dlp) */
        for (int i = 0; i < hd->n; i++)
            if (!foreign || !is_credential_header(hd->name[i], strlen(hd->name[i])))
                headers_set(&hs, hd->name[i], hd->value[i]);
    if (s->cfg->referer && *s->cfg->referer) headers_set(&hs, "Referer", s->cfg->referer);
    char *ck = cookie_header_for(s->cfg, url, !foreign);   /* the jar scopes itself per host */
    char *jk = jar_header(s, url);                           /* set by the site during this download */
    if (ck && jk) {
        char *both = xasprintf("%s; %s", ck, jk);
        free(ck);
        ck = both;
    } else if (jk) {
        ck = jk;
        jk = NULL;
    }
    free(jk);
    if (ck) headers_set(&hs, "Cookie", ck);
    free(ck);
    for (int i = 0; i < s->cfg->nheaders; i++) {             /* explicit -H headers win */
        const char *h = s->cfg->headers[i];
        const char *colon = strchr(h, ':');
        if (!colon) continue;
        char *name = str_trim(xstrndup(h, (size_t)(colon - h)));
        char *value = str_trim(xstrdup(colon + 1));
        if (!(foreign && is_credential_header(name, strlen(name)))) headers_set(&hs, name, value);
        free(name);
        free(value);
    }
    if (s->cfg->user_agent && *s->cfg->user_agent) headers_set(&hs, "User-Agent", s->cfg->user_agent);
    if (from >= 0) {
        char range[64];
        if (to >= 0) snprintf(range, sizeof range, "bytes=%lld-%lld", (long long)from, (long long)to);
        else snprintf(range, sizeof range, "bytes=%lld-", (long long)from);
        headers_set(&hs, "Range", range);
    }
    for (int i = 0; i < hs.n; i++) {
        if (!*hs.name[i] || !*hs.value[i] || strpbrk(hs.name[i], " \t")) continue;   /* empty = "delete" to WinHTTP */
        char *line = xasprintf("%s: %s", hs.name[i], hs.value[i]);
        wchar_t *wl = u8_to_w(line);
        WinHttpAddRequestHeaders(r->hRequest, wl, (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        free(wl);
        free(line);
    }
    headers_free(&hs);

    if (live_cancelled(r)) { SetLastError(ERROR_WINHTTP_OPERATION_CANCELLED); goto werr; }
    if (!WinHttpSendRequest(r->hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        goto werr;
    if (live_cancelled(r)) { SetLastError(ERROR_WINHTTP_OPERATION_CANCELLED); goto werr; }
    if (!WinHttpReceiveResponse(r->hRequest, NULL)) goto werr;
    if (live_cancelled(r)) { SetLastError(ERROR_WINHTTP_OPERATION_CANCELLED); goto werr; }
    trace_exchange(r->hRequest, url);

    /* cookies the site sets (on a redirect, a confirmation page...) come
       back with the next requests of this download */
    for (DWORD idx = 0; idx < JAR_MAX;) {
        wchar_t sc[4096];
        DWORD len = sizeof sc, before = idx;
        if (!WinHttpQueryHeaders(r->hRequest, WINHTTP_QUERY_SET_COOKIE, WINHTTP_HEADER_NAME_BY_INDEX, sc, &len, &idx))
            break;                                     /* none left (or one too long: skipped) */
        char *u8 = w_to_u8(sc);
        jar_store(s, url, u8);
        free(u8);
        if (idx == before) break;
    }

    DWORD st = 0, sz = sizeof st;
    WinHttpQueryHeaders(r->hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &st, &sz, WINHTTP_NO_HEADER_INDEX);
    r->status = (int)st;

    char *cl = query_header(r->hRequest, WINHTTP_QUERY_CONTENT_LENGTH, 0);
    if (cl) {
        /* digits only: "100, 200" or "12abc" is not a length */
        char *t = str_trim(cl), *e;
        errno = 0;
        long long v = _strtoi64(t, &e, 10);
        if (isdigit((unsigned char)*t) && !*e && errno != ERANGE && v >= 0) r->content_length = v;
        free(cl);
    }
    char *cr = query_header(r->hRequest, WINHTTP_QUERY_CONTENT_RANGE, 0);
    if (cr) {
        /* "bytes 100-199/1000", "bytes 0-99/<star>" or "bytes <star>/1000" */
        const char *p = cr;
        long long a = -1, b = -1, tot = -1;
        while (*p == ' ') p++;
        if (_strnicmp(p, "bytes", 5) == 0) {
            p += 5;
            while (*p == ' ') p++;
            char *e;
            if (isdigit((unsigned char)*p)) {
                a = _strtoi64(p, &e, 10);
                if (*e == '-' && isdigit((unsigned char)e[1])) b = _strtoi64(e + 1, &e, 10);
                else a = -1;
                p = e;
            } else if (*p == '*') {
                p++;
            } else {
                p = "";
            }
            if (*p == '/' && isdigit((unsigned char)p[1])) {
                tot = _strtoi64(p + 1, &e, 10);
                if (*e) tot = -1;
            }
        }
        /* only trust a self-consistent range */
        if (tot > 0 && a >= 0 && b >= a && b < tot && a < INT64_MAX && b < INT64_MAX && tot < INT64_MAX) {
            r->range_start = a;
            r->range_total = tot;
        } else if (tot >= 0 && a < 0 && b < 0) {
            r->range_total = tot;
        }
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
    char *home = origin_of(s, url);
    for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
        /* credentials belong to one origin: another host, a downgrade from
           https to http, or another port gets none */
        char *o = url_origin(cur);
        int foreign = _stricmp(o, home) != 0;
        free(o);
        int rq = request_once(s, cur, hd, from, to, flags, foreign, r, err, errlen);
        if (rq != 0) { free(cur); free(home); return rq; }
        int st = r->status;
        if (st == 301 || st == 302 || st == 303 || st == 307 || st == 308) {
            char *loc = query_header(r->hRequest, WINHTTP_QUERY_LOCATION, 1);
            http_close(r);
            r->content_length = r->range_start = r->range_total = -1;
            if (loc) str_trim(loc);
            if (!loc || !*loc) {
                free(loc); free(cur); free(home);
                snprintf(err, errlen, "redirect without a target");
                return -2;
            }
            char *next = url_resolve(cur, loc);
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
    /* closed by a pause: the handle is gone, never pass it to WinHTTP again */
    if (live_cancelled(r)) { SetLastError(ERROR_WINHTTP_OPERATION_CANCELLED); return -1; }
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
        /* a partial answer must be exactly the part we asked for */
        if (ok && r.status == 206 && from >= 0) {
            int64_t want = to >= from ? to - from + 1 : -1;
            if (r.range_start != from || (want > 0 && (int64_t)b.len != want)) {
                ok = 0;
                snprintf(err, errlen, "server sent the wrong byte range");
            }
        }
        /* server ignored our Range header: cut the slice out ourselves */
        if (ok && from >= 0 && (from > 0 || to >= 0) && r.status == 200) {
            if ((int64_t)b.len <= from) {
                ok = 0;
                snprintf(err, errlen, "byte range not available");
            } else {
                size_t n = b.len - (size_t)from;
                if (to >= from && to < INT64_MAX && (int64_t)n > to - from + 1) n = (size_t)(to - from + 1);
                if (counter) InterlockedExchangeAdd64(counter, -(LONG64)(b.len - n));   /* bytes thrown away */
                memmove(b.s, b.s + from, n);
                b.len = n;
                b.s[n] = 0;                        /* still a C string for playlists */
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
