/*
 * U-loader - helpers: memory, strings, UTF-8, JSON, cookies, files,
 * console and child processes.
 */
#include "uloader.h"
#include <shlobj.h>
#include <time.h>

volatile LONG g_abort = 0;
volatile LONG g_busy  = 0;
int g_vt = 0;
int g_tty = 0;
static int g_progress_open = 0;

/* ------------------------------------------------------------------ */
/* memory                                                              */
/* ------------------------------------------------------------------ */
static void die_oom(void) { fputs("out of memory\n", stderr); ExitProcess(3); }

void *xmalloc(size_t n)            { void *p = malloc(n ? n : 1); if (!p) die_oom(); return p; }
void *xcalloc(size_t a, size_t b)  { void *p = calloc(a ? a : 1, b ? b : 1); if (!p) die_oom(); return p; }
void *xrealloc(void *p, size_t n)  { p = realloc(p, n ? n : 1); if (!p) die_oom(); return p; }
char *xstrdup(const char *s)       { return xstrndup(s, strlen(s)); }

char *xstrndup(const char *s, size_t n)
{
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

char *xasprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    char *p = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(p, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return p;
}

/* ------------------------------------------------------------------ */
/* UTF-8 <-> UTF-16                                                    */
/* ------------------------------------------------------------------ */
wchar_t *u8_to_w(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) { wchar_t *w = xmalloc(sizeof(wchar_t)); w[0] = 0; return w; }
    wchar_t *w = xmalloc((size_t)n * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

char *w_to_u8(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return xstrdup("");
    char *s = xmalloc((size_t)n);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

/* Paths longer than MAX_PATH need the \\?\ prefix (and must be absolute). */
wchar_t *wpath(const char *p)
{
    wchar_t *w = u8_to_w(p);
    if (wcslen(w) < 240 || wcsncmp(w, L"\\\\?\\", 4) == 0) return w;
    DWORD n = GetFullPathNameW(w, 0, NULL, NULL);
    if (!n) return w;
    wchar_t *full = xmalloc(((size_t)n + 1) * sizeof(wchar_t));
    if (!GetFullPathNameW(w, n + 1, full, NULL)) { free(full); return w; }
    free(w);
    wchar_t *r;
    if (full[0] == L'\\' && full[1] == L'\\') {      /* \\server\share -> \\?\UNC\server\share */
        r = xmalloc((wcslen(full) + 8) * sizeof(wchar_t));
        wcscpy(r, L"\\\\?\\UNC");
        wcscat(r, full + 1);
    } else {
        r = xmalloc((wcslen(full) + 5) * sizeof(wchar_t));
        wcscpy(r, L"\\\\?\\");
        wcscat(r, full);
    }
    free(full);
    return r;
}

int utf8_valid(const unsigned char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i];
        int extra;
        if (c < 0x80) { i++; continue; }
        else if ((c & 0xE0) == 0xC0) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0) extra = 3;
        else return 0;
        if (i + (size_t)extra >= n) return 0;
        for (int k = 1; k <= extra; k++)
            if ((s[i + (size_t)k] & 0xC0) != 0x80) return 0;
        i += (size_t)extra + 1;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* string buffer                                                       */
/* ------------------------------------------------------------------ */
static void sb_grow(sbuf *b, size_t add)
{
    if (b->len + add + 1 <= b->cap) return;
    size_t nc = b->cap ? b->cap * 2 : 256;
    while (nc < b->len + add + 1) nc *= 2;
    b->s = xrealloc(b->s, nc);
    b->cap = nc;
}

void sb_putn(sbuf *b, const char *s, size_t n)
{
    sb_grow(b, n);
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}

void sb_puts(sbuf *b, const char *s) { sb_putn(b, s, strlen(s)); }
void sb_putc(sbuf *b, char c)        { sb_putn(b, &c, 1); }

void sb_printf(sbuf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    sb_grow(b, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(b->s + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

char *sb_steal(sbuf *b)
{
    char *s = b->s ? b->s : xstrdup("");
    b->s = NULL;
    b->len = b->cap = 0;
    return s;
}

void sb_free(sbuf *b) { free(b->s); b->s = NULL; b->len = b->cap = 0; }

/* ------------------------------------------------------------------ */
/* strings                                                             */
/* ------------------------------------------------------------------ */
int str_istarts(const char *s, const char *prefix)
{
    return _strnicmp(s, prefix, strlen(prefix)) == 0;
}

int str_iends(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && _stricmp(s + a - b, suffix) == 0;
}

const char *str_istr(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (!n) return hay;
    for (; *hay; hay++)
        if (_strnicmp(hay, needle, n) == 0) return hay;
    return NULL;
}

char *str_trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    return s;
}

const char *fmt_size(int64_t n, char *buf, size_t len)
{
    static const char *u[] = { "B", "KB", "MB", "GB", "TB" };
    if (n < 0) { snprintf(buf, len, "?"); return buf; }
    double v = (double)n;
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    if (i == 0) snprintf(buf, len, "%lld B", (long long)n);
    else snprintf(buf, len, v < 10 ? "%.2f %s" : v < 100 ? "%.1f %s" : "%.0f %s", v, u[i]);
    return buf;
}

const char *fmt_eta(double sec, char *buf, size_t len)
{
    if (sec < 0 || sec > 99.0 * 3600) { snprintf(buf, len, "--:--"); return buf; }
    long s = (long)(sec + 0.5);
    if (s >= 3600) snprintf(buf, len, "%ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
    else snprintf(buf, len, "%02ld:%02ld", s / 60, s % 60);
    return buf;
}

int64_t parse_size(const char *s)
{
    char *end;
    double v = strtod(s, &end);
    if (end == s || v < 0) return -1;
    switch (toupper((unsigned char)*end)) {
    case 'K': v *= 1024.0; break;
    case 'M': v *= 1048576.0; break;
    case 'G': v *= 1073741824.0; break;
    case 0: case 'B': break;
    default: return -1;
    }
    return (int64_t)v;
}

double now_sec(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}

const char *win_errstr(DWORD code, char *buf, size_t len)
{
    wchar_t w[512];
    DWORD n;
    if (code >= 12000 && code <= 12999)
        n = FormatMessageW(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
                           GetModuleHandleW(L"winhttp.dll"), code, 0, w, 512, NULL);
    else
        n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                           NULL, code, 0, w, 512, NULL);
    if (!n) { snprintf(buf, len, "system error %lu", (unsigned long)code); return buf; }
    while (n && (w[n - 1] == L'\r' || w[n - 1] == L'\n' || w[n - 1] == L'.' || w[n - 1] == L' '))
        w[--n] = 0;
    char *u = w_to_u8(w);
    snprintf(buf, len, "%s (%lu)", u, (unsigned long)code);
    free(u);
    return buf;
}

/* ------------------------------------------------------------------ */
/* JSON                                                                */
/* ------------------------------------------------------------------ */
const char *json_skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

static void put_utf8(sbuf *b, unsigned cp)
{
    char t[4];
    if (cp < 0x80) { sb_putc(b, (char)cp); return; }
    if (cp < 0x800) { t[0] = (char)(0xC0 | (cp >> 6)); t[1] = (char)(0x80 | (cp & 0x3F)); sb_putn(b, t, 2); return; }
    if (cp < 0x10000) {
        t[0] = (char)(0xE0 | (cp >> 12)); t[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        t[2] = (char)(0x80 | (cp & 0x3F)); sb_putn(b, t, 3); return;
    }
    t[0] = (char)(0xF0 | (cp >> 18)); t[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    t[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); t[3] = (char)(0x80 | (cp & 0x3F));
    sb_putn(b, t, 4);
}

static int hex4(const char *p, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        int c = p[i];
        int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
                (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0) return 0;
        v = v * 16 + (unsigned)d;
    }
    *out = v;
    return 1;
}

int json_parse_string(const char **pp, sbuf *out)
{
    const char *p = *pp;
    if (*p != '"') return 0;
    p++;
    while (*p && *p != '"') {
        if (*p != '\\') { sb_putc(out, *p++); continue; }
        p++;
        switch (*p) {
        case 'n': sb_putc(out, '\n'); break;
        case 't': sb_putc(out, '\t'); break;
        case 'r': sb_putc(out, '\r'); break;
        case 'b': sb_putc(out, '\b'); break;
        case 'f': sb_putc(out, '\f'); break;
        case 'u': {
            unsigned cp;
            if (!hex4(p + 1, &cp)) return 0;
            p += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && p[1] == '\\' && p[2] == 'u') {
                unsigned lo;
                if (hex4(p + 3, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
            }
            if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;   /* lone surrogate */
            if (cp == 0) cp = 0xFFFD;                        /* no embedded NULs */
            put_utf8(out, cp);
            break;
        }
        case 0: return 0;
        default: sb_putc(out, *p); break;   /* \" \\ \/ */
        }
        p++;
    }
    if (*p != '"') return 0;
    *pp = p + 1;
    return 1;
}

const char *json_skip(const char *p)
{
    p = json_skip_ws(p);
    if (*p == '"') {
        for (p++; *p && *p != '"'; p++)
            if (*p == '\\' && p[1]) p++;
        return *p == '"' ? p + 1 : NULL;
    }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        for (; *p; p++) {
            if (*p == '"') {
                for (p++; *p && *p != '"'; p++)
                    if (*p == '\\' && p[1]) p++;
                if (!*p) return NULL;
            } else if (*p == '{' || *p == '[') {
                depth++;
            } else if (*p == '}' || *p == ']') {
                if (--depth == 0) return p + 1;
            }
        }
        return NULL;
    }
    const char *s = p;
    while (*p && *p != ',' && *p != '}' && *p != ']' && !isspace((unsigned char)*p)) p++;
    return p > s ? p : NULL;
}

const char *json_find(const char *obj, const char *key)
{
    const char *p = json_skip_ws(obj);
    if (*p != '{') return NULL;
    p++;
    for (;;) {
        p = json_skip_ws(p);
        if (*p == '}' || !*p) return NULL;
        sbuf k = { 0 };
        if (!json_parse_string(&p, &k)) { sb_free(&k); return NULL; }
        p = json_skip_ws(p);
        if (*p != ':') { sb_free(&k); return NULL; }
        p = json_skip_ws(p + 1);
        int hit = k.s && strcmp(k.s, key) == 0;
        if (!k.s && !*key) hit = 1;
        sb_free(&k);
        if (hit) return p;
        p = json_skip(p);
        if (!p) return NULL;
        p = json_skip_ws(p);
        if (*p == ',') p++;
        else return NULL;
    }
}

char *json_str(const char *val)
{
    if (!val) return NULL;
    const char *p = json_skip_ws(val);
    if (*p == '"') {
        sbuf b = { 0 };
        if (!json_parse_string(&p, &b)) { sb_free(&b); return NULL; }
        return sb_steal(&b);
    }
    if (strncmp(p, "null", 4) == 0) return NULL;
    const char *e = json_skip(p);
    if (!e || *p == '{' || *p == '[') return NULL;
    return xstrndup(p, (size_t)(e - p));
}

void json_put_string(sbuf *b, const char *s)
{
    sb_putc(b, '"');
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
        case '"':  sb_puts(b, "\\\""); break;
        case '\\': sb_puts(b, "\\\\"); break;
        case '\n': sb_puts(b, "\\n"); break;
        case '\r': sb_puts(b, "\\r"); break;
        case '\t': sb_puts(b, "\\t"); break;
        default:
            if (*p < 0x20) sb_printf(b, "\\u%04x", *p);
            else sb_putc(b, (char)*p);
        }
    }
    sb_putc(b, '"');
}

/* ------------------------------------------------------------------ */
/* cookies                                                             */
/* ------------------------------------------------------------------ */
int host_matches(const char *host, const char *domain)
{
    if (!host || !domain || !*domain) return 0;
    if (*domain == '.') domain++;
    size_t hl = strlen(host), dl = strlen(domain);
    if (hl == dl) return _stricmp(host, domain) == 0;
    return hl > dl && host[hl - dl - 1] == '.' && _stricmp(host + hl - dl, domain) == 0;
}

/* Netscape cookies.txt: domain, include-subdomains, path, secure, expiry, name, value */
int cookies_load(ul_config *cfg, const char *path)
{
    size_t len;
    char *data = read_file(path, &len);
    if (!data) return -1;
    char *cur = data;
    int added = 0;
    while (*cur) {
        char *nl = strchr(cur, '\n');
        if (nl) *nl = 0;
        char *line = cur;
        cur = nl ? nl + 1 : cur + strlen(cur);
        size_t ll = strlen(line);
        if (ll && line[ll - 1] == '\r') line[--ll] = 0;
        if (strncmp(line, "#HttpOnly_", 10) == 0) line += 10;
        else if (line[0] == '#' || !line[0]) continue;
        char *f[7];
        int k = 0;
        char *p = line;
        for (; k < 6; k++) {
            char *t = strchr(p, '\t');
            if (!t) break;
            *t = 0;
            f[k] = p;
            p = t + 1;
        }
        if (k < 6) continue;
        f[6] = p;
        /* refuse domain cookies for a whole top-level domain (".com") or an
           IP fragment (".0.0.1"): they would go to unrelated sites */
        if (_stricmp(f[1], "TRUE") == 0 || f[0][0] == '.') {
            const char *dom = f[0][0] == '.' ? f[0] + 1 : f[0];
            int all_numeric = 1;
            for (const char *q = dom; *q; q++) if (!isdigit((unsigned char)*q) && *q != '.') all_numeric = 0;
            if (!strchr(dom, '.') || all_numeric) continue;
        }
        cfg->cookies = xrealloc(cfg->cookies, sizeof(ul_cookie) * (size_t)(cfg->ncookies + 1));
        ul_cookie *c = &cfg->cookies[cfg->ncookies++];
        int sub = _stricmp(f[1], "TRUE") == 0;
        c->domain = (sub && f[0][0] != '.') ? xasprintf(".%s", f[0]) : xstrdup(f[0]);
        if (!sub && c->domain[0] == '.') memmove(c->domain, c->domain + 1, strlen(c->domain));
        c->path = xstrdup(*f[2] ? f[2] : "/");
        c->secure = _stricmp(f[3], "TRUE") == 0;
        c->expires = _strtoi64(f[4], NULL, 10);
        c->name = xstrdup(f[5]);
        c->value = xstrdup(f[6]);
        added++;
    }
    free(data);
    return added;
}

char *cookie_header_for(const ul_config *cfg, const char *url)
{
    char *host = url_host(url);
    sbuf b = { 0 };
    int https = str_istarts(url, "https://");
    if (cfg->cookie && *cfg->cookie && (!cfg->cookie_host || host_matches(host, cfg->cookie_host)))
        sb_puts(&b, cfg->cookie);
    if (cfg->ncookies) {
        const char *ps = strstr(url, "://");
        ps = ps ? ps + 3 : url;
        ps += strcspn(ps, "/?#");
        size_t pl = strcspn(ps, "?#");
        char *path = pl ? xstrndup(ps, pl) : xstrdup("/");
        time_t now = time(NULL);
        for (int i = 0; i < cfg->ncookies; i++) {
            const ul_cookie *c = &cfg->cookies[i];
            int ok = c->domain[0] == '.' ? host_matches(host, c->domain) : _stricmp(host, c->domain) == 0;
            if (!ok || (c->secure && !https)) continue;
            if (c->expires > 0 && c->expires < (int64_t)now) continue;
            size_t cpl = strlen(c->path);
            if (strncmp(path, c->path, cpl) != 0) continue;
            if (cpl && c->path[cpl - 1] != '/' && path[cpl] && path[cpl] != '/') continue;
            if (b.len) sb_puts(&b, "; ");
            sb_printf(&b, "%s=%s", c->name, c->value);
        }
        free(path);
    }
    free(host);
    return b.len ? sb_steal(&b) : (sb_free(&b), NULL);
}

/* ------------------------------------------------------------------ */
/* configuration copies (one per GUI download)                         */
/* ------------------------------------------------------------------ */
void cfg_defaults(ul_config *c)
{
    memset(c, 0, sizeof *c);
    c->connections = 8;
    c->retries = 10;
    c->timeout_sec = 30;
}

static char *dupn(const char *s) { return s ? xstrdup(s) : NULL; }

ul_config *cfg_clone(const ul_config *s)
{
    ul_config *c = xmalloc(sizeof *c);
    *c = *s;
    c->out_name = dupn(s->out_name);       c->out_dir = dupn(s->out_dir);
    c->user_agent = dupn(s->user_agent);   c->referer = dupn(s->referer);
    c->cookie = dupn(s->cookie);           c->cookie_host = dupn(s->cookie_host);
    c->cookie_file = dupn(s->cookie_file); c->proxy = dupn(s->proxy);
    c->format = dupn(s->format);           c->ytdlp_args = dupn(s->ytdlp_args);
    for (int i = 0; i < s->nheaders; i++) c->headers[i] = xstrdup(s->headers[i]);
    c->cookies = NULL;
    c->ncookies = 0;
    if (s->ncookies) {
        c->cookies = xcalloc((size_t)s->ncookies, sizeof(ul_cookie));
        for (int i = 0; i < s->ncookies; i++) {
            c->cookies[i] = s->cookies[i];
            c->cookies[i].domain = xstrdup(s->cookies[i].domain);
            c->cookies[i].path = xstrdup(s->cookies[i].path);
            c->cookies[i].name = xstrdup(s->cookies[i].name);
            c->cookies[i].value = xstrdup(s->cookies[i].value);
        }
        c->ncookies = s->ncookies;
    }
    return c;
}

void cfg_free(ul_config *c)
{
    if (!c) return;
    free(c->out_name); free(c->out_dir); free(c->user_agent); free(c->referer);
    free(c->cookie); free(c->cookie_host); free(c->cookie_file); free(c->proxy);
    free(c->format); free(c->ytdlp_args);
    for (int i = 0; i < c->nheaders; i++) free(c->headers[i]);
    for (int i = 0; i < c->ncookies; i++) {
        free(c->cookies[i].domain); free(c->cookies[i].path);
        free(c->cookies[i].name); free(c->cookies[i].value);
    }
    free(c->cookies);
    free(c);
}

/* ------------------------------------------------------------------ */
/* URLs                                                                */
/* ------------------------------------------------------------------ */
static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

char *url_decode(const char *s, size_t n)
{
    sbuf b = { 0 };
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '%' && i + 2 < n && hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0) {
            char c = (char)(hexval(s[i + 1]) * 16 + hexval(s[i + 2]));
            sb_putc(&b, c ? c : '_');            /* never produce embedded NULs */
            i += 2;
        } else {
            sb_putc(&b, s[i]);
        }
    }
    return sb_steal(&b);
}

/* returns pointer to the start of the authority part ("host:port") */
static const char *url_authority(const char *url, size_t *len)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    *len = strcspn(p, "/?#");
    return p;
}

