/*
 * U-loader parser fuzz / invariant harness.
 * Includes every source file into one translation unit so static parsers
 * (disposition_filename, hls_attr, parse_media, parse_master, json_string,
 * parse_headers_json, load_state) can be called directly.
 *
 * Every input string is placed so that its terminating NUL is the last
 * byte of a committed page followed by a PAGE_NOACCESS guard page: any
 * read past the end of the input crashes immediately (poor man's ASan).
 *
 * Build v1 (from this folder):
 *   gcc -O1 -g -std=gnu11 -fsanitize=undefined -fsanitize-undefined-trap-on-error \
 *       -DSRC=../../review-v1/src fuzz.c -o fuzz.exe -static -lwinhttp -lbcrypt -lshell32 -lole32
 * Build v2: gcc -O1 -g -std=gnu11 -DV2 -fsanitize=undefined -fsanitize-undefined-trap-on-error fuzz.c
 *       ../../review-v2/src/ftp.c ../../review-v2/src/torrent.c -o fuzz2.exe -static -lwinhttp -lbcrypt
 *       -lws2_32 -lshell32 -lole32 -loleaut32 -luuid -ladvapi32
 * Run:  fuzz.exe [iterations] [seed]      (prints violations, exit 0 = no crash)
 */
#ifdef V2
/* v2 snapshot: ftp.c and torrent.c are linked as separate objects */
#include "../../review-v2/src/util.c"
#include "../../review-v2/src/http.c"
#include "../../review-v2/src/engine.c"
#include "../../review-v2/src/hls.c"
#include "../../review-v2/src/extract.c"
#include "../../review-v2/src/task.c"
#include "../../review-v2/src/host.c"
#else
#define main uloader_main
#include "../../review-v1/src/util.c"
#include "../../review-v1/src/http.c"
#include "../../review-v1/src/engine.c"
#include "../../review-v1/src/hls.c"
#include "../../review-v1/src/extract.c"
#include "../../review-v1/src/main.c"
#undef main
#endif
#include <assert.h>
#include <shellapi.h>   /* CommandLineToArgvW: without it the pointer is truncated to int */

/* ------------------------------------------------------------------ */
static uint64_t rs = 88172645463325252ULL;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)rs; }
static int rr(int n) { return n > 0 ? (int)(rnd() % (uint32_t)n) : 0; }

