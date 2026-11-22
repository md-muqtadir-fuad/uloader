/*
 * U-loader - HLS (.m3u8) stream downloader.
 *
 * Picks the best quality from a master playlist, downloads the media
 * segments in parallel, decrypts AES-128 segments (Windows CNG), and joins
 * them into one file. Segments are cached in "<name>.ulhls\" so an
 * interrupted download resumes. With ffmpeg available the result is
 * remuxed to .mp4 (and separate audio renditions are merged in).
 */
#include "uloader.h"
#include <process.h>
#include <bcrypt.h>
#include <errno.h>

#ifndef NT_SUCCESS
#define NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#endif

enum { KEY_AES128 = 1, KEY_UNSUPPORTED = 2 };

typedef struct {
    int      method;
    char    *uri;
    uint8_t  iv[16];
    int      has_iv;
    uint8_t  data[16];
    int      loaded;
} hls_key;

typedef struct {
    char    *url;
    int      key;          /* index into keys, -1 = clear */
    int64_t  seq;          /* media sequence number (default IV) */
    int64_t  br_off, br_len;
    double   dur;
} hls_seg;

typedef struct {
    hls_seg *segs;
    int      nsegs, segcap;
    hls_key *keys;
    int      nkeys, keycap;
    char    *map_url;
    int64_t  map_off, map_len;
    int      map_key;
    int      endlist;
    double   duration;
} hls_media;

typedef struct {
    char    *url;
    int64_t  bw;
    int      width, height;
    char    *audio_group;
} hls_variant;

/* ------------------------------------------------------------------ */
/* playlist parsing                                                    */
/* ------------------------------------------------------------------ */

/* Value of KEY in an attribute list like: A=1,B="x,y",C=0x10 */
static char *hls_attr(const char *s, const char *key)
{
    size_t kl = strlen(key);
    const char *p = s;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        const char *ns = p;
        while (*p && *p != '=' && *p != ',') p++;
        size_t nl = (size_t)(p - ns);
        if (*p != '=') continue;
        p++;
        const char *vs;
        size_t vl;
        if (*p == '"') {
            vs = ++p;
            while (*p && *p != '"') p++;
            vl = (size_t)(p - vs);
            if (*p) p++;
        } else {
            vs = p;
            while (*p && *p != ',') p++;
            vl = (size_t)(p - vs);
        }
        if (nl == kl && _strnicmp(ns, key, kl) == 0) return xstrndup(vs, vl);
    }
    return NULL;
}

/* Iterate lines: returns next trimmed line (modifies buffer) or NULL. */
static char *next_line(char **cursor)
{
    char *p = *cursor;
    if (!p || !*p) return NULL;
    char *nl = strchr(p, '\n');
    if (nl) { *nl = 0; *cursor = nl + 1; }
    else *cursor = p + strlen(p);
    return str_trim(p);
}

static int parse_hex_iv(const char *s, uint8_t iv[16])
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    size_t n = strlen(s);
    memset(iv, 0, 16);
    if (n == 0 || n > 32) return 0;
    for (size_t i = 0; i < n; i++) {
        int c = tolower((unsigned char)s[n - 1 - i]);
        int v = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
        if (v < 0) return 0;
        iv[15 - i / 2] |= (uint8_t)(i % 2 ? v << 4 : v);
    }
    return 1;
}

static int is_master(const char *text)
{
    return strstr(text, "#EXT-X-STREAM-INF") != NULL;
}

static int parse_master(const char *text, const char *base, hls_variant **out,
                        char ***audio_urls, char ***audio_groups, int *naudio)
{
    char *buf = xstrdup(text), *cur = buf, *line;
    hls_variant *v = NULL;
    int n = 0, cap = 0, pending = 0, na = 0;
    hls_variant pv;
    memset(&pv, 0, sizeof pv);
    char **aurl = NULL, **agrp = NULL;

    while ((line = next_line(&cur)) != NULL) {
        if (str_istarts(line, "#EXT-X-STREAM-INF:")) {
            const char *at = line + 18;
            char *bw = hls_attr(at, "BANDWIDTH"), *res = hls_attr(at, "RESOLUTION");
            if (pending) free(pv.audio_group);      /* a STREAM-INF without its URI line */
            memset(&pv, 0, sizeof pv);
            pv.bw = bw ? _strtoi64(bw, NULL, 10) : 0;
            if (res) sscanf(res, "%dx%d", &pv.width, &pv.height);
            pv.audio_group = hls_attr(at, "AUDIO");
            free(bw);
            free(res);
            pending = 1;
        } else if (str_istarts(line, "#EXT-X-MEDIA:")) {
            const char *at = line + 13;
            char *type = hls_attr(at, "TYPE"), *uri = hls_attr(at, "URI");
            char *grp = hls_attr(at, "GROUP-ID"), *def = hls_attr(at, "DEFAULT");
            if (type && grp && _stricmp(type, "AUDIO") == 0) {
                aurl = xrealloc(aurl, sizeof(char *) * (size_t)(na + 1));
                agrp = xrealloc(agrp, sizeof(char *) * (size_t)(na + 1));
                /* no URI = this audio is muxed into the video stream (kept so
                   that a DEFAULT one is not replaced by a commentary track) */
                aurl[na] = uri ? url_resolve(base, uri) : NULL;
                agrp[na] = xasprintf("%s%s", (def && _stricmp(def, "YES") == 0) ? "*" : "", grp);
                na++;
            }
            free(type); free(uri); free(grp); free(def);
        } else if (*line && *line != '#' && pending) {
            if (n == cap) { cap = cap ? cap * 2 : 8; v = xrealloc(v, sizeof *v * (size_t)cap); }
            pv.url = url_resolve(base, line);
            v[n++] = pv;
            pending = 0;
        }
    }
    if (pending) free(pv.audio_group);
    free(buf);
    *out = v;
    *audio_urls = aurl;
    *audio_groups = agrp;
    *naudio = na;
    return n;
}