char *url_host(const char *url)
{
    size_t n;
    const char *a = url_authority(url, &n);
    const char *at = NULL;
    for (size_t i = 0; i < n; i++) if (a[i] == '@') at = a + i;
    if (at) { n -= (size_t)(at + 1 - a); a = at + 1; }
    size_t hl = 0;
    if (n && a[0] == '[') { const char *rb = memchr(a, ']', n); hl = rb ? (size_t)(rb - a + 1) : n; }
    else { while (hl < n && a[hl] != ':') hl++; }
    char *h = xstrndup(a, hl);
    for (char *p = h; *p; p++) *p = (char)tolower((unsigned char)*p);
    return h;
}

char *url_filename(const char *url)
{
    size_t n;
    const char *a = url_authority(url, &n);
    const char *path = a + n;
    size_t pl = strcspn(path, "?#");
    if (!pl) return NULL;
    const char *last = path + pl;
    const char *seg = last;
    while (seg > path && seg[-1] != '/') seg--;
    /* ftp://host/file.bin;type=i -> file.bin */
    if (str_istarts(url, "ftp://")) {
        for (const char *p = seg; p < last; p++)
            if (*p == ';' && _strnicmp(p, ";type=", 6) == 0) { last = p; break; }
    }
    if (seg == last) return NULL;
    char *dec = url_decode(seg, (size_t)(last - seg));
    if (!*dec) { free(dec); return NULL; }
    return dec;
}

