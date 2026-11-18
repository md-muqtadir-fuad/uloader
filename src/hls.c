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
            if (type && uri && grp && _stricmp(type, "AUDIO") == 0) {
                aurl = xrealloc(aurl, sizeof(char *) * (size_t)(na + 1));
                agrp = xrealloc(agrp, sizeof(char *) * (size_t)(na + 1));
                aurl[na] = url_resolve(base, uri);
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
    if (!NT_SUCCESS(st)) return 0;
    unsigned pad = (unsigned char)data[*len - 1];
    if (pad >= 1 && pad <= 16) {
        int okpad = 1;
        for (unsigned i = 0; i < pad; i++)
            if ((unsigned char)data[*len - 1 - i] != pad) { okpad = 0; break; }
        if (okpad) *len -= pad;
    }
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
            snprintf(err, errlen, "AES-128 decryption failed");
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
                double due = c->t0 + (double)c->bytes / (double)lim, now = now_sec();
                if (due > now) task_sleep(t, (DWORD)((due - now) * 1000.0));
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

static void hls_ctx_free(hls_ctx *c)
{
    if (c->aes) BCryptCloseAlgorithmProvider(c->aes, 0);
    DeleteCriticalSection(&c->lock);
    media_free(c->m);
    free(c->m);
    free((void *)c->segdone);
    free(c->dir);
    free(c);
}

/* Download one media playlist into "<base>.ts" (or .mp4/.aac).
   Returns 0 ok, 1 error, 2 paused; *out_path receives the file name. */
static int hls_fetch_media(http_session *s, const ul_config *cfg, const ul_headers *hd,
                           const char *url, const char *text_in, const char *base,
                           const char *label, char **out_path)
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
    for (int i = 0; i < m->nkeys; i++) {
        if (m->keys[i].method == KEY_UNSUPPORTED) {
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
    hls_ctx *c = xcalloc(1, sizeof *c);
    c->s = s; c->cfg = cfg; c->hd = hd; c->m = m;
    c->dir = xasprintf("%s.ulhls", base);
    c->segdone = xcalloc((size_t)m->nsegs, 1);
    InitializeCriticalSection(&c->lock);
    task_set_file(t, path_basename(out), out);

    if (dir_create(c->dir) != 0) {
        tlog(t, LOG_ERR, "cannot create folder %s", c->dir);
        free(out);
        hls_ctx_free(c);
        return 1;
    }
    if (m->nkeys > 0) {
        if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&c->aes, BCRYPT_AES_ALGORITHM, NULL, 0)) ||
            !NT_SUCCESS(BCryptSetProperty(c->aes, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC,
                                          sizeof(BCRYPT_CHAIN_MODE_CBC), 0))) {
            tlog(t, LOG_ERR, "AES is not available on this system");
            free(out);
            hls_ctx_free(c);
            return 1;
        }
    }

    /* The cache must belong to this exact stream (same playlist, same
       number of segments); otherwise segments of different qualities
       would be joined together. */
    {
        size_t q = strcspn(url, "?#");
        char *id = xasprintf("%.*s\n%d\n", (int)q, url, m->nsegs);
        char *mf = xasprintf("%s\\stream.txt", c->dir);
        size_t ol = 0;
        char *old = read_file(mf, &ol);
        if (old && strcmp(old, id) != 0) {
            tlog(t, LOG_WARN, "the saved segments belong to a different stream - starting this one fresh");
            const char *nl = strchr(old, '\n');
            long old_n = nl ? strtol(nl + 1, NULL, 10) : 0;
            if (old_n < 0 || old_n > 10000000) old_n = 0;
            long n_del = old_n > m->nsegs ? old_n : m->nsegs;
            for (long i = -1; i < n_del; i++) {
                char *p = seg_path(c->dir, (int)i);
                file_delete(p);
                free(p);
            }
        }
        if (!old || strcmp(old, id) != 0) {
            FILE *f = fopen_u8(mf, "wb");
            if (f) { fputs(id, f); fclose(f); }
        }
        free(old);
        free(mf);
        free(id);
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
            int rc0 = task_aborted(t) ? 2 : 1;
            free(out);
            hls_ctx_free(c);
            return rc0;
        }
    }

    HANDLE th[UL_MAX_CONN];
    int nth = 0;
    double t0 = now_sec();
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
    double elapsed = now_sec() - t0;

    int rc = 1;
    if (task_aborted(t)) {
        tlog(t, LOG_WARN, "paused - start it again to resume.");
        rc = 2;
    } else if (c->failed || c->done < m->nsegs) {
        tlog(t, LOG_ERR, "%s", c->err[0] ? c->err : "some segments could not be downloaded");
        tlog(t, LOG_INFO, "  Downloaded segments are kept - start it again to resume.");
    } else {
        /* join: init + segments -> output */
        task_set_stage(t, "%s", "joining");
        task_progress(t);
        char *tmp = xasprintf("%s.ulpart", out);
        FILE *fo = fopen_u8(tmp, "wb");
        int ok = fo != NULL;
        char *cbuf = xmalloc(1 << 20);
        for (int i = m->map_url ? -1 : 0; ok && i < m->nsegs; i++) {
            char *p = seg_path(c->dir, i);
            FILE *fi = fopen_u8(p, "rb");
            if (!fi) { ok = 0; free(p); break; }
            size_t r;
            while ((r = fread(cbuf, 1, 1 << 20, fi)) > 0)
                if (fwrite(cbuf, 1, r, fo) != r) { ok = 0; break; }
            if (ferror(fi)) ok = 0;
            fclose(fi);
            free(p);
        }
        free(cbuf);
        if (fo && fclose(fo) != 0) ok = 0;
        if (ok && file_rename(tmp, out) == 0) {
            for (int i = m->map_url ? -1 : 0; i < m->nsegs; i++) {
                char *p = seg_path(c->dir, i);
                file_delete(p);
                free(p);
            }
            dir_remove(c->dir);
            int64_t sz = file_size(out);
            t->done = t->total = sz;
            tlog(t, LOG_OK, "  Done: %s in %.1fs", fmt_size(sz, a, sizeof a), elapsed);
            *out_path = out;
            out = NULL;
            rc = 0;
        } else {
            tlog(t, LOG_ERR, "could not write %s (disk full?)", out);
            file_delete(tmp);
        }
        task_set_stage(t, "%s", "");
        free(tmp);
    }
    free(out);
    hls_ctx_free(c);
    return rc;
}