static int parse_media(const char *text, const char *base, hls_media *m)
{
    char *buf = xstrdup(text), *cur = buf, *line;
    int64_t seq0 = 0, next_off = 0, br_len = -1, br_off = -1;
    int cur_key = -1, idx = 0;
    double dur = 0;
    char *last_url = NULL;
    memset(m, 0, sizeof *m);
    m->map_key = -1;
    m->map_len = -1;

    while ((line = next_line(&cur)) != NULL) {
        if (!*line) continue;
        if (str_istarts(line, "#EXT-X-MEDIA-SEQUENCE:")) {
            seq0 = _strtoi64(line + 22, NULL, 10);
        } else if (str_istarts(line, "#EXTINF:")) {
            dur = strtod(line + 8, NULL);
        } else if (str_istarts(line, "#EXT-X-KEY:")) {
            const char *at = line + 11;
            char *method = hls_attr(at, "METHOD");
            if (!method || _stricmp(method, "NONE") == 0) {
                cur_key = -1;
            } else {
                if (m->nkeys == m->keycap) {
                    m->keycap = m->keycap ? m->keycap * 2 : 4;
                    m->keys = xrealloc(m->keys, sizeof(hls_key) * (size_t)m->keycap);
                }
                hls_key *k = &m->keys[m->nkeys];
                memset(k, 0, sizeof *k);
                char *uri = hls_attr(at, "URI"), *iv = hls_attr(at, "IV");
                k->method = (_stricmp(method, "AES-128") == 0 && uri) ? KEY_AES128 : KEY_UNSUPPORTED;
                k->uri = uri ? url_resolve(base, uri) : NULL;
                if (iv) k->has_iv = parse_hex_iv(iv, k->iv);
                free(uri);
                free(iv);
                cur_key = m->nkeys++;
            }
            free(method);
        } else if (str_istarts(line, "#EXT-X-MAP:")) {
            const char *at = line + 11;
            char *uri = hls_attr(at, "URI"), *br = hls_attr(at, "BYTERANGE");
            if (uri) {
                free(m->map_url);
                m->map_url = url_resolve(base, uri);
                m->map_key = cur_key;
                m->map_len = -1;                    /* a new map is whole unless it says otherwise */
                m->map_off = 0;
                if (br) {
                    long long l = 0, o = 0;
                    int k = sscanf(br, "%lld@%lld", &l, &o);
                    /* only sane ranges: positive length, no overflow */
                    if (k >= 1 && l > 0 && o >= 0 && o <= INT64_MAX - l) {
                        m->map_len = l;
                        m->map_off = k == 2 ? o : 0;
                    }
                }
            }
            free(uri);
            free(br);
        } else if (str_istarts(line, "#EXT-X-BYTERANGE:")) {
            long long l = 0, o = 0;
            int k = sscanf(line + 17, "%lld@%lld", &l, &o);
            br_len = (k >= 1 && l > 0) ? l : -1;
            br_off = (k == 2 && o >= 0 && br_len > 0 && o <= INT64_MAX - br_len) ? o : -1;
        } else if (str_istarts(line, "#EXT-X-ENDLIST")) {
            m->endlist = 1;
        } else if (*line != '#') {
            if (m->nsegs == m->segcap) {
                m->segcap = m->segcap ? m->segcap * 2 : 64;
                m->segs = xrealloc(m->segs, sizeof(hls_seg) * (size_t)m->segcap);
            }
            hls_seg *sg = &m->segs[m->nsegs++];
            sg->url = url_resolve(base, line);
            sg->key = cur_key;
            sg->seq = (int64_t)((uint64_t)seq0 + (uint64_t)idx++);   /* wraps, never overflows */
            sg->dur = dur;
            sg->br_len = br_len;
            sg->br_off = -1;
            if (br_len > 0) {
                if (br_off < 0 && last_url && strcmp(last_url, sg->url) == 0) br_off = next_off;
                if (br_off < 0 || br_off > INT64_MAX - br_len) br_off = 0;
                sg->br_off = br_off;
                next_off = br_off + br_len;
            }
            m->duration += dur;
            last_url = sg->url;
            br_len = br_off = -1;
            dur = 0;
        }
    }
    free(buf);
    return m->nsegs;
}

static void media_free(hls_media *m)
{
    for (int i = 0; i < m->nsegs; i++) free(m->segs[i].url);
    for (int i = 0; i < m->nkeys; i++) free(m->keys[i].uri);
    free(m->segs);
    free(m->keys);
    free(m->map_url);
    memset(m, 0, sizeof *m);
}

/* ------------------------------------------------------------------ */
/* parallel segment download                                           */
/* ------------------------------------------------------------------ */
typedef struct {
    http_session      *s;
    const ul_config   *cfg;
    const ul_headers  *hd;
    hls_media         *m;
    char              *dir;
    HANDLE             dirlock;     /* "<dir>\.lock": this download owns the cache */
    BCRYPT_ALG_HANDLE  aes;
    CRITICAL_SECTION   lock;
    volatile LONG      next;
    volatile LONG      done;
    volatile LONG      failed;
    volatile LONG64    bytes;
    volatile char     *segdone;     /* per segment: on disk */
    double             t0;          /* for the speed limit */
    char               err[256];
} hls_ctx;

