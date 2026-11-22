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
    if (n < 0) return xstrdup("");
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
    /* already a device / long path ("\\?\", "\\.\", "//?/", "//./"): leave it alone */
    if ((w[0] == L'\\' || w[0] == L'/') && (w[1] == L'\\' || w[1] == L'/') &&
        (w[2] == L'?' || w[2] == L'.') && (w[3] == L'\\' || w[3] == L'/'))
        return w;
    if (wcslen(w) < 200) return w;                 /* short even when made absolute? check below */
    DWORD n = GetFullPathNameW(w, 0, NULL, NULL);
    if (!n) return w;
    wchar_t *full = xmalloc(((size_t)n + 1) * sizeof(wchar_t));
    DWORD m = GetFullPathNameW(w, n + 1, full, NULL);
    if (!m || m > n) { free(full); return w; }     /* changed under us: use as given */
    if (wcslen(full) < 240) { free(full); return w; }
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

/* Strict UTF-8: no overlong forms, no UTF-16 surrogates, nothing above U+10FFFF. */
int utf8_valid(const unsigned char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i];
        int extra;
        unsigned cp, min;
        if (c < 0x80) { i++; continue; }
        else if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; min = 0x80; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; min = 0x800; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; min = 0x10000; }
        else return 0;
        if (i + (size_t)extra >= n) return 0;
        for (int k = 1; k <= extra; k++) {
            unsigned char cc = s[i + (size_t)k];
            if ((cc & 0xC0) != 0x80) return 0;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
        i += (size_t)extra + 1;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* string buffer                                                       */
/* ------------------------------------------------------------------ */
static void sb_grow(sbuf *b, size_t add)
{
    if (add > SIZE_MAX / 2 - b->len - 1) die_oom();      /* no size_t wrap-around */
    size_t need = b->len + add + 1;
    if (need <= b->cap) return;
    size_t nc = b->cap ? b->cap : 256;
    while (nc < need) nc = nc > SIZE_MAX / 4 ? need : nc * 2;
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

/* Trims in place and returns s itself (never a pointer into the middle):
   callers may free() what they passed in. */
char *str_trim(char *s)
{
    size_t lead = 0;
    while (s[lead] && isspace((unsigned char)s[lead])) lead++;
    size_t n = strlen(s + lead);
    if (lead) memmove(s, s + lead, n + 1);
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
    if (!(sec >= 0 && sec <= 99.0 * 3600)) { snprintf(buf, len, "--:--"); return buf; }   /* also NaN */
    long s = (long)(sec + 0.5);
    if (s >= 3600) snprintf(buf, len, "%ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
    else snprintf(buf, len, "%02ld:%02ld", s / 60, s % 60);
    return buf;
}

/* "500K", "2M", "1.5G", "800" (bytes), optional trailing "B" / "/s" */
int64_t parse_size(const char *s)
{
    if (!isdigit((unsigned char)*s) && *s != '.') return -1;      /* no "nan", "inf", "-1" */
    char *end;
    double v = strtod(s, &end);
    if (end == s || !(v >= 0) || v > 1e15) return -1;
    switch (toupper((unsigned char)*end)) {
    case 'K': v *= 1024.0; end++; break;
    case 'M': v *= 1048576.0; end++; break;
    case 'G': v *= 1073741824.0; end++; break;
    default: break;
    }
    if (toupper((unsigned char)*end) == 'B') end++;
    if (end[0] == '/' && tolower((unsigned char)end[1]) == 's') end += 2;
    if (*end) return -1;                                          /* "10Mfoo" */
    return (int64_t)v;
}

double now_sec(void)
{
    static volatile LONG64 freq;
    LARGE_INTEGER c;
    if (!freq) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        InterlockedExchange64(&freq, f.QuadPart);
    }
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq;
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
    if (!host || !domain || !*host) return 0;
    if (*domain == '.') domain++;
    size_t hl = strlen(host), dl = strlen(domain);
    /* "" / "." / "com." / "..com" are never valid cookie domains */
    if (!dl || domain[0] == '.' || domain[dl - 1] == '.' || host[hl - 1] == '.') return 0;
    if (hl == dl) return _stricmp(host, domain) == 0;
    return hl > dl && host[hl - dl - 1] == '.' && _stricmp(host + hl - dl, domain) == 0;
}

/* "https://Example.com/x" -> "https://example.com:443": scheme + host + port,
   the unit credentials belong to (a downgrade to http or another port is
   a different origin) */
char *url_origin(const char *url)
{
    char *host = url_host(url);
    const char *se = strstr(url, "://");
    size_t sl = se ? (size_t)(se - url) : 0;
    char scheme[16] = "";
    for (size_t i = 0; i < sl && i < sizeof scheme - 1; i++) scheme[i] = (char)tolower((unsigned char)url[i]);
    const char *a = se ? se + 3 : url;
    size_t al = strcspn(a, "/?#");
    const char *at = NULL;
    for (size_t i = 0; i < al; i++) if (a[i] == '@') at = a + i;
    if (at) { al -= (size_t)(at + 1 - a); a = at + 1; }
    const char *colon = NULL;
    const char *rb = memchr(a, ']', al);
    for (const char *q = rb ? rb : a; q < a + al; q++) if (*q == ':') colon = q;
    long port = colon ? strtol(colon + 1, NULL, 10) : 0;
    if (port <= 0) port = !strcmp(scheme, "https") ? 443 : !strcmp(scheme, "ftp") ? 21 : 80;
    char *r = xasprintf("%s://%s:%ld", scheme, host, port);
    free(host);
    return r;
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

/* include_raw: also the -b "a=b" cookie (only to its own origin) */
char *cookie_header_for(const ul_config *cfg, const char *url, int include_raw)
{
    char *host = url_host(url);
    sbuf b = { 0 };
    int https = str_istarts(url, "https://");
    if (include_raw && cfg->cookie && *cfg->cookie && !strpbrk(cfg->cookie, "\r\n") &&
        (!cfg->cookie_host || host_matches(host, cfg->cookie_host)))
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
            /* one malformed cookie must not break the whole header */
            if (!*c->name || strpbrk(c->name, "\r\n;= \t") || strpbrk(c->value, "\r\n;")) continue;
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

/* Remove "." and ".." segments from an absolute path (RFC 3986 5.2.4).
   Empty segments ("a//b") are kept: some signed CDN / S3 links need them. */
static void append_normalized_path(sbuf *out, const char *path, size_t plen)
{
    sbuf b = { 0 };                        /* "/seg/seg..." without trailing slash */
    size_t i = (plen && path[0] == '/') ? 1 : 0;
    int trailing = plen == 0 || (plen == 1 && path[0] == '/');
    while (i <= plen) {
        size_t s = i;
        while (i < plen && path[i] != '/') i++;
        size_t sl = i - s;
        int last = i >= plen;
        if (sl == 1 && path[s] == '.') {
            trailing = last;
        } else if (sl == 2 && path[s] == '.' && path[s + 1] == '.') {
            while (b.len && b.s[b.len - 1] != '/') b.len--;
            if (b.len) b.len--;                /* drop the segment and its '/' */
            if (b.s) b.s[b.len] = 0;
            trailing = last;
        } else if (!(last && sl == 0)) {       /* a trailing '/' is not a segment */
            sb_putc(&b, '/');
            sb_putn(&b, path + s, sl);
            trailing = 0;
        } else {
            trailing = 1;
        }
        i++;                                   /* past the '/' */
    }
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
    size_t tl = strcspn(ct, "; \t");                     /* the type itself, without parameters */
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (strlen(map[i].ct) == tl && _strnicmp(ct, map[i].ct, tl) == 0) return map[i].ext;
    return "";
}

/* ------------------------------------------------------------------ */
/* U-loader's data folder and the browser extension's "I was here"      */
/* ------------------------------------------------------------------ */
char *uloader_data_dir(void)
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"ULOADER_DATA", buf, MAX_PATH * 2);   /* tests / portable use */
    if (n && n < MAX_PATH * 2) return w_to_u8(buf);
    n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH * 2);
    if (n && n < MAX_PATH * 2) {
        char *a = w_to_u8(buf), *d = path_join(a, "U-loader");
        free(a);
        return d;
    }
    char *e = exe_dir(), *d = path_join(e, "U-loader-data");
    free(e);
    return d;
}

/* The native host writes this when the extension talks to it, so the
   window knows the extension has been added (#13). */
void browser_mark_connected(void)
{
    char *dir = uloader_data_dir(), *p = path_join(dir, "browser.txt");
    dir_create(dir);
    FILE *f = fopen_u8(p, "wb");
    if (f) { fprintf(f, "extension=connected\nlast=%lld\n", (long long)time(NULL)); fclose(f); }
    free(p);
    free(dir);
}

int browser_connected(void)
{
    char *dir = uloader_data_dir(), *p = path_join(dir, "browser.txt");
    int r = file_exists(p);
    free(p);
    free(dir);
    return r;
}

/* ------------------------------------------------------------------ */
/* categories (#1): downloads sorted into sub-folders by file type     */
/* ------------------------------------------------------------------ */
static const char *CAT_NAMES[UL_NCATS] = { "Video", "Music", "Documents", "Compressed", "Programs", "Pictures" };
static const char *CAT_DEFAULTS[UL_NCATS] = {
    "mp4 mkv avi mov wmv flv webm m4v 3gp ts mts m2ts mpg mpeg ogv vob",
    "mp3 m4a aac flac wav ogg oga opus wma alac aiff mka",
    "pdf doc docx xls xlsx ppt pptx odt ods odp rtf txt csv epub mobi djvu",
    "zip rar 7z gz tgz bz2 tbz2 xz txz tar zst lz lzma cab",
    "exe msi msix msixbundle appx apk xapk dmg pkg deb rpm iso img jar",
    "jpg jpeg png gif webp bmp svg tif tiff heic heif avif ico psd",
};
static char *g_cat_exts[UL_NCATS];        /* NULL = the default list */
static SRWLOCK g_cat_lock = SRWLOCK_INIT;

const char *category_name(int cat) { return cat >= 0 && cat < UL_NCATS ? CAT_NAMES[cat] : ""; }
const char *category_default_exts(int cat) { return cat >= 0 && cat < UL_NCATS ? CAT_DEFAULTS[cat] : ""; }

char *category_exts(int cat)
{
    if (cat < 0 || cat >= UL_NCATS) return xstrdup("");
    AcquireSRWLockShared(&g_cat_lock);
    char *r = xstrdup(g_cat_exts[cat] ? g_cat_exts[cat] : CAT_DEFAULTS[cat]);
    ReleaseSRWLockShared(&g_cat_lock);
    return r;
}

void category_set_exts(int cat, const char *exts)
{
    if (cat < 0 || cat >= UL_NCATS) return;
    /* normalized: lower case, separated by single spaces, no dots */
    char *v = NULL;
    if (exts) {
        sbuf b = { 0 };
        for (const char *p = exts; *p;) {
            p += strspn(p, " \t,;.");
            size_t n = strcspn(p, " \t,;.");
            if (!n) break;
            if (n <= 16) {
                if (b.len) sb_putc(&b, ' ');
                for (size_t i = 0; i < n; i++) sb_putc(&b, (char)tolower((unsigned char)p[i]));
            }
            p += n;
        }
        v = sb_steal(&b);
        if (!strcmp(v, CAT_DEFAULTS[cat])) { free(v); v = NULL; }
    }
    AcquireSRWLockExclusive(&g_cat_lock);
    free(g_cat_exts[cat]);
    g_cat_exts[cat] = v;
    ReleaseSRWLockExclusive(&g_cat_lock);
}

static int word_in(const char *list, const char *w, size_t wl)
{
    for (const char *p = list; *p;) {
        p += strspn(p, " ");
        size_t n = strcspn(p, " ");
        if (n == wl && _strnicmp(p, w, n) == 0) return 1;
        p += n;
    }
    return 0;
}

int category_of(const char *filename)
{
    const char *ext = filename ? path_ext(filename) : "";
    if (!*ext || !ext[1]) return -1;
    ext++;                                            /* without the dot */
    size_t el = strlen(ext);
    int cat = -1;
    AcquireSRWLockShared(&g_cat_lock);
    for (int i = 0; i < UL_NCATS && cat < 0; i++)
        if (word_in(g_cat_exts[i] ? g_cat_exts[i] : CAT_DEFAULTS[i], ext, el)) cat = i;
    ReleaseSRWLockShared(&g_cat_lock);
    return cat;
}

char *category_folder(const ul_config *cfg, const char *filename)
{
    const char *base = cfg->out_dir ? cfg->out_dir : ".";
    int cat = cfg->by_category ? category_of(filename) : -1;
    return cat < 0 ? xstrdup(base) : path_join(base, CAT_NAMES[cat]);
}

/* ------------------------------------------------------------------ */
/* speed limiter (#2): one budget for every download of this process   */
/* ------------------------------------------------------------------ */
static volatile LONG64 g_rate;            /* bytes per second, 0 = off */
static double g_rate_next;                /* when the budget allows the next byte */
static SRWLOCK g_rate_lock = SRWLOCK_INIT;

void speed_limit_set(int64_t bps)
{
    if (bps < 0) bps = 0;
    AcquireSRWLockExclusive(&g_rate_lock);
    InterlockedExchange64(&g_rate, bps);
    g_rate_next = 0;                      /* a new limit starts from now, without old debt */
    ReleaseSRWLockExclusive(&g_rate_lock);
}

int64_t speed_limit_get(void) { return g_rate; }

void speed_limit_take(const ul_task *t, int64_t n)
{
    int64_t rate = g_rate;
    if (rate <= 0 || n <= 0) return;
    double now = now_sec();
    AcquireSRWLockExclusive(&g_rate_lock);
    if (g_rate_next < now - 0.25) g_rate_next = now - 0.25;   /* a quarter second of burst, no more */
    g_rate_next += (double)n / (double)rate;
    double due = g_rate_next;
    ReleaseSRWLockExclusive(&g_rate_lock);
    /* in short steps: a pause, or the limiter switched off or changed,
       ends the wait at once */
    while (g_rate == rate && !task_aborted(t)) {
        double left = due - now_sec();
        if (left <= 0) break;
        Sleep(left > 0.1 ? 100 : (DWORD)(left * 1000) + 1);
    }
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

/* Which version of the server's file a finished download is, kept in a
   hidden NTFS stream "<file>:uloader.meta". The file's dates stay the
   time it was downloaded, and its contents are untouched. (On FAT/exFAT
   drives there are no streams; then nothing is recorded.) */
static wchar_t *meta_stream_path(const char *p)
{
    char *s = xasprintf("%s:uloader.meta", p);
    wchar_t *w = wpath(s);
    free(s);
    return w;
}

void file_meta_write(const char *p, const char *etag, const char *lastmod)
{
    if ((!etag || !*etag) && (!lastmod || !*lastmod)) return;
    wchar_t *w = meta_stream_path(p);
    HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return;
    char *txt = xasprintf("etag=%s\nlastmod=%s\n", etag && !strpbrk(etag, "\r\n") ? etag : "",
                          lastmod && !strpbrk(lastmod, "\r\n") ? lastmod : "");
    DWORD wr;
    WriteFile(h, txt, (DWORD)strlen(txt), &wr, NULL);
    CloseHandle(h);
    free(txt);
}

/* 1 = same version as the server reports, 0 = a different one, -1 = cannot tell */
int file_meta_matches(const char *p, const char *etag, const char *lastmod)
{
    wchar_t *w = meta_stream_path(p);
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return -1;
    char buf[2048];
    DWORD n = 0;
    BOOL ok = ReadFile(h, buf, sizeof buf - 1, &n, NULL);
    CloseHandle(h);
    if (!ok) return -1;
    buf[n] = 0;
    char *e = strstr(buf, "etag="), *l = strstr(buf, "lastmod=");
    char se[1024] = "", sl[1024] = "";
    if (e) snprintf(se, sizeof se, "%.*s", (int)strcspn(e + 5, "\n"), e + 5);
    if (l) snprintf(sl, sizeof sl, "%.*s", (int)strcspn(l + 8, "\n"), l + 8);
    if (etag && *etag && *se) return strcmp(etag, se) == 0;
    if (lastmod && *lastmod && *sl) return strcmp(lastmod, sl) == 0;
    return -1;
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
    DWORD m = GetFullPathNameW(w, n + 1, full, NULL);
    free(w);
    if (!m || m > n) { free(full); return -1; }
    size_t len = wcslen(full);
    /* first component that can be created: after "C:\", "\\server\share\",
       "\\?\C:\" or "\\?\UNC\server\share\" */
    size_t start = 3, skip_seps = 0, from = 0;
    if (wcsncmp(full, L"\\\\?\\UNC\\", 8) == 0) { from = 8; skip_seps = 2; }
    else if (wcsncmp(full, L"\\\\?\\", 4) == 0) { start = 7; }
    else if (full[0] == L'\\' && full[1] == L'\\') { from = 2; skip_seps = 2; }
    if (skip_seps) {
        size_t seps = 0;
        for (start = from; start < len && seps < skip_seps; start++) if (full[start] == L'\\') seps++;
    }
    if (start > len) start = len;
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
    size_t dl = strlen(dir);
    /* a share root like \\server\share needs its trailing backslash */
    char *d2 = (dl && dir[dl - 1] != '\\' && dir[dl - 1] != '/') ? xasprintf("%s\\", dir) : xstrdup(dir);
    wchar_t *w = wpath(d2);
    ULARGE_INTEGER freeb;
    BOOL ok = GetDiskFreeSpaceExW(w, &freeb, NULL, NULL);
    free(w);
    free(d2);
    free(dir);
    return ok ? (int64_t)freeb.QuadPart : -1;
}

char *exe_dir(void)
{
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(NULL, buf, MAX_PATH * 4);
    if (!n || n >= MAX_PATH * 4) return xstrdup(".");
    while (n > 0 && buf[n - 1] != L'\\' && buf[n - 1] != L'/') n--;
    if (n > 0 && !(n == 3 && buf[1] == L':')) n--;     /* keep "C:\" whole */
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
            DWORD m = n ? ExpandEnvironmentStringsW(raw, ex, n + 1) : 0;
            if (m && m <= n + 1) {
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
        DWORD m = GetEnvironmentVariableW(L"PATH", wp, n);
        if (m && m < n) {                      /* else it changed meanwhile: skip */
            char *path = w_to_u8(wp);
            found = search_path_list(path, name);
            free(path);
        }
        free(wp);
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

/* Run a program, collect its stdout+stderr, and stop it if the task is
   paused. The child runs in a job object, so stopping it also stops
   everything it started (yt-dlp's onefile build runs itself again as a
   child, which may start deno). With no console (GUI) the child gets no
   window. Only the pipe and NUL are inherited - not the handles other
   downloads have open. Output beyond 16 MB is drained but not kept.
   Returns the exit code, or RUN_FAILED if it could not start or was stopped. */
int run_process(const char *cmdline, char **captured, const ul_task *t)
{
    const size_t keep_max = (size_t)16 << 20;
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd = NULL, wr = NULL, nul, job;
    PROCESS_INFORMATION pi;
    if (captured) *captured = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return RUN_FAILED;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);

    STARTUPINFOEXW si;
    ZeroMemory(&si, sizeof si);
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = wr;
    si.StartupInfo.hStdError = wr;
    si.StartupInfo.hStdInput = nul;
    HANDLE inherit[2] = { wr, nul };
    int ninherit = nul != INVALID_HANDLE_VALUE ? 2 : 1;
    SIZE_T asize = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &asize);
    si.lpAttributeList = xmalloc(asize);
    BOOL have_list = InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &asize) &&
                     UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                               inherit, sizeof(HANDLE) * (size_t)ninherit, NULL, NULL);

    job = CreateJobObjectW(NULL, NULL);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
        ZeroMemory(&li, sizeof li);
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }

    DWORD flags = (GetConsoleWindow() ? 0 : CREATE_NO_WINDOW) | CREATE_SUSPENDED |
                  (have_list ? EXTENDED_STARTUPINFO_PRESENT : 0);
    wchar_t *w = u8_to_w(cmdline);
    BOOL ok = CreateProcessW(NULL, w, NULL, NULL, TRUE, flags, NULL, NULL, &si.StartupInfo, &pi);
    free(w);
    if (have_list) DeleteProcThreadAttributeList(si.lpAttributeList);
    free(si.lpAttributeList);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        if (job) CloseHandle(job);
        return RUN_FAILED;
    }
    if (job && !AssignProcessToJobObject(job, pi.hProcess)) { CloseHandle(job); job = NULL; }
    ResumeThread(pi.hThread);

    sbuf b = { 0 };
    char buf[8192];
    int killed = 0;
    for (;;) {
        DWORD avail = 0, n = 0;
        if (PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail) {
            if (ReadFile(rd, buf, avail < sizeof buf ? avail : sizeof buf, &n, NULL) && n && b.len < keep_max)
                sb_putn(&b, buf, n);
            continue;
        }
        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
            while (PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail &&
                   ReadFile(rd, buf, avail < sizeof buf ? avail : sizeof buf, &n, NULL) && n)
                if (b.len < keep_max) sb_putn(&b, buf, n);
            break;
        }
        if (!killed && task_aborted(t)) {
            if (job) TerminateJobObject(job, 1);           /* the whole tree */
            else TerminateProcess(pi.hProcess, 1);
            killed = 1;
        }
    }
    CloseHandle(rd);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (job) CloseHandle(job);                              /* ends any leftover grandchild */
    if (captured) *captured = sb_steal(&b);
    else sb_free(&b);
    return killed ? RUN_FAILED : (int)code;
}

/* ffmpeg reads "file:" paths literally: a name starting with "-" can never
   be mistaken for an option */
static char *ffmpeg_path(const char *p) { return xasprintf("file:%s", p); }

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
        if (log && *log && r != RUN_FAILED) {
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
    char *v = ffmpeg_path(video), *a = ffmpeg_path(audio), *o = ffmpeg_path(out);
    const char *args[] = { "-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-i", v, "-i", a,
                           "-map", "0:v:0", "-map", "1:a:0", "-c", "copy", o };
    int r = run_ffmpeg(ff, args, sizeof args / sizeof *args, out, t);
    free(v); free(a); free(o);
    return r;
}

int ffmpeg_remux(const char *ff, const char *in, const char *out, const ul_task *t)
{
    char *i = ffmpeg_path(in), *o = ffmpeg_path(out);
    const char *args[] = { "-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-i", i,
                           "-map", "0:v?", "-map", "0:a?", "-c", "copy", o };
    int r = run_ffmpeg(ff, args, sizeof args / sizeof *args, out, t);
    free(i); free(o);
    return r;
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