static long violations = 0;
static char g_ctx[200];
static char g_lastctx[200];
static int g_ctxcount = 0;
static int g_maxper = 12;   /* print at most this many violations per context */
#define VIOL(...) do { violations++; \
    if (strcmp(g_ctx, g_lastctx)) { strcpy(g_lastctx, g_ctx); g_ctxcount = 0; } \
    if (g_ctxcount++ < g_maxper) { printf("[%s] ", g_ctx); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* crash catcher: an access violation inside GUARDED() is reported and
   execution resumes after it (uses __builtin_longjmp, no unwinding) */
static void *g_jb[5];
static volatile int g_armed = 0;
static LONG WINAPI veh(PEXCEPTION_POINTERS ep)
{
    if (g_armed && (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
                    ep->ExceptionRecord->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION)) {
        g_armed = 0;
        __builtin_longjmp(g_jb, 1);
    }
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
        ep->ExceptionRecord->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION ||
        ep->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        /* unexpected crash outside GUARDED(): dump PC + return addresses for addr2line */
        HMODULE mod = GetModuleHandleA(NULL);
        fprintf(stderr, "UNGUARDED CRASH code=%08lx pc=+%llx\n", ep->ExceptionRecord->ExceptionCode,
                (unsigned long long)((uintptr_t)ep->ExceptionRecord->ExceptionAddress - (uintptr_t)mod + 0x140000000ULL));
        uintptr_t *sp = (uintptr_t *)ep->ContextRecord->Rsp;
        for (int i = 0; i < 2048; i++) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(sp + i, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) break;
            if (sp[i] > (uintptr_t)mod && sp[i] < (uintptr_t)mod + 0x400000)
                fprintf(stderr, "  ret +%llx\n", (unsigned long long)(sp[i] - (uintptr_t)mod + 0x140000000ULL));
        }
        fflush(stderr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static long crashes = 0;
#define GUARDED(desc, stmt) do { \
    if (__builtin_setjmp(g_jb) == 0) { g_armed = 1; stmt; g_armed = 0; } \
    else { crashes++; violations++; printf("[%s] CRASH (access violation / UB trap) in %s\n", g_ctx, desc); } } while (0)

/* guarded copy: NUL is the last byte before a no-access page */
typedef struct { char *base; size_t total; char *s; } gbuf;
static gbuf gdup(const char *s, size_t n)
{
    SYSTEM_INFO si; GetSystemInfo(&si);
    size_t pg = si.dwPageSize;
    size_t need = n + 1;
    size_t pages = (need + pg - 1) / pg;
    gbuf g;
    g.total = (pages + 1) * pg;
    g.base = VirtualAlloc(NULL, g.total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD old;
    VirtualProtect(g.base + pages * pg, pg, PAGE_NOACCESS, &old);
    g.s = g.base + pages * pg - need;
    memcpy(g.s, s, n);
    g.s[n] = 0;
    return g;
}
static void gfree(gbuf g) { VirtualFree(g.base, 0, MEM_RELEASE); }

static void gen(sbuf *b, const char **toks, int ntok, int maxparts)
{
    int parts = 1 + rr(maxparts);
    for (int i = 0; i < parts; i++) {
        int k = rr(10);
        if (k < 7) sb_puts(b, toks[rr(ntok)]);
        else if (k < 9) { char c = (char)(32 + rr(95)); sb_putc(b, c); }
        else { char c = (char)(1 + rr(255)); sb_putc(b, c); }
    }
}

/* ------------------------------------------------------------------ */
static int is_device_name(const char *u8name)
{
    wchar_t *w = u8_to_w(u8name);
    size_t n = wcslen(w);
    wchar_t *in = malloc((n + 16) * sizeof(wchar_t));
    swprintf(in, n + 16, L"C:\\tmp\\%ls", w);
    wchar_t out[2048];
    DWORD r = GetFullPathNameW(in, 2048, out, NULL);
    free(in);
    free(w);
    return r && r < 2048 && wcsncmp(out, L"\\\\.\\", 4) == 0;
}

static void check_clean_name(const char *in, const char *s)
{
    if (!s || !*s) { VIOL("empty sanitized name for input"); return; }
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p < 32 || strchr("<>:\"/\\|?*", *p)) { VIOL("forbidden char 0x%02x in sanitized name \"%s\"", *p, s); break; }
    size_t n = strlen(s);
    if (s[n - 1] == ' ' || s[n - 1] == '.') VIOL("sanitized name ends with space/dot: \"%s\"", s);
    if (n > 200) VIOL("sanitized name too long (%zu)", n);
    if (!strcmp(s, ".") || !strcmp(s, "..")) VIOL("dot name \"%s\"", s);
    if (utf8_valid((const unsigned char *)in, strlen(in)) && !utf8_valid((const unsigned char *)s, n))
        VIOL("sanitize produced invalid UTF-8 from valid input: \"%s\"", s);
    const char *suf[] = { "", ".ulpart", ".ulstate", ".ulstate.tmp", ".mp4", ".ulhls" };
    for (int i = 0; i < 6; i++) {
        char *t = xasprintf("%s%s", s, suf[i]);
        if (is_device_name(t)) { VIOL("DEVICE NAME survives sanitize: input=\"%s\" -> \"%s\"", in, t); free(t); break; }
        free(t);
    }
}

/* ------------------------------------------------------------------ */
static const char *name_toks[] = { "CON", "con", "NUL", "nul", "AUX", "PRN", "COM1", "LPT9", "COM\xC2\xB9",
    "LPT\xC2\xB2", "CONIN$", "CONOUT$", " ", ".", "..", "\\", "/", ":", ".txt", ".mp4", "\t", "a", "b", "%2e",
    "%2f", "%5c", "\xE0\xA6\xAC", "\xC3", "\xFF", "*", "?", "\"", "~1", "x.y.z", "\xE3\x80\x80" };
static const char *url_toks[] = { "http://", "https://", "h", "host", ":80", "@", "/", "//", "./", "../",
    ".", "..", "?", "#", "a", "b.m3u8", "%2F", "%", "%zz", "%00", "[::1]", "]", ":", "x.ts", "seg", "=" };
static const char *cd_toks[] = { "attachment", ";", " ", "filename=", "filename*=", "FILENAME=", "\"",
    "UTF-8''", "'", "%2e%2e%5c", "..\\", "../", "\\\"", "CON", "nul .txt", "a.bin", "%", "%E0%A6%AC",
    "\\", "''", "x", "=", "\xC3\xA9", "CONOUT$" };
static const char *m3u_toks[] = { "#EXTM3U\n", "#EXT-X-STREAM-INF:", "BANDWIDTH=", "RESOLUTION=", "1920x1080",
    "AUDIO=\"a\"", "#EXT-X-MEDIA:", "TYPE=AUDIO", "GROUP-ID=\"a\"", "URI=\"", "URI=", "\"", ",", "\n",
    "#EXTINF:", "10.0,", "-1,", "nan,", "1e308,", "#EXT-X-KEY:", "METHOD=AES-128", "METHOD=NONE",
    "METHOD=SAMPLE-AES", "IV=0x", "IV=", "00112233445566778899aabbccddeeff", "0X1", "zz",
    "#EXT-X-BYTERANGE:", "100", "@", "0", "-5", "9223372036854775807", "#EXT-X-MAP:", "BYTERANGE=\"",
    "#EXT-X-MEDIA-SEQUENCE:", "-9223372036854775808", "#EXT-X-ENDLIST", "seg.ts", "../x.ts",
    "http://o/s.ts", "//cdn/x", "?q", "#", " ", "\r\n", "=", "DEFAULT=YES", "k.bin" };
static const char *json_toks[] = { "{", "}", "\"", "\\", "\\u", "\\uD800", "\\uDC00", "\\uD83D\\uDE00",
    "\\n", "\\\"", "\\\\", ":", ",", " ", "User-Agent", "X", "0", "1", "ab", "\\u0000", "\\u00e9", "null",
    "[", "]", "\\uD800\\u", "\\uD800\\uZZ" };
static const char *state_toks[] = { "ULSTATE 1\n", "url http://x\n", "total ", "1000", "999", "-1",
    "9223372036854775807", "\n", "etag ", "\"abc\"", "etag\n", "lastmod x\n", "seg ", "0 ", "100", "500 ",
    "1000", "1001", "-5 ", " ", "seg 0 1000\n", "seg 10 5\n", "seg 0 0\n", "\r\n", "x", "end\n", "end\n" };
static const char *arg_toks[] = { "\\", "\\\\", "\"", "\\\"", " ", "\t", "a", "b c", "\"\"", "\\\\\"",
    "%PATH%", "^", "&", "|", "<", ">", "\n", "\xC3\xA9", "''", "--", "-o" };

/* ------------------------------------------------------------------ */
static void fuzz_sanitize(int it)
{
    snprintf(g_ctx, sizeof g_ctx, "sanitize");
    for (int i = 0; i < it; i++) {
        sbuf b = { 0 }; gen(&b, name_toks, sizeof name_toks / sizeof *name_toks, 8);
        if (!b.s) continue;
        gbuf g = gdup(b.s, b.len);
        char *s = NULL;
        GUARDED("sanitize_filename", s = sanitize_filename(g.s));
        if (s) check_clean_name(g.s, s);
        free(s);
        gfree(g); sb_free(&b);
    }
    /* long names around the 180-byte limit */
    for (int i = 0; i < it / 10; i++) {
        sbuf b = { 0 };
        int L = 170 + rr(30);
        for (int k = 0; k < L; k++) sb_puts(&b, rr(4) ? "a" : (rr(2) ? " " : (rr(2) ? "." : "\xE0\xA6\xAC")));
        if (rr(2)) sb_puts(&b, rr(2) ? ".mp4" : ".verylongextension");
        gbuf g = gdup(b.s, b.len);
        char *s = sanitize_filename(g.s);
        check_clean_name(g.s, s);
        free(s);
        gfree(g); sb_free(&b);
    }
}

static void fuzz_urls(int it)
{
    for (int i = 0; i < it; i++) {
        sbuf a = { 0 }, r = { 0 };
        gen(&a, url_toks, sizeof url_toks / sizeof *url_toks, 10);
        gen(&r, url_toks, sizeof url_toks / sizeof *url_toks, 8);
        if (!a.s || !r.s) { sb_free(&a); sb_free(&r); continue; }
        gbuf ga = gdup(a.s, a.len), gr = gdup(r.s, r.len);
        snprintf(g_ctx, sizeof g_ctx, "url_filename");
        char *fn = url_filename(ga.s);
        if (fn) { char *s = sanitize_filename(fn); check_clean_name(fn, s); free(s); free(fn); }
        snprintf(g_ctx, sizeof g_ctx, "url_host");
        free(url_host(ga.s));
        snprintf(g_ctx, sizeof g_ctx, "is_video_site");
        is_video_site(ga.s);
        snprintf(g_ctx, sizeof g_ctx, "url_resolve");
        char *res = NULL;
        GUARDED("url_resolve", res = url_resolve(ga.s, gr.s));
        if (!res) VIOL("url_resolve returned NULL");
        else {
            /* when base is absolute and ref is relative, the result must keep base's scheme://authority */
            const char *se = strstr(ga.s, "://");
            const char *p = gr.s;
            while (isalnum((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.') p++;
            int ref_abs = (p > gr.s && !strncmp(p, "://", 3)) || (gr.s[0] == '/' && gr.s[1] == '/');
            if (se && !ref_abs && *gr.s) {
                size_t al; const char *au = url_authority(ga.s, &al);
                size_t pre = (size_t)(au + al - ga.s);
                if (strncmp(res, ga.s, pre) != 0) VIOL("url_resolve changed origin: base=\"%s\" ref=\"%s\" -> \"%s\"", ga.s, gr.s, res);
                size_t q = strcspn(res + pre, "?#");
                char *path = xstrndup(res + pre, q);
                if (strstr(path, "/../") || strstr(path, "/./") || str_iends(path, "/..") || str_iends(path, "/."))
                    VIOL("dot segment survived: \"%s\"", res);
                free(path);
            }
            free(res);
        }
        snprintf(g_ctx, sizeof g_ctx, "url_decode");
        free(url_decode(ga.s, strlen(ga.s)));
        gfree(ga); gfree(gr); sb_free(&a); sb_free(&r);
    }
}

static void fuzz_disposition(int it)
{
    snprintf(g_ctx, sizeof g_ctx, "disposition");
    for (int i = 0; i < it; i++) {
        sbuf b = { 0 }; gen(&b, cd_toks, sizeof cd_toks / sizeof *cd_toks, 10);
        if (!b.s) continue;
        gbuf g = gdup(b.s, b.len);
        char *n = NULL;
        GUARDED("disposition_filename", n = disposition_filename(g.s));
        if (n) {
            char *s = sanitize_filename(n);
            check_clean_name(n, s);
            char *p = path_join("C:\\out", s);
            if (strncmp(p, "C:\\out\\", 7) || strchr(p + 7, '\\') || strchr(p + 7, '/')) VIOL("escapes out dir: %s", p);
            free(p); free(s); free(n);
        }
        gfree(g); sb_free(&b);
    }
}

static void check_media(hls_media *m, const char *text)
{
    for (int i = 0; i < m->nsegs; i++) {
        hls_seg *s = &m->segs[i];
        if (!s->url) VIOL("segment without url");
        if (s->key < -1 || s->key >= m->nkeys) VIOL("bad key index %d", s->key);
        if (s->br_len >= 0 && s->br_off < 0) VIOL("byterange len without offset");
        if (s->br_len == 0) VIOL("zero-length BYTERANGE accepted -> fetch_segment requests open range (whole file)");
        if (s->br_len > 0 && s->br_off > INT64_MAX - s->br_len) VIOL("BYTERANGE off+len overflows int64 (off=%lld len=%lld)", (long long)s->br_off, (long long)s->br_len);
        if (s->br_len < -1) VIOL("negative BYTERANGE length %lld accepted (silently becomes whole file)", (long long)s->br_len);
    }
    for (int k = 0; k < m->nkeys; k++)
        if (m->keys[k].method == KEY_AES128 && !m->keys[k].uri) VIOL("AES key without URI");
    (void)text;
}

static void fuzz_hls(int it)
{
    for (int i = 0; i < it; i++) {
        sbuf b = { 0 }; gen(&b, m3u_toks, sizeof m3u_toks / sizeof *m3u_toks, 40);
        if (!b.s) continue;
        gbuf g = gdup(b.s, b.len);
        snprintf(g_ctx, sizeof g_ctx, "hls_attr");
        const char *keys[] = { "URI", "IV", "METHOD", "BYTERANGE", "BANDWIDTH", "" };
        free(hls_attr(g.s, keys[rr(6)]));
        snprintf(g_ctx, sizeof g_ctx, "parse_media");
        hls_media m;
        long pm_before = crashes;
        memset(&m, 0, sizeof m);
        GUARDED("parse_media", parse_media(g.s, rr(2) ? "http://h/a/b/index.m3u8?x=1" : "http://h", &m));
        if (crashes != pm_before) {
            static int shown = 0;
            if (shown++ < 3) {
                printf("    playlist: ");
                for (const char *q = b.s; *q; q++) { if (*q == '\n') printf("\\n"); else putchar(*q); }
                printf("\n");
            }
            gfree(g); sb_free(&b);
            continue;          /* m is half-built: do not touch it */
        }
        check_media(&m, g.s);
        media_free(&m);
        snprintf(g_ctx, sizeof g_ctx, "parse_master");
        hls_variant *v = NULL; char **au = NULL, **ag = NULL; int na = 0;
        int nv = parse_master(g.s, "http://h/m.m3u8", &v, &au, &ag, &na);
        if (nv > 0) { int bsel = pick_variant(v, nv, rr(2) ? 0 : 720); if (bsel < 0 || bsel >= nv) VIOL("pick_variant out of range"); }
        for (int k = 0; k < nv; k++) { free(v[k].url); free(v[k].audio_group); }
        for (int k = 0; k < na; k++) { free(au[k]); free(ag[k]); }
        free(v); free(au); free(ag);
        snprintf(g_ctx, sizeof g_ctx, "parse_hex_iv");
        uint8_t iv[16]; parse_hex_iv(g.s, iv);
        gfree(g); sb_free(&b);
    }
    /* targeted: huge / empty */
    snprintf(g_ctx, sizeof g_ctx, "parse_media-big");
    sbuf big = { 0 };
    sb_puts(&big, "#EXTM3U\n");
    for (int i = 0; i < 200000; i++) sb_puts(&big, "#EXTINF:1,\ns.ts\n");
    hls_media m; parse_media(big.s, "http://h/x.m3u8", &m);
    if (m.nsegs != 200000) VIOL("big playlist parsed %d segs", m.nsegs);
    media_free(&m); sb_free(&big);
    parse_media("", "http://h/", &m); media_free(&m);
    gbuf g1 = gdup("#EXT-X-KEY:METHOD=AES-128,URI=\"", 31); parse_media(g1.s, "http://h/", &m); media_free(&m); gfree(g1);
}

static void fuzz_json(int it)
{
    snprintf(g_ctx, sizeof g_ctx, "json");
    for (int i = 0; i < it; i++) {
        sbuf b = { 0 }; gen(&b, json_toks, sizeof json_toks / sizeof *json_toks, 20);
        if (!b.s) continue;
        gbuf g = gdup(b.s, b.len);
        ul_headers h = { 0 };
        static int jcr = 0;
        long before = crashes;
        GUARDED("parse_headers_json(random)", parse_headers_json(g.s, &h));
        if (crashes != before && jcr++ < 3) printf("    input: %s\n", g.s);
        if (crashes != before) { gfree(g); sb_free(&b); continue; }
        for (int k = 0; k < h.n; k++)
            if (strpbrk(h.value[k], "\r\n") || strpbrk(h.name[k], "\r\n:"))
                VIOL("CR/LF/colon in header from JSON -> request header injection: name=\"%s\"", h.name[k]);
        headers_free(&h);
        gfree(g); sb_free(&b);
    }
    /* targeted: truncated surrogate escape at the very end of the buffer */
    const char *t[] = { "{\"a\":\"\\uD800\\u", "{\"a\":\"\\uD800\\u1", "{\"a\":\"\\uD800\\u12", "{\"a\":\"\\uD800\\" };
    for (int i = 0; i < 4; i++) {
        snprintf(g_ctx, sizeof g_ctx, "json-trunc-%d", i);
        gbuf g = gdup(t[i], strlen(t[i]));
        ul_headers h = { 0 };
        GUARDED(t[i], parse_headers_json(g.s, &h));     /* crashes if it reads past the NUL */
        headers_free(&h);
        gfree(g);
    }
}

static void fuzz_state(int it)
{
    snprintf(g_ctx, sizeof g_ctx, "load_state");
    char tmp[MAX_PATH]; GetTempPathA(MAX_PATH, tmp);
    char *path = xasprintf("%sulfuzz_%lu.ulstate", tmp, (unsigned long)GetCurrentProcessId());
    for (int i = 0; i < it; i++) {
        sbuf b = { 0 };
        if (rr(3)) sb_puts(&b, "ULSTATE 1\n");
        gen(&b, state_toks, sizeof state_toks / sizeof *state_toks, 30);
        FILE *f = fopen(path, "wb"); fwrite(b.s, 1, b.len, f); fclose(f);
        dl_ctx *c = xcalloc(1, sizeof *c);
        InitializeCriticalSection(&c->lock);
        c->total = rr(2) ? 1000 : 999;
        c->etag = rr(2) ? xstrdup("\"abc\"") : NULL;
        if (load_state(c, path)) {
            if (c->nsegs < 0 || c->nsegs > MAX_SEGS) VIOL("nsegs %d", c->nsegs);
            for (int k = 0; k < c->nsegs; k++) {
                if (c->segs[k].pos < 0 || c->segs[k].end > c->total || c->segs[k].pos >= c->segs[k].end)
                    VIOL("bad seg %lld-%lld", (long long)c->segs[k].pos, (long long)c->segs[k].end);
                for (int j = 0; j < k; j++)
                    if (c->segs[j].pos < c->segs[k].end && c->segs[k].pos < c->segs[j].end) {
                        static int once; if (!once++) VIOL("overlapping segments accepted from state (e.g. %lld-%lld / %lld-%lld)",
                            (long long)c->segs[j].pos, (long long)c->segs[j].end, (long long)c->segs[k].pos, (long long)c->segs[k].end);
                    }
            }
            if (c->nsegs == 0 && !strstr(b.s, "seg ")) {
                static int once2; if (!once2++) VIOL("state with NO seg lines accepted as 'everything downloaded' (file: %.60s...)", b.s);
            }
        }
        DeleteCriticalSection(&c->lock);
        free(c->etag); free(c);
        sb_free(&b);
    }
    DeleteFileA(path);
    free(path);
}

static void fuzz_cmdline(int it)
{
    snprintf(g_ctx, sizeof g_ctx, "cmd_append_arg");
    for (int i = 0; i < it; i++) {
        sbuf a = { 0 };
        if (rr(10)) gen(&a, arg_toks, sizeof arg_toks / sizeof *arg_toks, 8);
        char *arg = a.s ? a.s : xstrdup("");
        if (a.s == NULL) a.s = arg;
        sbuf c = { 0 };
        cmd_append_arg(&c, "C:\\x\\prog.exe");
        cmd_append_arg(&c, arg);
        cmd_append_arg(&c, "END");
        wchar_t *w = u8_to_w(c.s);
        int argc = 0;
        wchar_t **av = CommandLineToArgvW(w, &argc);
        wchar_t *want = u8_to_w(arg);
        int bad = 0;
        long before = crashes;
        GUARDED("CommandLineToArgvW compare", bad = !av || argc != 3 || wcscmp(av[1], want) || wcscmp(av[2], L"END"));
        if (crashes != before) {
            printf("    arg bytes:");
            for (const unsigned char *q = (const unsigned char *)arg; *q; q++) printf(" %02x", *q);
            printf("  argc=%d av=%p\n", argc, (void *)av);
        }
        if (bad)
            VIOL("quoting mismatch for arg [%s] -> cmdline [%s] (argc=%d)", arg, c.s, argc);
        LocalFree(av); free(w); free(want);
        sb_free(&c); sb_free(&a);
    }
}

#ifdef V2
/* ------------------------------------------------------------------ */
/* v2-only targets: JSON reader, native-host cookie writer, job files,  */
/* cookie jar scoping                                                   */
/* ------------------------------------------------------------------ */
static const char *msg_toks[] = { "{", "}", "[", "]", "\"", "\\", "\\u", "\\uD800", "\\u0000", ":", ",", " ",
    "\"action\"", "\"download\"", "\"ping\"", "\"cookies\"", "\"domain\"", "\"name\"", "\"value\"", "\"path\"",
    "\"hostOnly\"", "true", "false", "null", "\"expirationDate\"", "1e999", "-1", "\"secure\"", "\".example.com\"",
    "\"a\\tb\"", "\"a\\nb\"", "\"x=y\"", "123", "\"\\\"\"", "{\"domain\":\"x.com\",\"name\":\"n\",\"value\":\"v\"}" };

static void fuzz_json_v2(int it)
{
    snprintf(g_ctx, sizeof g_ctx, "json_find/json_str");
    for (int i = 0; i < it; i++) {
        sbuf b = { 0 }; gen(&b, msg_toks, sizeof msg_toks / sizeof *msg_toks, 40);
        if (!b.s) continue;
        gbuf g = gdup(b.s, b.len);
        const char *keys[] = { "action", "cookies", "url", "", "domain" };
        const char *v = NULL;
        GUARDED("json_find", v = json_find(g.s, keys[rr(5)]));
        char *s = NULL;
        GUARDED("json_str", s = json_str(v));
        free(s);
        GUARDED("json_skip", json_skip(g.s));
        snprintf(g_ctx, sizeof g_ctx, "write_cookie_file");
        char *cf = NULL;
        sbuf m = { 0 };
        sb_puts(&m, "{\"cookies\":[");
        sb_puts(&m, g.s);
        gbuf g2 = gdup(m.s, m.len);
        GUARDED("write_cookie_file", cf = write_cookie_file(g2.s));
        if (cf) {
            /* every line of the written cookies.txt must have exactly 7 tab-separated fields */
            size_t L; char *data = read_file(cf, &L);
            int lineno = 0;
            for (char *line = strtok(data, "\n"); line; line = strtok(NULL, "\n")) {
                if (lineno++ == 0) continue;
                int tabs = 0; for (char *q = line; *q; q++) tabs += *q == '\t';
                if (tabs != 6) VIOL("cookie line with %d tabs: %s", tabs, line);
                if (line[0] == '#') VIOL("cookie line turned into a comment: %s", line);
            }
            free(data);
            file_delete(cf); free(cf);
        }
        gfree(g2); sb_free(&m);
        snprintf(g_ctx, sizeof g_ctx, "json_find/json_str");
        gfree(g); sb_free(&b);
    }
}

static void fuzz_job(int it)
{
    snprintf(g_ctx, sizeof g_ctx, "job_write/job_load");
    static const char *fld[] = { "a", "=", "\n", "\r", "url=http://evil/", "dir=C:\\x", "\x01", " ", "cookies=",
        "\xE0\xA6\xAC", "name=..\\..\\x", "%", "\t", "U-LOADER-JOB 1" };
    for (int i = 0; i < it; i++) {
        char *v[6];
        for (int k = 0; k < 6; k++) {
            sbuf b = { 0 }; gen(&b, fld, sizeof fld / sizeof *fld, 6);
            v[k] = b.s ? b.s : xstrdup("x");
        }
        char *url = xasprintf("http://h/%s", v[0]);
        for (char *p = url; *p; p++) if (*p == '\r' || *p == '\n') *p = '_';   /* host rejects CR/LF in url */
        char *job = job_write(url, NULL, v[1], v[2], v[3], v[4], rr(40) - 5, rr(3000), NULL);
        if (!job) { VIOL("job_write failed"); continue; }
        ul_config cfg; cfg_defaults(&cfg);
        char *u2 = NULL;
        int r = job_load(job, &cfg, &u2);
        if (r != 0 || !u2 || strcmp(u2, url)) VIOL("url not round-tripped: [%s] -> [%s]", url, u2 ? u2 : "(null)");
        if (cfg.cookie_file) VIOL("field injection produced a cookies= line: %s", cfg.cookie_file);
        if (cfg.out_dir && strpbrk(v[4], "\r\n") == NULL && strcmp(cfg.out_dir, v[4]) && *v[4])
            VIOL("dir changed: [%s] -> [%s]", v[4], cfg.out_dir);
        if (cfg.out_name && (strchr(cfg.out_name, '\\') || strchr(cfg.out_name, '/')))
            VIOL("name keeps a path separator: %s", cfg.out_name);
        free(u2); free(url);
        file_delete(job); free(job);
        free(cfg.out_name); free(cfg.out_dir); free(cfg.referer); free(cfg.user_agent); free(cfg.cookie_file);
        for (int k = 0; k < 6; k++) free(v[k]);
    }
}

static void cookie_case(const char *jar_line, const char *url, int expect)
{
    char tmp[MAX_PATH]; GetTempPathA(MAX_PATH, tmp);
    char *path = xasprintf("%sulck_%lu.txt", tmp, (unsigned long)GetCurrentProcessId());
    FILE *f = fopen(path, "wb"); fprintf(f, "%s\nzzz\tTRUE\t/\tFALSE\t0\tdummy\t1\n", jar_line); fclose(f);
    ul_config cfg; cfg_defaults(&cfg);
    cookies_load(&cfg, path);
    char *h = cookie_header_for(&cfg, url);
    int got = h && strstr(h, "SECRET") != NULL;
    printf("  %-4s cookie [%s] -> %s : %s\n", got == expect ? "ok" : "BAD", jar_line, url, got ? "SENT" : "not sent");
    if (got != expect) VIOL("cookie scoping: [%s] for %s %s", jar_line, url, got ? "SENT" : "not sent");
    free(h); DeleteFileA(path); free(path);
    for (int i = 0; i < cfg.ncookies; i++) { free(cfg.cookies[i].domain); free(cfg.cookies[i].path); free(cfg.cookies[i].name); free(cfg.cookies[i].value); }
    free(cfg.cookies);
}

static void cookie_table(void)
{
    snprintf(g_ctx, sizeof g_ctx, "cookies");
    cookie_case(".example.com\tTRUE\t/\tFALSE\t0\tsid\tSECRET", "http://example.com/x", 1);
    cookie_case(".example.com\tTRUE\t/\tFALSE\t0\tsid\tSECRET", "http://a.b.example.com/x", 1);
    cookie_case(".example.com\tTRUE\t/\tFALSE\t0\tsid\tSECRET", "http://evil-example.com/x", 0);
    cookie_case(".example.com\tTRUE\t/\tFALSE\t0\tsid\tSECRET", "http://example.com.evil.net/x", 0);
    cookie_case(".example.com\tTRUE\t/\tFALSE\t0\tsid\tSECRET", "http://evil.net/?example.com", 0);
    cookie_case(".example.com\tTRUE\t/\tFALSE\t0\tsid\tSECRET", "http://evil.net/.example.com", 0);
    cookie_case(".example.com\tTRUE\t/\tFALSE\t0\tsid\tSECRET", "http://example.com@evil.net/x", 0);
    cookie_case(".example.com\tTRUE\t/\tFALSE\t0\tsid\tSECRET", "http://evil.net#@example.com/x", 0);
    cookie_case(".example.com\tTRUE\t/\tFALSE\t0\tsid\tSECRET", "http://EXAMPLE.COM/x", 1);
    cookie_case("example.com\tFALSE\t/\tFALSE\t0\tsid\tSECRET", "http://sub.example.com/x", 0);
    cookie_case("example.com\tFALSE\t/\tFALSE\t0\tsid\tSECRET", "http://example.com:8080/x", 1);
    cookie_case("example.com\tFALSE\t/\tTRUE\t0\tsid\tSECRET", "http://example.com/x", 0);
    cookie_case("example.com\tFALSE\t/\tTRUE\t0\tsid\tSECRET", "https://example.com/x", 1);
    cookie_case("example.com\tFALSE\t/a\tFALSE\t0\tsid\tSECRET", "http://example.com/ab", 0);
    cookie_case("example.com\tFALSE\t/a\tFALSE\t0\tsid\tSECRET", "http://example.com/a/b", 1);
    cookie_case("example.com\tFALSE\t/\tFALSE\t1\tsid\tSECRET", "http://example.com/x", 0);
    cookie_case("127.0.0.1\tFALSE\t/\tFALSE\t0\tsid\tSECRET", "http://127.0.0.1/x", 1);
    cookie_case("127.0.0.1\tFALSE\t/\tFALSE\t0\tsid\tSECRET", "http://10.127.0.0.1/x", 0);
    cookie_case(".0.0.1\tTRUE\t/\tFALSE\t0\tsid\tSECRET", "http://127.0.0.1/x", 0);
    cookie_case(".com\tTRUE\t/\tFALSE\t0\tsid\tSECRET", "http://anything.com/x", 0);
    cookie_case(".example.com\tTRUE\t/\tFALSE\t0\tsid\tSECRET", "http://example.com./x", 1);
    cookie_case("[::1]\tFALSE\t/\tFALSE\t0\tsid\tSECRET", "http://[::1]:8080/x", 1);
    /* host-only flag written by the native host for a hostOnly cookie whose domain has a dot */
    cookie_case(".example.com\tFALSE\t/\tFALSE\t0\tsid\tSECRET", "http://sub.example.com/x", 0);
}
#endif

int main(int argc, char **argv)
{
    int it = argc > 1 ? atoi(argv[1]) : 20000;
    if (argc > 2) rs = (uint64_t)_strtoui64(argv[2], NULL, 10) | 1;
    setvbuf(stdout, NULL, _IONBF, 0);
    AddVectoredExceptionHandler(1, veh);
    SetConsoleOutputCP(CP_UTF8);
    printf("sanitize...\n");    fuzz_sanitize(it);
    printf("urls...\n");        fuzz_urls(it);
    printf("disposition...\n"); fuzz_disposition(it);
    printf("hls...\n");         fuzz_hls(it);
    printf("json...\n");        fuzz_json(it);
    printf("state...\n");       fuzz_state(it / 4);
    printf("cmdline...\n");     fuzz_cmdline(it);
#ifdef V2
    printf("json v2 / native-host cookies...\n"); fuzz_json_v2(it);
    printf("job files...\n");   fuzz_job(it / 4);
    printf("cookie scoping table:\n"); cookie_table();
#endif
    printf("done, %ld violations, %ld crashes\n", violations, crashes);
    return 0;
}