static char *seg_path(const char *dir, int i)
{
    return i < 0 ? xasprintf("%s\\init.seg", dir) : xasprintf("%s\\%06d.seg", dir, i);
}

static int get_key(hls_ctx *c, int k, uint8_t out[16], char *err, size_t errlen)
{
    hls_key *key = &c->m->keys[k];
    EnterCriticalSection(&c->lock);
    int have = key->loaded;
    if (have) memcpy(out, key->data, 16);
    LeaveCriticalSection(&c->lock);
    if (have) return 1;
    size_t len = 0;
    char *b = http_get_body(c->s, key->uri, c->hd, -1, -1, 0, &len, NULL, c->cfg->retries, err, errlen, NULL);
    if (!b) return 0;
    if (len != 16) { snprintf(err, errlen, "bad AES key (%u bytes)", (unsigned)len); free(b); return 0; }
    EnterCriticalSection(&c->lock);
    memcpy(key->data, b, 16);
    key->loaded = 1;
    LeaveCriticalSection(&c->lock);
    memcpy(out, b, 16);
    free(b);
    return 1;
}

/* AES-128-CBC decrypt in place, then strip PKCS#7 padding. */
static int aes_decrypt(hls_ctx *c, char *data, size_t *len, const uint8_t key[16], const uint8_t iv_in[16])
{
    if (*len == 0 || *len % 16) return 0;
    BCRYPT_KEY_HANDLE hk = NULL;
    uint8_t iv[16];
    ULONG outlen = 0;
    if (!NT_SUCCESS(BCryptGenerateSymmetricKey(c->aes, &hk, NULL, 0, (PUCHAR)key, 16, 0))) return 0;
    memcpy(iv, iv_in, 16);
    NTSTATUS st = BCryptDecrypt(hk, (PUCHAR)data, (ULONG)*len, NULL, iv, 16,
                                (PUCHAR)data, (ULONG)*len, &outlen, 0);
    BCryptDestroyKey(hk);
    if (!NT_SUCCESS(st) || outlen != (ULONG)*len) return 0;
    /* AES-128 segments always end in PKCS#7 padding. Bad padding means a
       wrong key or damaged data: fail, never save garbage as "done". */
    unsigned pad = (unsigned char)data[*len - 1];
    if (pad < 1 || pad > 16) return 0;
    for (unsigned i = 0; i < pad; i++)
        if ((unsigned char)data[*len - 1 - i] != pad) return 0;
    *len -= pad;
    return 1;
}

/* Download (and decrypt) one segment to its cache file. */
static int fetch_segment(hls_ctx *c, int i, char *err, size_t errlen)
{
    int is_map = i < 0;
    const char *url = is_map ? c->m->map_url : c->m->segs[i].url;
    int64_t off = is_map ? c->m->map_off : c->m->segs[i].br_off;
    int64_t len = is_map ? c->m->map_len : c->m->segs[i].br_len;
    int key = is_map ? c->m->map_key : c->m->segs[i].key;
    int64_t from = len >= 0 ? off : -1, to = len >= 0 ? off + len - 1 : -1;

    size_t n = 0;
    char *data = http_get_body(c->s, url, c->hd, from, to, 0, &n, NULL, c->cfg->retries, err, errlen,
                               &c->bytes);
    if (!data) return 0;
    speed_limit_take(http_task(c->s), (int64_t)n);   /* the window's limiter, shared by all (#2) */
    if (key >= 0) {
        uint8_t k[16], iv[16];
        if (!get_key(c, key, k, err, errlen)) { free(data); return 0; }
        if (c->m->keys[key].has_iv) {
            memcpy(iv, c->m->keys[key].iv, 16);
        } else {
            uint64_t sq = is_map ? 0 : (uint64_t)c->m->segs[i].seq;
            memset(iv, 0, 16);
            for (int b = 0; b < 8; b++) iv[15 - b] = (uint8_t)(sq >> (8 * b));
        }
        if (!aes_decrypt(c, data, &n, k, iv)) {
            snprintf(err, errlen, "AES-128 decryption failed (wrong key or damaged data)");
            free(data);
            return 0;
        }
    }
    char *path = seg_path(c->dir, i);
    char *tmp = xasprintf("%s.tmp", path);
    FILE *f = fopen_u8(tmp, "wb");
    int ok = f && fwrite(data, 1, n, f) == n;
    if (f && fclose(f) != 0) ok = 0;
    if (ok) ok = file_rename(tmp, path) == 0;
    if (!ok) { snprintf(err, errlen, "cannot write %s", tmp); file_delete(tmp); }
    free(tmp);
    free(path);
    free(data);
    return ok;
}

static int pick_variant(const hls_variant *v, int n, int max_height);

static unsigned __stdcall hls_worker(void *arg)
{
    hls_ctx *c = arg;
    ul_task *t = http_task(c->s);
    char err[256];
    while (!task_aborted(t) && !c->failed) {
        LONG i = InterlockedIncrement(&c->next) - 1;
        if (i >= c->m->nsegs) break;
        if (c->segdone[i]) continue;   /* already on disk (resume) */
        if (fetch_segment(c, i, err, sizeof err)) {
            c->segdone[i] = 1;
            InterlockedIncrement(&c->done);
            /* speed limit: shared by all segment workers */
            int64_t lim = c->cfg->speed_limit;
            if (lim > 0) {
                /* wait in small steps (no DWORD overflow on a big lag) and
                   stop waiting when another worker failed or on pause */
                double due = c->t0 + (double)c->bytes / (double)lim;
                while (!task_aborted(t) && !c->failed && now_sec() < due) Sleep(100);
            }
        } else if (!task_aborted(t)) {
            EnterCriticalSection(&c->lock);
            snprintf(c->err, sizeof c->err, "segment %ld: %s", (long)i + 1, err);
            LeaveCriticalSection(&c->lock);
            InterlockedExchange(&c->failed, 1);
        }
    }
    return 0;
}