/* Remove "." and ".." segments from an absolute path (RFC 3986 5.2.4). */
static void append_normalized_path(sbuf *out, const char *path, size_t plen)
{
    sbuf b = { 0 };
    size_t i = 0;
    int trailing = 0;
    while (i < plen) {
        if (path[i] == '/') { i++; continue; }
        size_t s = i;
        while (i < plen && path[i] != '/') i++;
        size_t sl = i - s;
        trailing = (i >= plen) ? 0 : 1;
        if (sl == 1 && path[s] == '.') { trailing = 1; continue; }
        if (sl == 2 && path[s] == '.' && path[s + 1] == '.') {
            while (b.len && b.s[b.len - 1] != '/') b.len--;
            if (b.len) b.len--;
            if (b.s) b.s[b.len] = 0;
            trailing = 1;
            continue;
        }
        sb_putc(&b, '/');
        sb_putn(&b, path + s, sl);
    }
    if (plen && path[plen - 1] == '/') trailing = 1;
    if (!b.len || trailing) sb_putc(&b, '/');
    sb_putn(out, b.s, b.len);
    sb_free(&b);
}

char *url_resolve(const char *base, const char *ref)
{
    if (!ref || !*ref) return xstrdup(base);
    const char *p = ref;
    while (isalnum((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.') p++;
    if (p > ref && strncmp(p, "://", 3) == 0) return xstrdup(ref);

    const char *se = strstr(base, "://");
    if (!se) return xstrdup(ref);
    if (ref[0] == '/' && ref[1] == '/')
        return xasprintf("%.*s:%s", (int)(se - base), base, ref);

    size_t alen;
    const char *auth = url_authority(base, &alen);
    const char *bpath = auth + alen;               /* "/dir/file?query" */
    size_t bplen = strcspn(bpath, "?#");

    sbuf full = { 0 };                             /* path + query of result */
    if (ref[0] == '/') {
        sb_puts(&full, ref);
    } else if (ref[0] == '?') {
        if (bplen) sb_putn(&full, bpath, bplen); else sb_putc(&full, '/');
        sb_puts(&full, ref);
    } else if (ref[0] == '#') {
        if (bplen) sb_putn(&full, bpath, bplen); else sb_putc(&full, '/');
        size_t q = strcspn(bpath, "#");
        if (q > bplen) sb_putn(&full, bpath + bplen, q - bplen);
    } else {
        size_t d = bplen;
        while (d > 0 && bpath[d - 1] != '/') d--;
        if (d == 0) sb_putc(&full, '/');
        else sb_putn(&full, bpath, d);
        sb_puts(&full, ref);
    }

    sbuf out = { 0 };
    sb_putn(&out, base, (size_t)(bpath - base));   /* scheme://host */
    size_t qp = strcspn(full.s, "?#");
    append_normalized_path(&out, full.s, qp);
    sb_puts(&out, full.s + qp);
    sb_free(&full);
    return sb_steal(&out);
}

char *sanitize_filename(const char *in)
{
    sbuf b = { 0 };
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        unsigned char c = *p;
        if (c < 32 || c == 127 || strchr("<>:\"/\\|?*", c)) sb_putc(&b, '_');
        else sb_putc(&b, (char)c);
    }
    char *s = sb_steal(&b);
    /* invalid UTF-8 would turn into '?' in file names: replace those bytes */
    if (!utf8_valid((const unsigned char *)s, strlen(s)))
        for (char *p = s; *p; p++) if ((unsigned char)*p >= 0x80) *p = '_';
    /* trim leading spaces/dots, trailing spaces and dots */
    char *start = s;
    while (*start == ' ' || *start == '.') start++;
    size_t n = strlen(start);
    while (n && (start[n - 1] == ' ' || start[n - 1] == '.')) n--;
    char *r = xstrndup(start, n);
    free(s);

    /* limit length to ~180 bytes, keeping the extension, on a UTF-8 boundary */
    const size_t maxlen = 180;
    n = strlen(r);
    if (n > maxlen) {
        const char *dot = strrchr(r, '.');
        size_t extlen = (dot && strlen(dot) <= 12) ? strlen(dot) : 0;
        size_t keep = maxlen - extlen;
        while (keep > 0 && ((unsigned char)r[keep] & 0xC0) == 0x80) keep--;
        char *t = xmalloc(keep + extlen + 1);
        memcpy(t, r, keep);
        memcpy(t + keep, r + n - extlen, extlen);
        t[keep + extlen] = 0;
        free(r);
        r = t;
        n = strlen(r);
        while (n && (r[n - 1] == ' ' || r[n - 1] == '.')) r[--n] = 0;
    }
    if (!*r) { free(r); return xstrdup("download"); }

    /* reserved device names, with or without extension, ignoring trailing spaces */
    static const char *reserved[] = { "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3",
        "COM4", "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4",
        "LPT5", "LPT6", "LPT7", "LPT8", "LPT9", "CONIN$", "CONOUT$",
        /* Windows also treats superscript digits as device numbers */
        "COM\xC2\xB9", "COM\xC2\xB2", "COM\xC2\xB3", "LPT\xC2\xB9", "LPT\xC2\xB2", "LPT\xC2\xB3" };
    size_t stem = strcspn(r, ".");
    while (stem && r[stem - 1] == ' ') stem--;
    for (size_t i = 0; i < sizeof reserved / sizeof *reserved; i++) {
        if (strlen(reserved[i]) == stem && _strnicmp(r, reserved[i], stem) == 0) {
            char *t = xasprintf("_%s", r);
            free(r);
            return t;
        }
    }
    return r;
}

const char *ext_for_content_type(const char *ct)
{
    static const struct { const char *ct, *ext; } map[] = {
        { "video/mp4", ".mp4" }, { "video/webm", ".webm" }, { "video/x-matroska", ".mkv" },
        { "video/quicktime", ".mov" }, { "video/x-msvideo", ".avi" }, { "video/mp2t", ".ts" },
        { "video/x-flv", ".flv" }, { "video/3gpp", ".3gp" },
        { "audio/mpeg", ".mp3" }, { "audio/mp4", ".m4a" }, { "audio/aac", ".aac" },
        { "audio/ogg", ".ogg" }, { "audio/wav", ".wav" }, { "audio/x-wav", ".wav" },
        { "audio/webm", ".weba" }, { "audio/flac", ".flac" },
        { "application/zip", ".zip" }, { "application/x-zip-compressed", ".zip" },
        { "application/x-rar-compressed", ".rar" }, { "application/vnd.rar", ".rar" },
        { "application/x-7z-compressed", ".7z" }, { "application/gzip", ".gz" },
        { "application/x-gzip", ".gz" }, { "application/x-tar", ".tar" },
        { "application/pdf", ".pdf" }, { "application/x-msdownload", ".exe" },
        { "application/vnd.microsoft.portable-executable", ".exe" },
        { "application/x-msi", ".msi" }, { "application/x-iso9660-image", ".iso" },
        { "application/vnd.android.package-archive", ".apk" },
        { "application/x-bittorrent", ".torrent" },
        { "application/json", ".json" }, { "application/xml", ".xml" },
        { "image/jpeg", ".jpg" }, { "image/png", ".png" }, { "image/gif", ".gif" },
        { "image/webp", ".webp" }, { "image/svg+xml", ".svg" },
        { "text/plain", ".txt" }, { "text/html", ".html" }, { "text/csv", ".csv" },
    };
    if (!ct) return "";
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (str_istarts(ct, map[i].ct)) return map[i].ext;
    return "";
}

/* ------------------------------------------------------------------ */
/* files                                                               */
/* ------------------------------------------------------------------ */
char *path_join(const char *dir, const char *name)
{
    if (!dir || !*dir) return xstrdup(name);
    size_t n = strlen(dir);
    int sep = dir[n - 1] == '\\' || dir[n - 1] == '/';
    return xasprintf("%s%s%s", dir, sep ? "" : "\\", name);
}

const char *path_basename(const char *p)
{
    const char *b = p;
    for (; *p; p++) if (*p == '\\' || *p == '/') b = p + 1;
    return b;
}

const char *path_ext(const char *p)
{
    const char *b = path_basename(p);
    const char *dot = strrchr(b, '.');
    return (dot && dot != b) ? dot : b + strlen(b);
}

char *path_strip_ext(const char *p)
{
    const char *e = path_ext(p);
    return xstrndup(p, (size_t)(e - p));
}

char *path_dirname(const char *p)
{
    const char *b = path_basename(p);
    if (b == p) return xstrdup(".");
    size_t n = (size_t)(b - p);
    if (n > 1 && !(n == 3 && p[1] == ':')) n--;     /* keep "C:\" */
    return xstrndup(p, n);
}

int file_exists(const char *p)
{
    wchar_t *w = wpath(p);
    DWORD a = GetFileAttributesW(w);
    free(w);
    return a != INVALID_FILE_ATTRIBUTES;
}

int dir_exists(const char *p)
{
    wchar_t *w = wpath(p);
    DWORD a = GetFileAttributesW(w);
    free(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

int64_t file_size(const char *p)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    wchar_t *w = wpath(p);
    BOOL ok = GetFileAttributesExW(w, GetFileExInfoStandard, &d);
    free(w);
    if (!ok || (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return -1;
    return ((int64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}

/* "Wed, 23 Sep 2026 14:33:02 GMT" (RFC 1123, what servers send) -> FILETIME */
int http_date_to_filetime(const char *s, FILETIME *ft)
{
    static const char *mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    int d, y, H, M, S;
    char m[4] = "";
    if (!s || sscanf(s, "%*[^,], %d %3s %d %d:%d:%d", &d, m, &y, &H, &M, &S) != 6) return 0;
    SYSTEMTIME st;
    memset(&st, 0, sizeof st);
    for (int i = 0; i < 12; i++) if (_stricmp(m, mon[i]) == 0) st.wMonth = (WORD)(i + 1);
    if (!st.wMonth || y < 1980 || y > 9999 || d < 1 || d > 31 || H > 23 || M > 59 || S > 60) return 0;
    st.wYear = (WORD)y; st.wDay = (WORD)d; st.wHour = (WORD)H; st.wMinute = (WORD)M; st.wSecond = (WORD)(S > 59 ? 59 : S);
    return SystemTimeToFileTime(&st, ft) ? 1 : 0;
}

/* 1 if the file's modification time equals ft (within 2 s), 0 if not, -1 if unknown */
int file_mtime_equals(const char *p, const FILETIME *ft)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    wchar_t *w = wpath(p);
    BOOL ok = GetFileAttributesExW(w, GetFileExInfoStandard, &d);
    free(w);
    if (!ok) return -1;
    ULARGE_INTEGER a, b;
    a.LowPart = d.ftLastWriteTime.dwLowDateTime; a.HighPart = d.ftLastWriteTime.dwHighDateTime;
    b.LowPart = ft->dwLowDateTime;               b.HighPart = ft->dwHighDateTime;
    uint64_t diff = a.QuadPart > b.QuadPart ? a.QuadPart - b.QuadPart : b.QuadPart - a.QuadPart;
    return diff <= 20000000ULL;                  /* 2 s in 100 ns units */
}

int file_delete(const char *p)
{
    wchar_t *w = wpath(p);
    BOOL ok = DeleteFileW(w);
    free(w);
    return ok ? 0 : -1;
}

int file_rename(const char *from, const char *to)
{
    wchar_t *a = wpath(from), *b = wpath(to);
    BOOL ok = MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
    free(a);
    free(b);
    return ok ? 0 : -1;
}

int dir_create(const char *p)
{
    if (!p || !*p || dir_exists(p)) return 0;
    wchar_t *w = u8_to_w(p);
    DWORD n = GetFullPathNameW(w, 0, NULL, NULL);
    if (!n) { free(w); return -1; }
    wchar_t *full = xmalloc(((size_t)n + 1) * sizeof(wchar_t));
    GetFullPathNameW(w, n + 1, full, NULL);
    free(w);
    size_t len = wcslen(full);
    /* first component that can be created: after "C:\" or "\\server\share\" */
    size_t start = 3;
    if (full[0] == L'\\' && full[1] == L'\\') {
        int seps = 0;
        for (start = 2; start < len && seps < 2; start++) if (full[start] == L'\\') seps++;
    }
    int rc = 0;
    for (size_t i = start; i <= len && rc == 0; i++) {
        if (i == len || full[i] == L'\\' || full[i] == L'/') {
            wchar_t save = full[i];
            full[i] = 0;
            char *u = w_to_u8(full);
            wchar_t *wp = wpath(u);
            free(u);
            if (!CreateDirectoryW(wp, NULL)) {
                DWORD e = GetLastError();
                if (e != ERROR_ALREADY_EXISTS && e != ERROR_ACCESS_DENIED) rc = -1;
            }
            free(wp);
            full[i] = save;
        }
    }
    free(full);
    return dir_exists(p) ? 0 : -1;
}

int dir_remove(const char *p)
{
    wchar_t *w = wpath(p);
    BOOL ok = RemoveDirectoryW(w);
    free(w);
    return ok ? 0 : -1;
}

FILE *fopen_u8(const char *p, const char *mode)
{
    wchar_t *w = wpath(p), *m = u8_to_w(mode);
    FILE *f = _wfopen(w, m);
    free(w);
    free(m);
    return f;
}

char *read_file(const char *p, size_t *len)
{
    FILE *f = fopen_u8(p, "rb");
    if (!f) return NULL;
    sbuf b = { 0 };
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) sb_putn(&b, buf, n);
    fclose(f);
    if (len) *len = b.len;
    return sb_steal(&b);
}

/* "name.ext" -> "name (1).ext", "name (2).ext", ... (first one not taken) */
char *unique_path(const char *path)
{
    char *stem = path_strip_ext(path);
    const char *ext = path_ext(path);
    for (int i = 1; i < 100000; i++) {
        char *c = xasprintf("%s (%d)%s", stem, i, ext);
        char *part = xasprintf("%s.ulpart", c);
        char *st = xasprintf("%s.ulstate", c);
        int taken = file_exists(c) || file_exists(part) || file_exists(st);
        free(part);
        free(st);
        if (!taken) { free(stem); return c; }
        free(c);
    }
    free(stem);
    return xstrdup(path);
}

int64_t disk_free(const char *path)
{
    char *dir = path_dirname(path);
    wchar_t *w = u8_to_w(dir);
    ULARGE_INTEGER freeb;
    BOOL ok = GetDiskFreeSpaceExW(w, &freeb, NULL, NULL);
    free(w);
    free(dir);
    return ok ? (int64_t)freeb.QuadPart : -1;
}

char *exe_dir(void)
{
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(NULL, buf, MAX_PATH * 4);
    if (!n || n >= MAX_PATH * 4) return xstrdup(".");
    while (n > 0 && buf[n - 1] != L'\\' && buf[n - 1] != L'/') n--;
    if (n > 0) n--;
    buf[n] = 0;
    return w_to_u8(buf);
}

static const GUID UL_FOLDERID_Downloads =
    { 0x374DE290, 0x123F, 0x4565, { 0x91, 0x64, 0x39, 0xC4, 0x92, 0x5E, 0x46, 0x7B } };

char *downloads_dir(void)
{
    PWSTR p = NULL;
    char *r = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(&UL_FOLDERID_Downloads, 0, NULL, &p)) && p)
        r = w_to_u8(p);
    if (p) CoTaskMemFree(p);
    if (!r) {
        wchar_t up[MAX_PATH * 2];
        DWORD n = GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH * 2);
        if (n && n < MAX_PATH * 2) {
            char *u = w_to_u8(up);
            r = xasprintf("%s\\Downloads", u);
            free(u);
        } else {
            r = xstrdup(".");
        }
    }
    return r;
}

/* Helper programs (yt-dlp, ffmpeg) are taken ONLY from U-loader's own folder
   or from absolute folders listed in PATH - never from the current folder,
   which is often the download folder (a downloaded "yt-dlp.exe" there must
   never be executed). */
/* Look for `name` in a ';'-separated folder list (absolute folders only). */
static char *search_path_list(char *path, const char *name)
{
    char *found = NULL, *save = NULL;
    for (char *dir = strtok_r(path, ";", &save); dir && !found; dir = strtok_r(NULL, ";", &save)) {
        char *dd = str_trim(dir);
        size_t l = strlen(dd);
        if (l >= 2 && dd[0] == '"' && dd[l - 1] == '"') { dd[l - 1] = 0; dd++; }
        /* absolute only: "C:\..." or "\\server\..."; skip "." and relative entries */
        int absolute = (isalpha((unsigned char)dd[0]) && dd[1] == ':' && (dd[2] == '\\' || dd[2] == '/')) ||
                       (dd[0] == '\\' && dd[1] == '\\');
        if (!absolute) continue;
        char *cand = path_join(dd, name);
        if (file_exists(cand) && !dir_exists(cand)) found = cand;
        else free(cand);
    }
    return found;
}

/* The PATH stored in the registry right now (user + machine, %VARS%
   expanded): sees tools installed after this program - or Explorer -
   was started, e.g. "winget install DenoLand.Deno" a minute ago. */
static char *registry_path(void)
{
    static const struct { HKEY root; const wchar_t *key; } src[] = {
        { HKEY_CURRENT_USER, L"Environment" },
        { HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment" },
    };
    sbuf all = { 0 };
    for (size_t i = 0; i < sizeof src / sizeof *src; i++) {
        DWORD type = 0, size = 0;
        if (RegGetValueW(src[i].root, src[i].key, L"Path", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                         &type, NULL, &size) != ERROR_SUCCESS || size == 0 || size > (1 << 20))
            continue;
        wchar_t *raw = xmalloc(size + sizeof(wchar_t));
        if (RegGetValueW(src[i].root, src[i].key, L"Path", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                         &type, raw, &size) == ERROR_SUCCESS) {
            raw[size / sizeof(wchar_t)] = 0;
            DWORD n = ExpandEnvironmentStringsW(raw, NULL, 0);
            wchar_t *ex = xmalloc(((size_t)n + 1) * sizeof(wchar_t));
            if (n && ExpandEnvironmentStringsW(raw, ex, n + 1)) {
                char *u = w_to_u8(ex);
                if (all.len) sb_putc(&all, ';');
                sb_puts(&all, u);
                free(u);
            }
            free(ex);
        }
        free(raw);
    }
    return all.len ? sb_steal(&all) : (sb_free(&all), NULL);
}

char *find_tool(const char *name)
{
    char *d = exe_dir();
    char *p = path_join(d, name);
    free(d);
    if (file_exists(p) && !dir_exists(p)) return p;
    free(p);

    char *found = NULL;
    DWORD n = GetEnvironmentVariableW(L"PATH", NULL, 0);
    if (n) {
        wchar_t *wp = xmalloc((size_t)n * sizeof(wchar_t));
        GetEnvironmentVariableW(L"PATH", wp, n);
        char *path = w_to_u8(wp);
        free(wp);
        found = search_path_list(path, name);
        free(path);
    }
    if (!found) {
        char *rp = registry_path();
        if (rp) { found = search_path_list(rp, name); free(rp); }
    }
    return found;
}

/* ------------------------------------------------------------------ */
/* processes                                                           */
/* ------------------------------------------------------------------ */
/* Append one argument using the MSVCRT command-line quoting rules. */
void cmd_append_arg(sbuf *b, const char *a)
{
    if (b->len) sb_putc(b, ' ');
    if (*a && !strpbrk(a, " \t\n\v\"")) { sb_puts(b, a); return; }
    sb_putc(b, '"');
    for (const char *p = a;; p++) {
        size_t bs = 0;
        while (*p == '\\') { bs++; p++; }
        if (!*p) { for (size_t i = 0; i < bs * 2; i++) sb_putc(b, '\\'); break; }
        if (*p == '"') {
            for (size_t i = 0; i < bs * 2 + 1; i++) sb_putc(b, '\\');
            sb_putc(b, '"');
        } else {
            for (size_t i = 0; i < bs; i++) sb_putc(b, '\\');
            sb_putc(b, *p);
        }
    }
    sb_putc(b, '"');
}

/* Run a program, collect its stdout+stderr, and kill it if the task is
   paused. With no console (GUI) the child gets no window either.
   Returns the exit code, or -1 if it could not start / was cancelled. */
int run_process(const char *cmdline, char **captured, const ul_task *t)
{
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd = NULL, wr = NULL, nul;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    if (captured) *captured = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = nul;
    DWORD flags = GetConsoleWindow() ? 0 : CREATE_NO_WINDOW;
    wchar_t *w = u8_to_w(cmdline);
    BOOL ok = CreateProcessW(NULL, w, NULL, NULL, TRUE, flags, NULL, NULL, &si, &pi);
    free(w);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { CloseHandle(rd); return -1; }

    sbuf b = { 0 };
    char buf[8192];
    int killed = 0;
    for (;;) {
        DWORD avail = 0, n = 0;
        if (PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail) {
            if (ReadFile(rd, buf, avail < sizeof buf ? avail : sizeof buf, &n, NULL) && n)
                sb_putn(&b, buf, n);
            continue;
        }
        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
            while (PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail &&
                   ReadFile(rd, buf, avail < sizeof buf ? avail : sizeof buf, &n, NULL) && n)
                sb_putn(&b, buf, n);
            break;
        }
        if (!killed && task_aborted(t)) {
            TerminateProcess(pi.hProcess, 1);
            killed = 1;
        }
    }
    CloseHandle(rd);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (captured) *captured = sb_steal(&b);
    else sb_free(&b);
    return killed ? -1 : (int)code;
}

static int run_ffmpeg(const char *ff, const char **args, size_t nargs, const char *out, const ul_task *t)
{
    sbuf c = { 0 };
    cmd_append_arg(&c, ff);
    for (size_t i = 0; i < nargs; i++) cmd_append_arg(&c, args[i]);
    char *log = NULL;
    int r = run_process(c.s, &log, t);
    sb_free(&c);
    if (r != 0) {
        file_delete(out);
        if (log && *log && r > 0) {
            char *last = str_trim(log);
            char *nl = strrchr(last, '\n');
            tlog((ul_task *)t, LOG_DIM, "  ffmpeg: %s", nl ? nl + 1 : last);
        }
    }
    free(log);
    return r;
}

int ffmpeg_merge(const char *ff, const char *video, const char *audio, const char *out, const ul_task *t)
{
    const char *args[] = { "-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-i", video, "-i", audio,
                           "-map", "0:v:0", "-map", "1:a:0", "-c", "copy", out };
    return run_ffmpeg(ff, args, sizeof args / sizeof *args, out, t);
}

int ffmpeg_remux(const char *ff, const char *in, const char *out, const ul_task *t)
{
    const char *args[] = { "-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-i", in,
                           "-map", "0:v?", "-map", "0:a?", "-c", "copy", out };
    return run_ffmpeg(ff, args, sizeof args / sizeof *args, out, t);
}

/* ------------------------------------------------------------------ */
/* headers                                                             */
/* ------------------------------------------------------------------ */
void headers_set(ul_headers *h, const char *name, const char *value)
{
    if (strpbrk(name, "\r\n:") || strpbrk(value, "\r\n")) return;   /* no header injection */
    for (int i = 0; i < h->n; i++) {
        if (_stricmp(h->name[i], name) == 0) {
            free(h->value[i]);
            h->value[i] = xstrdup(value);
            return;
        }
    }
    if (h->n >= UL_MAX_HEADERS) return;
    h->name[h->n] = xstrdup(name);
    h->value[h->n] = xstrdup(value);
    h->n++;
}

void headers_free(ul_headers *h)
{
    for (int i = 0; i < h->n; i++) { free(h->name[i]); free(h->value[i]); }
    h->n = 0;
}

/* ------------------------------------------------------------------ */
/* console                                                             */
/* ------------------------------------------------------------------ */
void con_init(void)
{
    SetConsoleOutputCP(CP_UTF8);
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    if (GetConsoleMode(h, &mode)) {
        g_tty = 1;
        if (SetConsoleMode(h, mode | 0x0004 /* VT processing */)) g_vt = 1;
    }
}

int con_width(void)
{
    CONSOLE_SCREEN_BUFFER_INFO bi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &bi)) {
        int w = bi.srWindow.Right - bi.srWindow.Left + 1;
        if (w >= 40) return w;
    }
    return 80;
}