/* ------------------------------------------------------------------ */
static int pick_variant(const hls_variant *v, int n, int max_height)
{
    int best = -1;
    for (int i = 0; i < n; i++) {
        if (max_height > 0 && v[i].height > max_height) continue;
        if (best < 0 || v[i].height > v[best].height ||
            (v[i].height == v[best].height && v[i].bw > v[best].bw))
            best = i;
    }
    if (best < 0) {  /* everything is above the cap: take the smallest */
        for (int i = 0; i < n; i++)
            if (best < 0 || v[i].height < v[best].height) best = i;
    }
    return best;
}

static char *default_base(const ul_config *cfg, const char *url)
{
    char *fn = url_filename(url);
    char *stem = fn ? path_strip_ext(fn) : xstrdup("video");
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
    char *base = path_join(cfg->out_dir, clean);
    free(clean);
    free(stem);
    free(fn);
    return base;
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
        char *mp4 = xasprintf("%s.mp4", base), *ts = xasprintf("%s.ts", base), *cache = xasprintf("%s.ulhls", base);
        if (!cfg->overwrite && !dir_exists(cache) && (file_exists(mp4) || file_exists(ts))) {
            char *u = unique_path(mp4);
            free(base);
            base = path_strip_ext(u);
            free(u);
        }
        free(mp4); free(ts); free(cache);
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
                int pick = -1;
                for (int i = 0; i < na; i++) {
                    const char *g = agrp[i][0] == '*' ? agrp[i] + 1 : agrp[i];
                    if (strcmp(g, v[b].audio_group) == 0 && (pick < 0 || agrp[i][0] == '*')) pick = i;
                }
                if (pick >= 0) audio_url = xstrdup(aurl[pick]);
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
    char *mp4 = xasprintf("%s.mp4", base);
    int rc;
    if (audio_url) {
        char *vb = xasprintf("%s.video", base), *ab = xasprintf("%s.audio", base);
        char *vout = NULL, *aout = NULL;
        rc = hls_fetch_media(s, cfg, hd, media_url, NULL, vb, "video: ", &vout);
        if (rc == 0) rc = hls_fetch_media(s, cfg, hd, audio_url, NULL, ab, "audio: ", &aout);
        if (rc == 0) {
            if (ffmpeg) {
                tlog(t, LOG_DIM, "  merging audio + video with ffmpeg...");
                if (ffmpeg_merge(ffmpeg, vout, aout, mp4, t) == 0) {
                    file_delete(vout);
                    file_delete(aout);
                    tlog(t, LOG_DIM, "  Saved to %s", mp4);
                } else {
                    tlog(t, LOG_WARN, "ffmpeg merge failed; kept %s and %s", vout, aout);
                }
            } else {
                tlog(t, LOG_WARN, "audio and video are separate files. Install ffmpeg to merge them automatically.");
                tlog(t, LOG_DIM, "  Saved %s", vout);
                tlog(t, LOG_DIM, "  Saved %s", aout);
            }
        }
        free(vb); free(ab); free(vout); free(aout);
    } else {
        char *out = NULL;
        rc = hls_fetch_media(s, cfg, hd, media_url, media_text, base, "", &out);
        if (rc == 0) {
            if (ffmpeg && str_iends(out, ".ts")) {
                tlog(t, LOG_DIM, "  converting to .mp4 with ffmpeg...");
                if (ffmpeg_remux(ffmpeg, out, mp4, t) == 0) {
                    file_delete(out);
                    tlog(t, LOG_DIM, "  Saved to %s", mp4);
                } else {
                    tlog(t, LOG_WARN, "ffmpeg conversion failed; kept %s", out);
                }
            } else {
                tlog(t, LOG_DIM, "  Saved to %s", out);
            }
        }
        free(out);
    }
    free(ffmpeg);
    free(mp4);
    free(media_url);
    free(audio_url);
    free(media_text);
    free(base);
    return rc;
}