/* Two downloads that end up with the same name (a CDN that calls every
   video "1080p.av1.mp4.m3u8", or one video started twice) must never share
   a segment folder: the second used to take the first one's segments for
   "a different stream" and delete them, and the first then failed at the
   join. The owner keeps "<dir>\.lock" open without sharing (deleted when
   closed); others see ERROR_SHARING_VIOLATION. */
static HANDLE cache_lock(const char *dir)
{
    char *p = xasprintf("%s\\.lock", dir);
    wchar_t *w = wpath(p);
    HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    free(w);
    free(p);
    return h;
}

/* is a download running in the cache folder of this base name right now? */
static int hls_cache_busy(const char *base)
{
    static const char *sfx[] = { ".ulhls", ".video.ulhls", ".audio.ulhls" };
    for (size_t i = 0; i < sizeof sfx / sizeof *sfx; i++) {
        char *d = xasprintf("%s%s", base, sfx[i]);
        int busy = 0;
        if (dir_exists(d)) {
            HANDLE h = cache_lock(d);
            if (h == INVALID_HANDLE_VALUE) busy = GetLastError() == ERROR_SHARING_VIOLATION;
            else CloseHandle(h);
        }
        free(d);
        if (busy) return 1;
    }
    return 0;
}

static void hls_ctx_free(hls_ctx *c)
{
    if (c->dirlock && c->dirlock != INVALID_HANDLE_VALUE) CloseHandle(c->dirlock);
    if (c->aes) BCryptCloseAlgorithmProvider(c->aes, 0);
    DeleteCriticalSection(&c->lock);
    media_free(c->m);
    free(c->m);
    free((void *)c->segdone);
    free(c->dir);
    free(c);
}

/* Identity of a downloaded stream. It is kept with the finished file
   (NTFS stream, file_meta_write) so a later run knows that file is this
   stream, complete - e.g. the video part when only the audio was paused. */
static char *stream_id(const char *url, int nsegs)
{
    size_t q = strcspn(url, "?#");
    return xasprintf("hls:%.*s#%d", (int)q, url, nsegs);
}

static int is_stream_file(const char *path, const char *id)
{
    return file_exists(path) && file_meta_matches(path, id, NULL) == 1;
}

/* Download one media playlist. The segments are joined into "<base>.ts"
   (or .mp4 / .aac, by content). The result is then saved as `want` when
   given (a caller that needs exactly that name), else as "<base>.mp4"
   when `ff` is given and the content is MPEG-TS (remuxed by ffmpeg).
   Returns 0 ok, 1 error, 2 paused; *out_path receives the file name. */