static int utf8_columns(const char *s)
{
    int n = 0;
    for (; *s; s++) if (((unsigned char)*s & 0xC0) != 0x80) n++;
    return n;
}

void con_progress(const char *line)
{
    if (!g_tty) {
        /* redirected to a file/pipe: one plain line every few seconds */
        static double last;
        double t = now_sec();
        if (t - last < 5.0 && strncmp(line, "100.0%", 6) != 0) return;
        last = t;
        fputs(line, stdout);
        fputc('\n', stdout);
        fflush(stdout);
        return;
    }
    if (g_vt) {
        fputs("\r\x1b[2K", stdout);
        fputs(line, stdout);
    } else {
        fputc('\r', stdout);
        fputs(line, stdout);
        for (int i = utf8_columns(line); i < con_width() - 1; i++) fputc(' ', stdout);
    }
    fflush(stdout);
    g_progress_open = 1;
}

void con_progress_end(void)
{
    if (g_progress_open) {
        fputc('\n', stdout);
        fflush(stdout);
        g_progress_open = 0;
    }
}

char *con_read_line(void)
{
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    if (GetConsoleMode(h, &mode)) {
        wchar_t buf[8192];
        DWORD n = 0;
        if (!ReadConsoleW(h, buf, 8191, &n, NULL) || n == 0) return NULL;
        buf[n] = 0;
        return w_to_u8(buf);
    }
    char buf[8192];
    if (!fgets(buf, sizeof buf, stdin)) return NULL;
    return xstrdup(buf);
}

void con_log(int kind, const char *msg)
{
    con_progress_end();
    const char *col = "", *pre = "";
    switch (kind) {
    case LOG_OK:   col = "\x1b[32m"; break;
    case LOG_WARN: col = "\x1b[33m"; pre = "Warning: "; break;
    case LOG_ERR:  col = "\x1b[31m"; pre = "Error: "; break;
    case LOG_HEAD: col = "\x1b[1;36m"; break;
    case LOG_DIM:  col = "\x1b[90m"; break;
    default: break;
    }
    if (g_vt && *col) fputs(col, stdout);
    fputs(pre, stdout);
    fputs(msg, stdout);
    if (g_vt && *col) fputs("\x1b[0m", stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

void ul_log(int kind, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *msg = xmalloc((size_t)(n > 0 ? n : 0) + 1);
    va_start(ap, fmt);
    vsnprintf(msg, (size_t)(n > 0 ? n : 0) + 1, fmt, ap);
    va_end(ap);
    con_log(kind, msg);
    free(msg);
}