static int hls_fetch_media(http_session *s, const ul_config *cfg, const ul_headers *hd,
                           const char *url, const char *text_in, const char *base,
                           const char *label, const char *want, const char *ff, char **out_path)
{
    ul_task *t = http_task(s);
    char err[256], a[32];
    char *text = NULL, *final = NULL;
    size_t tl;
    *out_path = NULL;
    if (text_in) {
        text = xstrdup(text_in);
        final = xstrdup(url);
    } else {
        text = http_get_body(s, url, hd, -1, -1, HTTP_TEXT, &tl, &final, cfg->retries, err, sizeof err, NULL);
        if (!text) { tlog(t, LOG_ERR, "cannot load playlist: %s", err); return task_aborted(t) ? 2 : 1; }
    }
    /* a "media" playlist that is really another master list: choose again */
    for (int depth = 0; is_master(text); depth++) {
        hls_variant *v = NULL;
        char **aurl = NULL, **agrp = NULL;
        int na = 0, nv = parse_master(text, final, &v, &aurl, &agrp, &na);
        char *next = (nv > 0 && depth < 3) ? xstrdup(v[pick_variant(v, nv, cfg->max_height)].url) : NULL;
        for (int i = 0; i < nv; i++) { free(v[i].url); free(v[i].audio_group); }
        for (int i = 0; i < na; i++) { free(aurl[i]); free(agrp[i]); }
        free(v); free(aurl); free(agrp);
        free(text);
        free(final);
        if (!next) { tlog(t, LOG_ERR, "the playlist does not lead to any stream"); return 1; }
        text = http_get_body(s, next, hd, -1, -1, HTTP_TEXT, &tl, &final, cfg->retries, err, sizeof err, NULL);
        free(next);
        if (!text) { tlog(t, LOG_ERR, "cannot load playlist: %s", err); return task_aborted(t) ? 2 : 1; }
    }
    hls_media *m = xcalloc(1, sizeof *m);
    int n = parse_media(text, final, m);
    free(text);
    free(final);
    if (n == 0) { tlog(t, LOG_ERR, "the playlist has no segments"); media_free(m); free(m); return 1; }
    /* only a key that is really used matters (an unused SAMPLE-AES entry
       next to a clear stream must not block it) */
    for (int i = 0; i <= m->nsegs; i++) {
        int k = i < m->nsegs ? m->segs[i].key : (m->map_url ? m->map_key : -1);
        if (k >= 0 && m->keys[k].method == KEY_UNSUPPORTED) {
            tlog(t, LOG_ERR, "this stream is DRM-protected (SAMPLE-AES / FairPlay / Widevine) and cannot be downloaded");
            media_free(m);
            free(m);
            return 1;
        }
    }
    if (!m->endlist) tlog(t, LOG_WARN, "live stream - downloading the segments that are available now");

    const char *ext = ".ts";
    const char *fp = m->segs[0].url;
    size_t q = strcspn(fp, "?#");
    char *first = xstrndup(fp, q);
    if (m->map_url || str_iends(first, ".m4s") || str_iends(first, ".mp4")) ext = ".mp4";
    else if (str_iends(first, ".aac")) ext = ".aac";
    free(first);

    char *out = xasprintf("%s%s", base, ext);
    char *dest = want ? xstrdup(want) : (ff && !strcmp(ext, ".ts")) ? xasprintf("%s.mp4", base) : xstrdup(out);
    char *id = stream_id(url, m->nsegs);
    hls_ctx *c = xcalloc(1, sizeof *c);
    c->s = s; c->cfg = cfg; c->hd = hd; c->m = m;
    c->dir = xasprintf("%s.ulhls", base);
    c->segdone = xcalloc((size_t)m->nsegs, 1);
    InitializeCriticalSection(&c->lock);
    task_set_file(t, path_basename(dest), dest);
    int rc = 1;
    double t0 = now_sec();

    /* finished by an earlier run? (then only what is left is done) */
    int joined = 0;
    if (!dir_exists(c->dir)) {
        if (is_stream_file(dest, id)) {
            tlog(t, LOG_DIM, "  %salready downloaded", label ? label : "");
            t->done = t->total = file_size(dest);
            *out_path = dest;
            dest = NULL;
            rc = 0;
            goto done;
        }
        joined = _stricmp(dest, out) != 0 && is_stream_file(out, id);   /* paused while converting */
    }

    if (!joined) {
        if (dir_create(c->dir) != 0) {
            tlog(t, LOG_ERR, "cannot create folder %s", c->dir);
            goto done;
        }
        c->dirlock = cache_lock(c->dir);
        if (c->dirlock == INVALID_HANDLE_VALUE) {
            DWORD e = GetLastError();
            if (e == ERROR_SHARING_VIOLATION)
                tlog(t, LOG_ERR, "another download is already saving this stream into %s - "
                                 "wait for it to finish (or give this one another name)", c->dir);
            else
                tlog(t, LOG_ERR, "cannot use the folder %s: %s", c->dir, win_errstr(e, err, sizeof err));
            goto done;
        }
        if (m->nkeys > 0) {
            if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&c->aes, BCRYPT_AES_ALGORITHM, NULL, 0)) ||
                !NT_SUCCESS(BCryptSetProperty(c->aes, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC,
                                              sizeof(BCRYPT_CHAIN_MODE_CBC), 0))) {
                tlog(t, LOG_ERR, "AES is not available on this system");
                goto done;
            }
        }

        /* The cache must belong to this exact stream (same playlist, same
           number of segments); otherwise segments of different qualities
           would be joined together. Segments without that record are of
           unknown origin and are not used either. */
        {
            size_t uq = strcspn(url, "?#");
            char *sid = xasprintf("%.*s\n%d\n", (int)uq, url, m->nsegs);
            char *mf = xasprintf("%s\\stream.txt", c->dir);
            size_t ol = 0;
            char *old = read_file(mf, &ol);
            if (!old || strcmp(old, sid) != 0) {
                if (old) tlog(t, LOG_WARN, "the saved segments belong to a different stream - starting this one fresh");
                const char *nl = old ? strchr(old, '\n') : NULL;
                long old_n = nl ? strtol(nl + 1, NULL, 10) : 0;
                if (old_n < 0 || old_n > 10000000) old_n = 0;
                long n_del = old_n > m->nsegs ? old_n : m->nsegs;
                for (long i = -1; i < n_del; i++) {
                    char *p = seg_path(c->dir, (int)i);
                    file_delete(p);
                    free(p);
                }
                FILE *f = fopen_u8(mf, "wb");
                if (f) { fputs(sid, f); fclose(f); }
            }
            free(old);
            free(mf);
            free(sid);
        }

        int resumed = 0;
        int64_t resumed_bytes = 0;
        for (int i = 0; i < m->nsegs; i++) {
            char *p = seg_path(c->dir, i);
            int64_t sz = file_size(p);
            if (sz >= 0) { c->segdone[i] = 1; c->done++; resumed_bytes += sz; resumed++; }
            free(p);
        }

        long mins = (long)(m->duration / 60), secs = (long)m->duration % 60;
        int conns = cfg->connections < m->nsegs ? cfg->connections : m->nsegs;
        if (conns > UL_MAX_CONN) conns = UL_MAX_CONN;
        if (conns < 1) conns = 1;
        tlog(t, LOG_DIM, "  %s%d segments  |  %ld:%02ld  |  %d connections%s", label ? label : "",
             m->nsegs, mins, secs, conns, m->nkeys ? "  |  AES-128" : "");
        if (resumed) tlog(t, LOG_DIM, "  resuming: %d segments already downloaded", resumed);

        if (m->map_url) {
            char *mp = seg_path(c->dir, -1);
            int have = file_exists(mp);
            free(mp);
            if (!have && !fetch_segment(c, -1, err, sizeof err)) {
                tlog(t, LOG_ERR, "init segment: %s", err);
                rc = task_aborted(t) ? 2 : 1;
                goto done;
            }
        }

        HANDLE th[UL_MAX_CONN];
        int nth = 0;
        c->t0 = t0;
        for (int i = 0; i < conns; i++) {
            th[nth] = (HANDLE)_beginthreadex(NULL, 0, hls_worker, c, 0, NULL);
            if (th[nth]) nth++;
        }
        double last_t = t0, speed = 0;
        int64_t last_b = 0;
        int all_done = nth == 0;
        while (!all_done) {
            DWORD wr = WaitForMultipleObjects((DWORD)nth, th, TRUE, 500);
            all_done = wr != WAIT_TIMEOUT;
            double tm = now_sec();
            int64_t bytes = c->bytes;
            if (tm - last_t >= 1.0) {
                double inst = (double)(bytes - last_b) / (tm - last_t);
                speed = speed > 0 ? speed * 0.6 + inst * 0.4 : inst;
                last_t = tm;
                last_b = bytes;
            }
            int done = c->done;
            int64_t got = resumed_bytes + bytes;
            int64_t est = done > 0 ? (int64_t)((double)got / done * m->nsegs) : -1;
            unsigned char map[UL_SEGVIEW];
            for (int k = 0; k < UL_SEGVIEW; k++) {
                int a0 = (int)((int64_t)m->nsegs * k / UL_SEGVIEW);
                int b0 = (int)((int64_t)m->nsegs * (k + 1) / UL_SEGVIEW);
                if (b0 <= a0) b0 = a0 + 1;
                if (b0 > m->nsegs) b0 = m->nsegs;
                int cnt = 0;
                for (int i = a0; i < b0; i++) cnt += c->segdone[i] ? 1 : 0;
                map[k] = b0 > a0 ? (unsigned char)(cnt * 255 / (b0 - a0)) : 0;
            }
            EnterCriticalSection(&t->lock);
            memcpy(t->segmap, map, sizeof map);
            snprintf(t->stage, sizeof t->stage, "seg %d/%d", done, m->nsegs);
            LeaveCriticalSection(&t->lock);
            t->done = got;
            t->total = est;
            t->speed = (LONG64)speed;
            t->eta = (speed > 1 && est > got) ? (LONG64)((double)(est - got) / speed) : -1;
            t->conns = nth;
            task_progress(t);
            if ((task_aborted(t) || c->failed) && !all_done) {
                /* close connections the workers may be blocked on, then wait
                   until every one has exited: they use c, the session and cfg */
                while (WaitForMultipleObjects((DWORD)nth, th, TRUE, 200) == WAIT_TIMEOUT)
                    http_session_cancel(s);
                all_done = 1;
            }
        }
        con_progress_end();
        task_set_stage(t, "%s", "");
        for (int i = 0; i < nth; i++) CloseHandle(th[i]);

        if (task_aborted(t)) {
            tlog(t, LOG_WARN, "paused - start it again to resume.");
            rc = 2;
            goto done;
        }
        if (c->failed || c->done < m->nsegs) {
            tlog(t, LOG_ERR, "%s", c->err[0] ? c->err : "some segments could not be downloaded");
            tlog(t, LOG_INFO, "  Downloaded segments are kept - start it again to resume.");
            goto done;
        }

        /* join: init + segments -> out */
        task_set_stage(t, "%s", "joining");
        task_progress(t);
        /* Every failure says what it was: "disk full?" for everything sent
           people looking at free space when a segment had gone missing. */
        char *tmp = xasprintf("%s.ulpart", out);
        char why[400] = "";
        FILE *fo = fopen_u8(tmp, "wb");
        if (!fo) snprintf(why, sizeof why, "cannot create %s: %s", tmp, strerror(errno));
        char *cbuf = xmalloc(1 << 20);
        for (int i = m->map_url ? -1 : 0; !why[0] && i < m->nsegs; i++) {
            char *p = seg_path(c->dir, i);
            FILE *fi = fopen_u8(p, "rb");
            if (!fi) {
                /* gone from the cache: the next start fetches it again */
                if (i < 0) snprintf(why, sizeof why, "the init segment is missing from %s", c->dir);
                else snprintf(why, sizeof why, "segment %d of %d is missing from %s", i + 1, m->nsegs, c->dir);
                c->failed = 1;
                free(p);
                break;
            }
            size_t r;
            while (!why[0] && (r = fread(cbuf, 1, 1 << 20, fi)) > 0)
                if (fwrite(cbuf, 1, r, fo) != r)
                    snprintf(why, sizeof why, errno == ENOSPC ? "the disk is full (writing %s)" : "cannot write %s: %s",
                             tmp, strerror(errno));
            if (!why[0] && ferror(fi)) snprintf(why, sizeof why, "cannot read %s", p);
            fclose(fi);
            free(p);
        }
        free(cbuf);
        if (fo && fclose(fo) != 0 && !why[0])
            snprintf(why, sizeof why, errno == ENOSPC ? "the disk is full (writing %s)" : "cannot write %s: %s",
                     tmp, strerror(errno));
        task_set_stage(t, "%s", "");
        if (!why[0] && file_rename(tmp, out) != 0) {
            char e[200];
            snprintf(why, sizeof why, "cannot save %s: %s (is it open in another program?)", out,
                     win_errstr(GetLastError(), e, sizeof e));
        }
        if (why[0]) {
            tlog(t, LOG_ERR, "%s", why);
            if (c->failed) tlog(t, LOG_INFO, "  Start it again: the missing part is downloaded again.");
            file_delete(tmp);
            free(tmp);
            goto done;
        }
        free(tmp);
        file_meta_write(out, id, NULL);
        /* the cache is finished with: every file in it, then the folder
           (a leftover stream.txt kept the folder - and the next download
           of the same name took it for a paused one and overwrote) */
        for (int i = m->map_url ? -1 : 0; i < m->nsegs; i++) {
            char *p = seg_path(c->dir, i);
            file_delete(p);
            free(p);
        }
        char *mf = xasprintf("%s\\stream.txt", c->dir);
        file_delete(mf);
        free(mf);
        CloseHandle(c->dirlock);                       /* deletes .lock: the folder is empty now */
        c->dirlock = NULL;
        dir_remove(c->dir);
    }

    /* the joined file -> the name the caller wants */
    if (_stricmp(dest, out) != 0) {
        if (ff && str_iends(out, ".ts") && str_iends(dest, ".mp4")) {
            tlog(t, LOG_DIM, "  converting to .mp4 with ffmpeg...");
            task_set_stage(t, "%s", "converting");
            task_progress(t);
            int fr = ffmpeg_remux(ff, out, dest, t);
            task_set_stage(t, "%s", "");
            if (fr == 0) {
                file_delete(out);
                file_meta_write(dest, id, NULL);
            } else if (task_aborted(t)) {
                tlog(t, LOG_WARN, "paused - start it again to finish the conversion.");
                rc = 2;
                goto done;
            } else {
                tlog(t, LOG_WARN, "ffmpeg conversion failed; kept %s", out);
                free(dest);
                dest = xstrdup(out);
            }
        } else if (file_rename(out, dest) != 0) {
            tlog(t, LOG_ERR, "could not rename %s to %s: %s", out, dest, win_errstr(GetLastError(), err, sizeof err));
            goto done;
        }
    }
    int64_t sz = file_size(dest);
    t->done = t->total = sz;
    task_set_file(t, path_basename(dest), dest);
    tlog(t, LOG_OK, "  Done: %s in %.1fs", fmt_size(sz, a, sizeof a), now_sec() - t0);
    *out_path = dest;
    dest = NULL;
    rc = 0;

done:
    free(dest);
    free(out);
    free(id);
    hls_ctx_free(c);
    return rc;
}

/* ------------------------------------------------------------------ */
/* The best video within the height cap. Variants without a resolution
   (usually audio-only) are chosen only when no variant gives one. */
static int pick_variant(const hls_variant *v, int n, int max_height)
{
    int best = -1, any_res = 0;
    for (int i = 0; i < n; i++) if (v[i].height > 0) any_res = 1;
    for (int i = 0; i < n; i++) {
        if (any_res && v[i].height <= 0) continue;
        if (max_height > 0 && v[i].height > max_height) continue;
        if (best < 0 || v[i].height > v[best].height ||
            (v[i].height == v[best].height && v[i].bw > v[best].bw))
            best = i;
    }
    if (best < 0) {  /* every video is above the cap: take the smallest */
        for (int i = 0; i < n; i++) {
            if (any_res && v[i].height <= 0) continue;
            if (best < 0 || v[i].height < v[best].height ||
                (v[i].height == v[best].height && v[i].bw > v[best].bw))
                best = i;
        }
    }
    return best < 0 ? 0 : best;
}

static char *default_base(const ul_config *cfg, const char *url)
{
    char *fn = url_filename(url);
    char *stem = fn ? path_strip_ext(fn) : xstrdup("video");
    /* "clip.mp4.m3u8" -> "clip" (it became "clip.mp4.mp4") */
    static const char *media_ext[] = { ".mp4", ".ts", ".m4s", ".m4v", ".aac", ".m4a", ".mkv", ".webm", ".mov" };
    for (size_t i = 0; i < sizeof media_ext / sizeof *media_ext; i++) {
        size_t sl = strlen(stem), el = strlen(media_ext[i]);
        if (sl > el && str_iends(stem, media_ext[i])) { stem[sl - el] = 0; break; }
    }
    static const char *generic[] = { "index", "master", "playlist", "prog_index", "chunklist",
                                     "manifest", "main", "video", "stream", "media", "mono" };
    int is_generic = 0;
    for (size_t i = 0; i < sizeof generic / sizeof *generic; i++)
        if (str_istarts(stem, generic[i])) is_generic = 1;
    if (is_generic) {
        /* use the parent folder name: .../my-movie/index.m3u8 -> my-movie */
        size_t q = strcspn(url, "?#");
        char *u = xstrndup(url, q);
        char *slash = strrchr(u, '/');
        if (slash) {
            *slash = 0;
            char *parent = url_filename(u);
            if (parent && !strchr(parent, ':') && strlen(parent) > 2) {
                free(stem);
                stem = parent;
                parent = NULL;
            }
            free(parent);
        }
        free(u);
    }
    char *clean = sanitize_filename(stem);
    char *dir = category_folder(cfg, "stream.mp4");   /* a stream is a video: Video\ (#1) */
    char *base = path_join(dir, clean);
    free(dir);
    free(clean);
    free(stem);
    free(fn);
    return base;
}

/* a download of this name is paused (its segment cache is still there) */
static int hls_cache_exists(const char *base)
{
    static const char *sfx[] = { ".ulhls", ".video.ulhls", ".audio.ulhls" };
    for (size_t i = 0; i < sizeof sfx / sizeof *sfx; i++) {
        char *p = xasprintf("%s%s", base, sfx[i]);
        int e = dir_exists(p);
        free(p);
        if (e) return 1;
    }
    return 0;
}

/* a finished (or finishing) file already has this name */
static int hls_name_used(const char *base)
{
    static const char *sfx[] = { ".mp4", ".ts", ".aac", ".mp4.ulpart", ".ts.ulpart", ".aac.ulpart" };
    for (size_t i = 0; i < sizeof sfx / sizeof *sfx; i++) {
        char *p = xasprintf("%s%s", base, sfx[i]);
        int e = file_exists(p);
        free(p);
        if (e) return 1;
    }
    return 0;
}

int hls_run(http_session *s, const ul_config *cfg, const char *url, const ul_headers *hd, const char *hint)
{
    ul_task *t = http_task(s);
    char err[256];
    char *final = NULL;
    size_t tl;
    char *text = http_get_body(s, url, hd, -1, -1, HTTP_TEXT, &tl, &final, cfg->retries, err, sizeof err, NULL);
    if (!text) { tlog(t, LOG_ERR, "cannot load playlist: %s", err); return task_aborted(t) ? 2 : 1; }
    if (!strstr(text, "#EXTM3U")) {
        tlog(t, LOG_ERR, "not a valid HLS playlist");
        free(text); free(final);
        return 1;
    }

    char *base;
    if (hint) {
        base = path_strip_ext(hint);
    } else {
        base = default_base(cfg, url);
        /* A free name: never one another download is saving into right now
           (its segment folder is locked), and never an earlier finished
           file - "name (N)" instead. A paused "name (N)" has no file yet
           and an unlocked folder, so it is picked up again and resumes. */
        char *b = xstrdup(base);
        for (int i = 1; i < 100000; i++) {
            int busy = hls_cache_busy(b);
            int used = !cfg->overwrite && !hls_cache_exists(b) && hls_name_used(b);
            if (!busy && !used) break;
            free(b);
            b = xasprintf("%s (%d)", base, i);
        }
        free(base);
        base = b;
    }
    tlog(t, LOG_HEAD, "%s", path_basename(base));

    char *media_url = NULL, *audio_url = NULL, *media_text = NULL;
    if (is_master(text)) {
        hls_variant *v = NULL;
        char **aurl = NULL, **agrp = NULL;
        int na = 0;
        int nv = parse_master(text, final, &v, &aurl, &agrp, &na);
        if (nv == 0) {
            tlog(t, LOG_ERR, "the master playlist lists no streams");
        } else {
            int b = pick_variant(v, nv, cfg->max_height);
            media_url = xstrdup(v[b].url);
            if (v[b].height) tlog(t, LOG_DIM, "  quality: %dx%d, %.1f Mbps (best of %d)", v[b].width,
                                    v[b].height, (double)v[b].bw / 1e6, nv);
            if (v[b].audio_group) {
                /* the DEFAULT rendition of the group, else its first one */
                int pick = -1;
                for (int i = 0; i < na; i++) {
                    const char *g = agrp[i][0] == '*' ? agrp[i] + 1 : agrp[i];
                    if (strcmp(g, v[b].audio_group) != 0) continue;
                    if (pick < 0 || (agrp[i][0] == '*' && agrp[pick][0] != '*')) pick = i;
                }
                /* no URI: the audio is already inside the video stream */
                if (pick >= 0 && aurl[pick]) audio_url = xstrdup(aurl[pick]);
            }
        }
        for (int i = 0; i < nv; i++) { free(v[i].url); free(v[i].audio_group); }
        for (int i = 0; i < na; i++) { free(aurl[i]); free(agrp[i]); }
        free(v); free(aurl); free(agrp);
    } else {
        media_url = xstrdup(final);
        media_text = xstrdup(text);
    }
    free(text);
    free(final);
    if (!media_url) { free(base); return 1; }

    char *ffmpeg = find_tool("ffmpeg.exe");
    int rc;
    if (audio_url) {
        char *target = hint ? xstrdup(hint) : xasprintf("%s.mp4", base);
        char *vb = xasprintf("%s.video", base), *ab = xasprintf("%s.audio", base);
        char *vout = NULL, *aout = NULL;
        rc = hls_fetch_media(s, cfg, hd, media_url, NULL, vb, "video: ", NULL, NULL, &vout);
        if (rc == 0) rc = hls_fetch_media(s, cfg, hd, audio_url, NULL, ab, "audio: ", NULL, NULL, &aout);
        if (rc == 0) {
            if (ffmpeg) {
                tlog(t, LOG_DIM, "  merging audio + video with ffmpeg...");
                task_set_file(t, path_basename(target), target);
                task_set_stage(t, "%s", "merging");
                task_progress(t);
                if (ffmpeg_merge(ffmpeg, vout, aout, target, t) == 0) {
                    file_delete(vout);
                    file_delete(aout);
                    t->done = t->total = file_size(target);
                    tlog(t, LOG_DIM, "  Saved to %s", target);
                } else if (task_aborted(t)) {
                    tlog(t, LOG_WARN, "paused - start it again to finish merging.");
                    rc = 2;
                } else {
                    tlog(t, LOG_WARN, "ffmpeg merge failed; kept %s and %s", vout, aout);
                    task_set_file(t, path_basename(vout), vout);
                    if (hint) rc = 1;                /* the caller needs exactly `hint` */
                }
                task_set_stage(t, "%s", "");
            } else {
                tlog(t, LOG_WARN, "audio and video are separate files. Install ffmpeg to merge them automatically.");
                tlog(t, LOG_DIM, "  Saved %s", vout);
                tlog(t, LOG_DIM, "  Saved %s", aout);
                task_set_file(t, path_basename(vout), vout);
            }
        }
        free(vb); free(ab); free(vout); free(aout); free(target);
    } else {
        char *out = NULL;
        rc = hls_fetch_media(s, cfg, hd, media_url, media_text, base, "", hint, ffmpeg, &out);
        if (rc == 0) tlog(t, LOG_DIM, "  Saved to %s", out);
        free(out);
    }
    free(ffmpeg);
    free(media_url);
    free(audio_url);
    free(media_text);
    free(base);
    return rc;
}
