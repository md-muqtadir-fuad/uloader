/*
 * U-loader - BitTorrent: .torrent files and magnet links (BitTorrent v1;
 * hybrid v1/v2 torrents download through their v1 half).
 *
 *  - metadata: a .torrent file, or for magnet links the info dictionary
 *    fetched from peers (BEP 9 ut_metadata over the BEP 10 extension
 *    protocol) and checked against the info-hash
 *  - peers: HTTP(S) trackers through http_get_body (compact BEP 23),
 *    UDP trackers (BEP 15), the mainline DHT (BEP 5, read-only) and PEX
 *  - outgoing connections only: nothing listens and nothing is bound to a
 *    fixed port (UDP sockets bind themselves on the first sendto), so the
 *    Windows Firewall never asks anything
 *  - one network thread drives every peer with select(); tracker requests
 *    and DNS lookups run in short-lived helper threads that never touch
 *    the torrent itself, so pausing never waits for them
 *  - pieces are assembled in memory, verified with SHA-1 (CNG) and only
 *    then written, so the files only ever contain verified data. A corrupt
 *    piece is thrown away and the peers that sent it are banned (a peer
 *    that sent a whole bad piece at once, or whose blocks turn out to
 *    differ once the piece verifies - "smart ban").
 *  - "<out_dir>\<name>.ulbt" keeps the info-hash, the raw info dictionary
 *    (a paused magnet needs no metadata again) and the verified pieces
 */
#define FD_SETSIZE 256
#include "uloader.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>
#include <process.h>
#include <winioctl.h>
#include <limits.h>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#define BLOCK          16384                 /* request size                      */
#define MAX_PEERS      60                    /* connections (incl. connecting)    */
#define MAX_HALFOPEN   40
#define MAX_CANDS      4000                  /* known peer addresses              */
#define MAX_TRACKERS   48
#define MAX_JOBS       16                    /* helper threads in flight          */
#define MIN_PIPE       10                    /* outstanding requests per peer     */
#define MAX_PIPE       30
#define MAX_UPQ        16                    /* queued upload requests per peer   */
#define MAX_ACTIVE     2048                  /* pieces being assembled            */
#define MEM_BUDGET     ((int64_t)192 << 20)  /* for pieces being assembled        */
#define MAX_MSG        (2u << 20)
#define MAX_PLEN       ((int64_t)128 << 20)
#define MAX_TOTAL      ((int64_t)1 << 50)
#define MAX_PRE_BITS   (1 << 21)             /* "have" indexes before metadata    */
#define MD_MAX         (32 << 20)            /* largest info dictionary accepted  */
#define MD_PIECE       16384
#define TORRENT_MAX    (32 << 20)            /* largest .torrent file accepted    */
#define NO_PEERS_SECS  240.0                 /* give up after 4 min without peers */
#define MD_STALL_SECS  600.0
#define STALL_SECS     1800.0
#define CLIENT_PORT    6881                  /* reported to trackers only         */
#define EXT_UT_METADATA 1                    /* our extension message ids         */
#define EXT_UT_PEX      2

/* ================================================================== */
/* small helpers                                                       */
/* ================================================================== */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void wr64(uint8_t *p, uint64_t v) { wr32(p, (uint32_t)(v >> 32)); wr32(p + 4, (uint32_t)v); }

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = d[in[i] >> 4]; out[2 * i + 1] = d[in[i] & 15]; }
    out[2 * n] = 0;
}

static int  bit_get(const uint8_t *bf, int i) { return bf[i >> 3] >> (7 - (i & 7)) & 1; }
static void bit_set(uint8_t *bf, int i)       { bf[i >> 3] |= (uint8_t)(0x80 >> (i & 7)); }
static void bit_clr(uint8_t *bf, int i)       { bf[i >> 3] &= (uint8_t)~(0x80 >> (i & 7)); }
static int  nbytes_of(int bits)               { return (bits + 7) / 8; }

static void clear_spare_bits(uint8_t *bf, int bits)
{
    if (bits & 7) bf[bits >> 3] &= (uint8_t)(0xFF00 >> (bits & 7));
}

/* printable copy of a message from the network */
static void clean_text(char *dst, size_t cap, const uint8_t *s, size_t n)
{
    size_t k = 0;
    for (size_t i = 0; i < n && k + 1 < cap; i++) dst[k++] = (s[i] < 32 || s[i] == 127) ? ' ' : (char)s[i];
    dst[k] = 0;
}

static const char *fmt_dur(double s, char *buf, size_t n)
{
    if (s < 60) snprintf(buf, n, "%.1fs", s);
    else if (s < 3600) snprintf(buf, n, "%dm %02ds", (int)s / 60, (int)s % 60);
    else snprintf(buf, n, "%dh %02dm", (int)s / 3600, (int)s % 3600 / 60);
    return buf;
}

/* ---- SHA-1 through Windows CNG ---- */
static INIT_ONCE g_sha_once = INIT_ONCE_STATIC_INIT;
static BCRYPT_ALG_HANDLE g_sha_alg;

static BOOL CALLBACK sha_init_cb(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&g_sha_alg, BCRYPT_SHA1_ALGORITHM, NULL, 0)))
        g_sha_alg = NULL;
    return TRUE;
}

typedef struct { BCRYPT_HASH_HANDLE h; int ok; } sha1_ctx;

static int sha1_begin(sha1_ctx *c)
{
    InitOnceExecuteOnce(&g_sha_once, sha_init_cb, NULL, NULL);
    c->h = NULL;
    c->ok = g_sha_alg && BCRYPT_SUCCESS(BCryptCreateHash(g_sha_alg, &c->h, NULL, 0, NULL, 0, 0));
    return c->ok;
}

static void sha1_add(sha1_ctx *c, const void *d, size_t n)
{
    const uint8_t *p = d;
    while (c->ok && n > 0) {
        ULONG k = n > (1u << 30) ? (1u << 30) : (ULONG)n;
        if (!BCRYPT_SUCCESS(BCryptHashData(c->h, (PUCHAR)p, k, 0))) c->ok = 0;
        p += k;
        n -= k;
    }
}

static int sha1_end(sha1_ctx *c, uint8_t out[20])
{
    int ok = c->ok && BCRYPT_SUCCESS(BCryptFinishHash(c->h, out, 20, 0));
    if (c->h) BCryptDestroyHash(c->h);
    c->h = NULL;
    if (!ok) memset(out, 0, 20);
    return ok;
}

static int sha1(const void *d, size_t n, uint8_t out[20])
{
    sha1_ctx c;
    sha1_begin(&c);
    sha1_add(&c, d, n);
    return sha1_end(&c, out);
}

static void rand_bytes(void *buf, size_t n)
{
    if (BCRYPT_SUCCESS(BCryptGenRandom(NULL, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) return;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    uint64_t x = (uint64_t)q.QuadPart ^ ((uint64_t)GetCurrentThreadId() << 32) ^ (uint64_t)(uintptr_t)buf;
    uint8_t *p = buf;
    for (size_t i = 0; i < n; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        p[i] = (uint8_t)(x >> 24);
    }
}

/* ---- Winsock, once per process ---- */
static INIT_ONCE g_wsa_once = INIT_ONCE_STATIC_INIT;
static int g_wsa_ok;

static BOOL CALLBACK wsa_init_cb(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    WSADATA w;
    g_wsa_ok = WSAStartup(MAKEWORD(2, 2), &w) == 0;
    return TRUE;
}

static int net_init(void)
{
    InitOnceExecuteOnce(&g_wsa_once, wsa_init_cb, NULL, NULL);
    return g_wsa_ok;
}

/* ================================================================== */
/* bencode - strict, bounded decoder                                   */
/* ================================================================== */
enum { BE_INT = 1, BE_STR, BE_LIST, BE_DICT };

typedef struct {
    int64_t  ival;
    uint32_t off, end;      /* raw bytes of the value: buf[off, end) */
    uint32_t soff, slen;    /* string payload                        */
    int32_t  child, next;   /* first child / next sibling, -1 = none */
    uint8_t  type;
} bnode;

typedef struct {
    const uint8_t *buf;
    size_t      len;
    bnode      *n;
    int         nn, cap, maxn;
    const char *err;
} bdoc;

#define BE_MAX_DEPTH 32

static int be_node(bdoc *d, uint8_t type, size_t off)
{
    if (d->nn >= d->maxn) { d->err = "too many elements"; return -1; }
    if (d->nn == d->cap) {
        int nc = d->cap ? d->cap * 2 : 32;
        if (nc > d->maxn) nc = d->maxn;
        d->n = xrealloc(d->n, (size_t)nc * sizeof(bnode));
        d->cap = nc;
    }
    bnode *x = &d->n[d->nn];
    memset(x, 0, sizeof *x);
    x->type = type;
    x->off = (uint32_t)off;
    x->child = x->next = -1;
    return d->nn++;
}

static int be_parse(bdoc *d, size_t *pos, int depth)
{
    const uint8_t *b = d->buf;
    size_t len = d->len, p = *pos;
    int idx;
    if (depth > BE_MAX_DEPTH) { d->err = "nested too deeply"; return -1; }
    if (p >= len) { d->err = "truncated"; return -1; }
    uint8_t c = b[p];
    if (c == 'i') {
        size_t q = p + 1;
        int neg = 0;
        if (q < len && b[q] == '-') { neg = 1; q++; }
        size_t ds = q;
        uint64_t v = 0, lim = neg ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
        while (q < len && b[q] >= '0' && b[q] <= '9') {
            uint64_t dg = (uint64_t)(b[q] - '0');
            if (v > (lim - dg) / 10) { d->err = "integer overflow"; return -1; }
            v = v * 10 + dg;
            q++;
        }
        if (q == ds || q >= len || b[q] != 'e') { d->err = "bad integer"; return -1; }
        if ((b[ds] == '0' && q - ds > 1) || (neg && v == 0)) { d->err = "bad integer"; return -1; }
        if ((idx = be_node(d, BE_INT, p)) < 0) return -1;
        d->n[idx].ival = !neg ? (int64_t)v : v == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)v;
        p = q + 1;
    } else if (c >= '0' && c <= '9') {
        size_t q = p;
        uint64_t n = 0;
        while (q < len && b[q] >= '0' && b[q] <= '9') {
            if (q - p >= 10) { d->err = "string too long"; return -1; }
            n = n * 10 + (uint64_t)(b[q] - '0');
            q++;
        }
        if (q >= len || b[q] != ':') { d->err = "bad string"; return -1; }
        if (b[p] == '0' && q - p > 1) { d->err = "bad string"; return -1; }
        q++;
        if (n > len - q) { d->err = "truncated string"; return -1; }
        if ((idx = be_node(d, BE_STR, p)) < 0) return -1;
        d->n[idx].soff = (uint32_t)q;
        d->n[idx].slen = (uint32_t)n;
        p = q + (size_t)n;
    } else if (c == 'l' || c == 'd') {
        if ((idx = be_node(d, c == 'l' ? BE_LIST : BE_DICT, p)) < 0) return -1;
        size_t q = p + 1;
        int last = -1, cnt = 0;
        for (;;) {
            if (q >= len) { d->err = "truncated"; return -1; }
            if (b[q] == 'e') { q++; break; }
            int ch = be_parse(d, &q, depth + 1);
            if (ch < 0) return -1;
            if (c == 'd' && !(cnt & 1) && d->n[ch].type != BE_STR) {
                d->err = "dictionary key is not a string";
                return -1;
            }
            if (last < 0) d->n[idx].child = ch;
            else d->n[last].next = ch;
            last = ch;
            cnt++;
        }
        if (c == 'd' && (cnt & 1)) { d->err = "dictionary key without a value"; return -1; }
        p = q;
    } else {
        d->err = "invalid data";
        return -1;
    }
    d->n[idx].end = (uint32_t)p;
    *pos = p;
    return idx;
}

/* Decode one value at the start of buf. used (optional) receives its
   length; without it, trailing bytes are ignored. Node 0 is the root. */
static int be_decode(bdoc *d, const void *buf, size_t len, int maxn, size_t *used)
{
    memset(d, 0, sizeof *d);
    d->buf = buf;
    d->len = len;
    d->maxn = maxn;
    if (len > 0x7FFFFFF0u) { d->err = "too large"; return -1; }
    size_t pos = 0;
    if (be_parse(d, &pos, 0) < 0) return -1;
    if (used) *used = pos;
    return 0;
}

static void be_free(bdoc *d) { free(d->n); d->n = NULL; d->nn = d->cap = 0; }

static int be_type(const bdoc *d, int i) { return (i < 0 || i >= d->nn) ? 0 : d->n[i].type; }

static int be_get(const bdoc *d, int dict, const char *key)
{
    if (be_type(d, dict) != BE_DICT) return -1;
    size_t kl = strlen(key);
    for (int k = d->n[dict].child; k >= 0;) {
        int v = d->n[k].next;
        if (v < 0) break;
        if (d->n[k].slen == kl && memcmp(d->buf + d->n[k].soff, key, kl) == 0) return v;
        k = d->n[v].next;
    }
    return -1;
}

static const uint8_t *be_str(const bdoc *d, int i, size_t *len)
{
    if (be_type(d, i) != BE_STR) return NULL;
    *len = d->n[i].slen;
    return d->buf + d->n[i].soff;
}

static int be_int(const bdoc *d, int i, int64_t *v)
{
    if (be_type(d, i) != BE_INT) return 0;
    *v = d->n[i].ival;
    return 1;
}

static char *be_cstr(const bdoc *d, int i)      /* NUL-terminated copy (NULs -> '_') */
{
    size_t n;
    const uint8_t *s = be_str(d, i, &n);
    if (!s) return NULL;
    char *r = xmalloc(n + 1);
    for (size_t k = 0; k < n; k++) r[k] = s[k] ? (char)s[k] : '_';
    r[n] = 0;
    return r;
}

static void be_put_str(sbuf *s, const void *p, size_t n)
{
    sb_printf(s, "%zu:", n);
    sb_putn(s, (const char *)p, n);
}

/* ================================================================== */
/* addresses                                                           */
/* ================================================================== */
typedef struct { uint8_t fam; uint8_t ip[16]; uint16_t port; } baddr;   /* fam 4 or 6 */

static int addr_eq(const baddr *a, const baddr *b)
{
    return a->fam == b->fam && a->port == b->port && memcmp(a->ip, b->ip, a->fam == 6 ? 16 : 4) == 0;
}

static void addr_v4(baddr *a, const uint8_t *ip, uint16_t port)
{
    memset(a, 0, sizeof *a);
    a->fam = 4;
    memcpy(a->ip, ip, 4);
    a->port = port;
}

static void addr_v6(baddr *a, const uint8_t *ip, uint16_t port)
{
    static const uint8_t mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };
    if (memcmp(ip, mapped, 12) == 0) { addr_v4(a, ip + 12, port); return; }
    memset(a, 0, sizeof *a);
    a->fam = 6;
    memcpy(a->ip, ip, 16);
    a->port = port;
}

static int addr_to_sa(const baddr *a, struct sockaddr_storage *ss)
{
    memset(ss, 0, sizeof *ss);
    if (a->fam == 4) {
        struct sockaddr_in *s = (struct sockaddr_in *)ss;
        s->sin_family = AF_INET;
        s->sin_port = htons(a->port);
        memcpy(&s->sin_addr, a->ip, 4);
        return (int)sizeof *s;
    }
    struct sockaddr_in6 *s = (struct sockaddr_in6 *)ss;
    s->sin6_family = AF_INET6;
    s->sin6_port = htons(a->port);
    memcpy(&s->sin6_addr, a->ip, 16);
    return (int)sizeof *s;
}

static int addr_from_sa(baddr *a, const struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *s = (const struct sockaddr_in *)sa;
        addr_v4(a, (const uint8_t *)&s->sin_addr, ntohs(s->sin_port));
        return 1;
    }
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *s = (const struct sockaddr_in6 *)sa;
        addr_v6(a, (const uint8_t *)&s->sin6_addr, ntohs(s->sin6_port));
        return 1;
    }
    return 0;
}

static const char *addr_str(const baddr *a, char *buf, size_t n)
{
    char ip[64];
    if (a->fam == 4) {
        snprintf(buf, n, "%u.%u.%u.%u:%u", a->ip[0], a->ip[1], a->ip[2], a->ip[3], a->port);
        return buf;
    }
    struct in6_addr x;
    memcpy(&x, a->ip, 16);
    if (!inet_ntop(AF_INET6, &x, ip, sizeof ip)) strcpy(ip, "?");
    snprintf(buf, n, "[%s]:%u", ip, a->port);
    return buf;
}

/* worth connecting to at all */
static int addr_usable(const baddr *a)
{
    static const uint8_t zero[16];
    if (a->port == 0) return 0;
    if (a->fam == 4) return a->ip[0] != 0 && a->ip[0] < 224;
    if (a->fam == 6) return memcmp(a->ip, zero, 16) != 0 && a->ip[0] != 0xFF;
    return 0;
}

/* public IPv4 (DHT nodes on private networks are junk to us) */
static int addr_public4(const uint8_t *ip)
{
    if (ip[0] == 0 || ip[0] == 10 || ip[0] == 127 || ip[0] >= 224) return 0;
    if (ip[0] == 169 && ip[1] == 254) return 0;
    if (ip[0] == 172 && (ip[1] & 0xF0) == 16) return 0;
    if (ip[0] == 192 && ip[1] == 168) return 0;
    if (ip[0] == 100 && (ip[1] & 0xC0) == 64) return 0;
    return 1;
}

static int addr_from_text_ip(const char *host, uint16_t port, baddr *a)
{
    struct in_addr v4;
    struct in6_addr v6;
    if (inet_pton(AF_INET, host, &v4) == 1) { addr_v4(a, (const uint8_t *)&v4, port); return 1; }
    if (inet_pton(AF_INET6, host, &v6) == 1) { addr_v6(a, (const uint8_t *)&v6, port); return 1; }
    return 0;
}

/* "1.2.3.4:6881" or "[2001:db8::1]:6881" (numeric only) */
static int addr_parse(const char *s, baddr *a)
{
    char host[64];
    const char *port;
    if (*s == '[') {
        const char *rb = strchr(s, ']');
        if (!rb || rb[1] != ':' || rb - s - 1 >= (int)sizeof host) return 0;
        memcpy(host, s + 1, (size_t)(rb - s - 1));
        host[rb - s - 1] = 0;
        port = rb + 2;
    } else {
        const char *c = strrchr(s, ':');
        if (!c || c - s >= (int)sizeof host || strchr(s, ':') != c) return 0;
        memcpy(host, s, (size_t)(c - s));
        host[c - s] = 0;
        port = c + 1;
    }
    char *e;
    long pv = strtol(port, &e, 10);
    if (e == port || *e || pv <= 0 || pv > 65535) return 0;
    return addr_from_text_ip(host, (uint16_t)pv, a);
}

/* ================================================================== */
/* metainfo                                                            */
/* ================================================================== */
typedef struct {
    char    *sub;       /* path inside the torrent folder (multi-file), sanitized */
    char    *rel;       /* path relative to out_dir                              */
    char    *path;      /* out_dir\rel                                           */
    int64_t  off, len;  /* position in the torrent's byte stream                 */
    int      pad;       /* BEP 47 padding file: never created, reads as zeros    */
    HANDLE   h;
    int      rw;        /* h was opened for writing too                           */
    uint32_t used;
} bfile;

typedef struct {
    uint8_t   ih[20];
    uint8_t  *info;          /* raw bencoded info dictionary (owned) */
    size_t    info_len;
    char     *disp;          /* name from the torrent (UTF-8 if it was) */
    int64_t   total, plen;
    int       np;
    const uint8_t *hashes;   /* np * 20 bytes inside info */
    int       multi, priv;
    int       dropped;       /* unsafe path elements dropped */
    int       renamed;       /* files renamed to avoid collisions */
    bfile    *files;
    int       nfiles;
    uint32_t  lru;           /* open file handles */
    int       nopen;
    char      ioerr[300];
} meta;

static void meta_close_files(meta *m)
{
    for (int i = 0; i < m->nfiles; i++)
        if (m->files[i].h != INVALID_HANDLE_VALUE && m->files[i].h != NULL) {
            CloseHandle(m->files[i].h);
            m->files[i].h = INVALID_HANDLE_VALUE;
        }
    m->nopen = 0;
}

static void meta_free(meta *m)
{
    if (m->files) {
        meta_close_files(m);
        for (int i = 0; i < m->nfiles; i++) {
            free(m->files[i].sub);
            free(m->files[i].rel);
            free(m->files[i].path);
        }
    }
    free(m->files);
    free(m->info);
    free(m->disp);
    memset(m, 0, sizeof *m);
}

/* One element of a file path from a torrent. "", "." and ".." (and
   anything Windows would treat as them, like ". ." or "..  ") are
   dropped (NULL); everything else becomes a safe file name. */
static char *path_component(const uint8_t *s, size_t n)
{
    size_t dots = 0, other = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '.') dots++;
        else if (s[i] != ' ') other++;
    }
    if (n == 0 || (other == 0 && dots <= 2)) return NULL;
    char *raw = xmalloc(n + 1);
    for (size_t i = 0; i < n; i++) raw[i] = s[i] ? (char)s[i] : '_';
    raw[n] = 0;
    char *c = sanitize_filename(raw);     /* also maps / \ : and device names */
    free(raw);
    return c;
}

/* a usable path.utf-8: a non-empty list of valid UTF-8 strings */
static int path_list_ok_utf8(const bdoc *d, int list)
{
    if (be_type(d, list) != BE_LIST || d->n[list].child < 0) return 0;
    for (int k = d->n[list].child; k >= 0; k = d->n[k].next) {
        size_t n;
        const uint8_t *s = be_str(d, k, &n);
        if (!s || !utf8_valid(s, n)) return 0;
    }
    return 1;
}

/* path list -> "a\b\c" ("" when every element was dropped, NULL when the
   list is malformed) */
static char *join_path_list(const bdoc *d, int list, int *dropped)
{
    if (be_type(d, list) != BE_LIST) return NULL;
    sbuf s = { 0 };
    sb_puts(&s, "");
    for (int k = d->n[list].child; k >= 0; k = d->n[k].next) {
        size_t n;
        const uint8_t *c = be_str(d, k, &n);
        if (!c) { sb_free(&s); return NULL; }
        char *comp = path_component(c, n);
        if (!comp) { (*dropped)++; continue; }
        if (s.len) sb_putc(&s, '\\');
        sb_puts(&s, comp);
        free(comp);
    }
    if (s.len > 8000) { sb_free(&s); return NULL; }
    return sb_steal(&s);
}

/* Parse a raw info dictionary (copied). 0 ok, -1 error. */
static int meta_parse(meta *m, const uint8_t *info, size_t len, char *err, size_t el)
{
    memset(m, 0, sizeof *m);
    m->info = xmalloc(len ? len : 1);
    memcpy(m->info, info, len);
    m->info_len = len;
    if (!sha1(m->info, len, m->ih)) { snprintf(err, el, "SHA-1 is not available on this system"); goto fail0; }

    bdoc d;
    if (be_decode(&d, m->info, len, 4000000, NULL) < 0 || d.n[0].type != BE_DICT) {
        snprintf(err, el, "the torrent metadata is damaged (%s)", d.err ? d.err : "not a dictionary");
        goto fail;
    }
    int nm = be_get(&d, 0, "name.utf-8");
    size_t nl;
    const uint8_t *ns = be_str(&d, nm, &nl);
    if (!ns || !nl || !utf8_valid(ns, nl)) { nm = be_get(&d, 0, "name"); ns = be_str(&d, nm, &nl); }
    if (!ns) { snprintf(err, el, "invalid torrent: it has no name"); goto fail; }
    m->disp = be_cstr(&d, nm);

    int64_t ver = 1;
    be_int(&d, be_get(&d, 0, "meta version"), &ver);
    int pc = be_get(&d, 0, "pieces"), fl = be_get(&d, 0, "files"), ln = be_get(&d, 0, "length");
    if (pc < 0 || (fl < 0 && ln < 0)) {
        if (ver >= 2 || be_get(&d, 0, "file tree") >= 0)
            snprintf(err, el, "this is a BitTorrent v2-only torrent, which is not supported "
                              "(v1 and hybrid v1/v2 torrents work)");
        else
            snprintf(err, el, "invalid torrent: the piece hashes or the file list are missing");
        goto fail;
    }
    if (!be_int(&d, be_get(&d, 0, "piece length"), &m->plen) || m->plen < 1 || m->plen > MAX_PLEN) {
        snprintf(err, el, "invalid torrent: bad piece length");
        goto fail;
    }
    size_t hl;
    const uint8_t *ph = be_str(&d, pc, &hl);
    if (!ph || hl % 20) { snprintf(err, el, "invalid torrent: bad piece hashes"); goto fail; }

    if (fl >= 0) {
        if (be_type(&d, fl) != BE_LIST) { snprintf(err, el, "invalid torrent: bad file list"); goto fail; }
        int cnt = 0;
        for (int k = d.n[fl].child; k >= 0; k = d.n[k].next) cnt++;
        if (cnt == 0 || cnt > 1000000) { snprintf(err, el, "invalid torrent: bad file list"); goto fail; }
        m->multi = 1;
        m->files = xcalloc((size_t)cnt, sizeof(bfile));
        int64_t off = 0;
        int i = 0;
        for (int k = d.n[fl].child; k >= 0; k = d.n[k].next, i++) {
            bfile *f = &m->files[i];
            f->h = INVALID_HANDLE_VALUE;
            m->nfiles = i + 1;
            int64_t flen;
            if (be_type(&d, k) != BE_DICT || !be_int(&d, be_get(&d, k, "length"), &flen) ||
                flen < 0 || flen > MAX_TOTAL - off) {
                snprintf(err, el, "invalid torrent: bad file size");
                goto fail;
            }
            size_t al = 0;
            const uint8_t *attr = be_str(&d, be_get(&d, k, "attr"), &al);
            f->pad = attr && memchr(attr, 'p', al) != NULL;
            /* path.utf-8 when it is usable, else path (also when nothing of
               path.utf-8 survives sanitizing but path has something) */
            int pl = be_get(&d, k, "path.utf-8"), drop = 0;
            char *sub = path_list_ok_utf8(&d, pl) ? join_path_list(&d, pl, &drop) : NULL;
            if (!sub || !*sub) {
                int drop2 = 0;
                char *alt = join_path_list(&d, be_get(&d, k, "path"), &drop2);
                if (alt && (*alt || !sub)) { free(sub); sub = alt; drop = drop2; }
                else free(alt);
            }
            m->dropped += drop;
            if (!sub) { snprintf(err, el, "invalid torrent: bad file path"); goto fail; }
            if (!*sub && !f->pad) {      /* nothing usable left of the path */
                free(sub);
                sub = xasprintf("file %d", i + 1);
            }
            f->sub = sub;
            f->off = off;
            f->len = flen;
            off += flen;
        }
        m->total = off;
    } else {
        int64_t flen;
        if (!be_int(&d, ln, &flen) || flen < 0 || flen > MAX_TOTAL) {
            snprintf(err, el, "invalid torrent: bad file size");
            goto fail;
        }
        m->files = xcalloc(1, sizeof(bfile));
        m->files[0].h = INVALID_HANDLE_VALUE;
        m->files[0].len = flen;
        m->nfiles = 1;
        m->total = flen;
    }
    if (m->total == 0) { snprintf(err, el, "the torrent contains no data"); goto fail; }
    int64_t np = (m->total - 1) / m->plen + 1;
    if ((int64_t)(hl / 20) != np || np > INT_MAX / 4) {
        snprintf(err, el, "invalid torrent: the piece count does not match the size");
        goto fail;
    }
    m->np = (int)np;
    m->hashes = ph;
    int64_t pv = 0;
    m->priv = be_int(&d, be_get(&d, 0, "private"), &pv) && pv == 1;
    be_free(&d);
    return 0;
fail:
    be_free(&d);
fail0:
    meta_free(m);
    return -1;
}

/* ---- file layout ---- */
typedef struct { char **k; int *v; size_t cap, n; } nameset;   /* v: a number per name */

static uint64_t str_hash(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 1099511628211ULL; }
    return h;
}

static int *nameset_find(const nameset *t, const char *s)
{
    if (!t->cap) return NULL;
    for (size_t i = str_hash(s) & (t->cap - 1); t->k[i]; i = (i + 1) & (t->cap - 1))
        if (!strcmp(t->k[i], s)) return &t->v[i];
    return NULL;
}

static int nameset_has(const nameset *t, const char *s) { return nameset_find(t, s) != NULL; }

/* adds s (number 0) unless it is there; returns its number */
static int *nameset_add(nameset *t, const char *s)
{
    int *v = nameset_find(t, s);
    if (v) return v;
    if ((t->n + 1) * 2 > t->cap) {
        size_t nc = t->cap ? t->cap * 2 : 64;
        char **nk = xcalloc(nc, sizeof(char *));
        int *nv = xcalloc(nc, sizeof(int));
        for (size_t i = 0; i < t->cap; i++) {
            if (!t->k[i]) continue;
            size_t j = str_hash(t->k[i]) & (nc - 1);
            while (nk[j]) j = (j + 1) & (nc - 1);
            nk[j] = t->k[i];
            nv[j] = t->v[i];
        }
        free(t->k);
        free(t->v);
        t->k = nk;
        t->v = nv;
        t->cap = nc;
    }
    size_t j = str_hash(s) & (t->cap - 1);
    while (t->k[j]) j = (j + 1) & (t->cap - 1);
    t->k[j] = xstrdup(s);
    t->v[j] = 0;
    t->n++;
    return &t->v[j];
}

static void nameset_free(nameset *t)
{
    for (size_t i = 0; i < t->cap; i++) free(t->k[i]);
    free(t->k);
    free(t->v);
    memset(t, 0, sizeof *t);
}

static char *lower_key(const char *s)          /* Windows file names ignore case */
{
    wchar_t *w = u8_to_w(s);
    CharLowerBuffW(w, (DWORD)wcslen(w));
    char *r = w_to_u8(w);
    free(w);
    return r;
}

static char *suffixed(const char *rel, int n)   /* "d\name.ext" -> "d\name (n).ext" */
{
    const char *base = path_basename(rel);
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base) dot = base + strlen(base);
    return xasprintf("%.*s (%d)%s", (int)(dot - rel), rel, n, dot);
}

static wchar_t *full_wpath(const char *p)
{
    wchar_t *w = u8_to_w(p);
    DWORD n = GetFullPathNameW(w, 0, NULL, NULL);
    if (!n) return w;
    wchar_t *f = xmalloc(((size_t)n + 1) * sizeof(wchar_t));
    DWORD r = GetFullPathNameW(w, n + 1, f, NULL);
    if (!r || r > n) { free(f); return w; }
    free(w);
    return f;
}

/* Is p a path strictly inside the folder base? (after Windows resolves
   "." / ".." / slashes the way CreateFile would) */
static int path_inside(const char *base, const char *p)
{
    wchar_t *b = full_wpath(base), *f = full_wpath(p);
    size_t nb = wcslen(b);
    while (nb > 0 && (b[nb - 1] == L'\\' || b[nb - 1] == L'/')) nb--;
    int ok = nb > 0 && wcslen(f) > nb + 1 && _wcsnicmp(b, f, nb) == 0 && f[nb] == L'\\';
    free(b);
    free(f);
    return ok;
}

/* Final paths: out_dir\top (single file) or out_dir\top\sub (multi-file).
   Two files that would land on the same path (or a file where another one
   needs a folder) get " (2)" names; every path is checked to stay inside
   out_dir. */
static int meta_layout(meta *m, const char *out_dir, const char *top, char *err, size_t el)
{
    for (int i = 0; i < m->nfiles; i++) {
        bfile *f = &m->files[i];
        free(f->rel);
        free(f->path);
        f->rel = m->multi ? xasprintf("%s\\%s", top, f->sub ? f->sub : "") : xstrdup(top);
        f->path = NULL;
    }
    m->renamed = 0;
    if (m->multi) {
        nameset dirs = { 0 }, used = { 0 }, next = { 0 };    /* next: name -> next " (n)" to try */
        int rc = 0;
        for (int i = 0; i < m->nfiles; i++) {
            if (m->files[i].pad) continue;
            char *key = lower_key(m->files[i].rel);
            for (char *q = key; *q; q++)
                if (*q == '\\') { *q = 0; nameset_add(&dirs, key); *q = '\\'; }
            free(key);
        }
        for (int i = 0; i < m->nfiles && rc == 0; i++) {
            bfile *f = &m->files[i];
            if (f->pad) continue;
            char *key = lower_key(f->rel);
            if (nameset_has(&dirs, key) || nameset_has(&used, key)) {
                /* carry on from where the previous file of this name stopped:
                   every name tried is then a different taken one, so even a
                   million equal names cost a million tries, not a million squared */
                int *nx = nameset_add(&next, key);
                if (*nx < 2) *nx = 2;
                for (;;) {
                    if (*nx > 4000000) {
                        snprintf(err, el, "invalid torrent: too many files named %s", f->rel);
                        rc = -1;
                        break;
                    }
                    char *alt = suffixed(f->rel, (*nx)++);
                    char *ak = lower_key(alt);
                    if (!nameset_has(&dirs, ak) && !nameset_has(&used, ak)) {
                        free(f->rel);
                        f->rel = alt;
                        free(key);
                        key = ak;
                        m->renamed++;
                        break;
                    }
                    free(alt);
                    free(ak);
                }
            }
            if (rc == 0) nameset_add(&used, key);
            free(key);
        }
        nameset_free(&dirs);
        nameset_free(&used);
        nameset_free(&next);
        if (rc) return rc;
    }
    for (int i = 0; i < m->nfiles; i++) {
        bfile *f = &m->files[i];
        f->path = path_join(out_dir, f->rel);
        if (!f->pad && !path_inside(out_dir, f->path)) {
            snprintf(err, el, "unsafe file path in the torrent: %s", f->rel);
            return -1;
        }
    }
    return 0;
}

/* ================================================================== */
/* magnet links                                                        */
/* ================================================================== */
#define MAG_MAX_TR 64
typedef struct {
    uint8_t ih[20];
    char   *dn;
    char   *tr[MAG_MAX_TR];
    int     ntr;
    char   *pe[MAG_MAX_TR];
    int     npe;
} magnet;

static int b32val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a';
    if (c >= '2' && c <= '7') return c - '2' + 26;
    return -1;
}

/* 40 hex digits or 32 base32 characters -> 20 bytes */
static int parse_btih(const char *h, size_t n, uint8_t out[20])
{
    if (n == 40) {
        for (int i = 0; i < 20; i++) {
            int a = hexv(h[2 * i]), b = hexv(h[2 * i + 1]);
            if (a < 0 || b < 0) return 0;
            out[i] = (uint8_t)(a << 4 | b);
        }
        return 1;
    }
    if (n == 32) {
        uint32_t acc = 0;
        int nb = 0, o = 0;
        for (size_t i = 0; i < 32; i++) {
            int v = b32val((unsigned char)h[i]);
            if (v < 0) return 0;
            acc = (acc << 5) | (uint32_t)v;
            nb += 5;
            if (nb >= 8) {
                out[o++] = (uint8_t)(acc >> (nb - 8));
                nb -= 8;
                acc &= (1u << nb) - 1;
            }
        }
        return o == 20;
    }
    return 0;
}

static void magnet_free(magnet *m)
{
    free(m->dn);
    for (int i = 0; i < m->ntr; i++) free(m->tr[i]);
    for (int i = 0; i < m->npe; i++) free(m->pe[i]);
    memset(m, 0, sizeof *m);
}

/* Trim s where it is: the text stays at the start of its buffer, so the
   pointer can still be freed (unlike str_trim's result). */
static char *trim_owned(char *s)
{
    size_t a = 0, n = strlen(s);
    while (a < n && isspace((unsigned char)s[a])) a++;
    while (n > a && isspace((unsigned char)s[n - 1])) n--;
    memmove(s, s + a, n - a);
    s[n - a] = 0;
    return s;
}

static int magnet_parse(const char *uri, magnet *m, char *err, size_t el)
{
    memset(m, 0, sizeof *m);
    if (!str_istarts(uri, "magnet:?")) { snprintf(err, el, "not a magnet link"); return -1; }
    int have = 0, bad = 0, v2 = 0;
    const char *p = uri + 8;
    const char *end = p + strcspn(p, "#");          /* a #fragment is not part of the query */
    while (p < end) {
        size_t n = strcspn(p, "&");
        if (n > (size_t)(end - p)) n = (size_t)(end - p);
        const char *kv = p;
        size_t kvn = n;
        if (kvn >= 4 && _strnicmp(kv, "amp;", 4) == 0) { kv += 4; kvn -= 4; }   /* pasted from HTML */
        const char *eq = memchr(kv, '=', kvn);
        if (eq) {
            char *key = xstrndup(kv, (size_t)(eq - kv));
            for (char *q = key; *q; q++) *q = (char)tolower((unsigned char)*q);
            const char *v = eq + 1;
            size_t vl = kvn - (size_t)(eq - kv) - 1;
            if (!strcmp(key, "xt") || !strncmp(key, "xt.", 3)) {
                char *val = url_decode(v, vl);
                char *t = str_trim(val);
                if (str_istarts(t, "urn:btih:")) {
                    uint8_t ih[20];
                    if (parse_btih(t + 9, strlen(t + 9), ih)) {
                        if (!have) memcpy(m->ih, ih, 20);
                        have = 1;
                    } else bad = 1;
                } else if (str_istarts(t, "urn:btmh:")) v2 = 1;
                free(val);
            } else if (!strcmp(key, "dn")) {
                char *raw = xstrndup(v, vl);
                for (char *q = raw; *q; q++) if (*q == '+') *q = ' ';
                free(m->dn);
                m->dn = url_decode(raw, strlen(raw));
                free(raw);
            } else if (!strcmp(key, "tr") || !strncmp(key, "tr.", 3)) {
                if (m->ntr < MAG_MAX_TR) m->tr[m->ntr++] = trim_owned(url_decode(v, vl));
            } else if (!strcmp(key, "x.pe")) {
                if (m->npe < MAG_MAX_TR) m->pe[m->npe++] = url_decode(v, vl);
            }
            free(key);
        }
        p += n;
        if (p < end && *p == '&') p++;
    }
    if (!have) {
        snprintf(err, el, "%s", bad ? "the magnet link has an invalid info-hash" :
                 v2 ? "v2-only magnet links (urn:btmh) are not supported" :
                 "the magnet link has no BitTorrent info-hash (xt=urn:btih:...)");
        magnet_free(m);
        return -1;
    }
    return 0;
}

int is_magnet(const char *s) { return s && str_istarts(s, "magnet:?"); }

/* ================================================================== */
/* storage: the torrent's byte stream mapped onto its files            */
/* ================================================================== */
typedef struct { int fi; int64_t foff, len, boff; } fseg;

/* Split [off, off+len) into per-file pieces. Returns a new array (count in
   *n), or NULL with *n = -1 when the range is outside the torrent. */
static fseg *map_range(const bfile *f, int nf, int64_t off, int64_t len, int *n)
{
    *n = 0;
    if (off < 0 || len < 0) { *n = -1; return NULL; }
    if (len == 0) return NULL;
    int lo = 0, hi = nf;                 /* first file that ends after off */
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (f[mid].off + f[mid].len <= off) lo = mid + 1;
        else hi = mid;
    }
    fseg *s = NULL;
    int cnt = 0, cap = 0;
    int64_t pos = off, end = off + len;
    for (int i = lo; i < nf && pos < end; i++) {
        if (f[i].len == 0 || f[i].off + f[i].len <= pos) continue;
        if (f[i].off > pos) break;
        int64_t fe = f[i].off + f[i].len;
        int64_t take = (fe < end ? fe : end) - pos;
        if (cnt == cap) { cap = cap ? cap * 2 : 4; s = xrealloc(s, (size_t)cap * sizeof(fseg)); }
        s[cnt].fi = i;
        s[cnt].foff = pos - f[i].off;
        s[cnt].len = take;
        s[cnt].boff = pos - off;
        cnt++;
        pos += take;
    }
    if (pos < end) { free(s); *n = -1; return NULL; }
    *n = cnt;
    return s;
}

static void set_sparse(HANDLE h, BOOLEAN on)
{
    struct { BOOLEAN SetSparse; } sb = { on };
    DWORD ret;
    DeviceIoControl(h, FSCTL_SET_SPARSE, &sb, sizeof sb, NULL, 0, &ret, NULL);
}

/* write = 0: only reading (checking existing files, uploading), which also
   works on read-only files and on files another program has open for
   writing; write = 1 creates the file (and its folders) when needed */
static HANDLE stor_open(meta *m, int fi, int write)
{
    bfile *f = &m->files[fi];
    if (f->h != INVALID_HANDLE_VALUE) {
        if (f->rw || !write) { f->used = ++m->lru; return f->h; }
        CloseHandle(f->h);                      /* was opened for reading only */
        f->h = INVALID_HANDLE_VALUE;
        m->nopen--;
    }
    if (m->nopen >= 48) {                       /* keep the number of open files bounded */
        int victim = -1;
        for (int i = 0; i < m->nfiles; i++)
            if (m->files[i].h != INVALID_HANDLE_VALUE && (victim < 0 || m->files[i].used < m->files[victim].used))
                victim = i;
        if (victim >= 0) { CloseHandle(m->files[victim].h); m->files[victim].h = INVALID_HANDLE_VALUE; m->nopen--; }
    }
    wchar_t *w = wpath(f->path);
    HANDLE h;
    if (!write) {
        h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    } else {
        h = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PATH_NOT_FOUND) {
            char *dir = path_dirname(f->path);
            dir_create(dir);
            free(dir);
            h = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
        }
    }
    free(w);
    if (h == INVALID_HANDLE_VALUE) {
        char e[200];
        snprintf(m->ioerr, sizeof m->ioerr, "cannot open %s: %s", f->path, win_errstr(GetLastError(), e, sizeof e));
        return INVALID_HANDLE_VALUE;
    }
    f->h = h;
    f->rw = write;
    f->used = ++m->lru;
    m->nopen++;
    return h;
}

/* Read or write [off, off+len) of the torrent. 0 ok, -1 error (m->ioerr). */
static int stor_io(meta *m, int64_t off, uint8_t *buf, int64_t len, int write)
{
    int n;
    fseg *sg = map_range(m->files, m->nfiles, off, len, &n);
    if (n < 0) { snprintf(m->ioerr, sizeof m->ioerr, "internal error: range outside the torrent"); return -1; }
    int rc = 0;
    for (int i = 0; i < n && rc == 0; i++) {
        bfile *f = &m->files[sg[i].fi];
        uint8_t *p = buf + sg[i].boff;
        int64_t left = sg[i].len, fo = sg[i].foff;
        if (f->pad) {
            if (!write) memset(p, 0, (size_t)left);
            continue;
        }
        HANDLE h = stor_open(m, sg[i].fi, write);
        if (h == INVALID_HANDLE_VALUE) { rc = -1; break; }
        while (left > 0) {
            DWORD k = left > (1 << 30) ? (1u << 30) : (DWORD)left, got = 0;
            OVERLAPPED ov;
            ZeroMemory(&ov, sizeof ov);
            ov.Offset = (DWORD)((uint64_t)fo & 0xFFFFFFFF);
            ov.OffsetHigh = (DWORD)((uint64_t)fo >> 32);
            BOOL ok = write ? WriteFile(h, p, k, &got, &ov) : ReadFile(h, p, k, &got, &ov);
            if (!ok && !write && GetLastError() == ERROR_HANDLE_EOF) { ok = TRUE; got = 0; }
            if (!ok || (write && got == 0)) {
                char e[200];
                snprintf(m->ioerr, sizeof m->ioerr, "cannot %s %s: %s", write ? "write to" : "read",
                         f->path, win_errstr(GetLastError(), e, sizeof e));
                rc = -1;
                break;
            }
            if (got == 0) { memset(p, 0, (size_t)left); break; }   /* past the end: not there yet */
            p += got;
            fo += got;
            left -= got;
        }
    }
    free(sg);
    return rc;
}

/* Give every file its final size: missing ones are created (sparse, so
   nothing is written), longer ones are cut. Files that already have the
   right size are not opened. Returns 0, -1 on error, -2 when aborted. */
static int stor_prepare(meta *m, const ul_task *t)
{
    for (int i = 0; i < m->nfiles; i++) {
        bfile *f = &m->files[i];
        if (f->pad) continue;
        if ((i & 63) == 63 && task_aborted(t)) return -2;
        if (f->h == INVALID_HANDLE_VALUE && file_size(f->path) == f->len) continue;
        HANDLE h = stor_open(m, i, 1);
        if (h == INVALID_HANDLE_VALUE) return -1;
        LARGE_INTEGER sz;
        int known = GetFileSizeEx(h, &sz) != 0;
        if (!known || sz.QuadPart != f->len) {
            if (f->len > 0 && (!known || f->len > sz.QuadPart)) set_sparse(h, TRUE);   /* growing only */
            LARGE_INTEGER li;
            li.QuadPart = f->len;
            if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN) || !SetEndOfFile(h)) {
                char e[200];
                snprintf(m->ioerr, sizeof m->ioerr, "cannot create %s: %s", f->path, win_errstr(GetLastError(), e, sizeof e));
                return -1;
            }
        }
        if (m->nfiles > 48) { CloseHandle(f->h); f->h = INVALID_HANDLE_VALUE; m->nopen--; }
    }
    return 0;
}

/* complete: files are no longer sparse */
static void stor_finish(meta *m)
{
    meta_close_files(m);
    for (int i = 0; i < m->nfiles; i++) {
        bfile *f = &m->files[i];
        if (f->pad || f->len == 0) continue;
        wchar_t *w = wpath(f->path);
        HANDLE h = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        free(w);
        if (h == INVALID_HANDLE_VALUE) continue;
        set_sparse(h, FALSE);
        CloseHandle(h);
    }
}

/* ================================================================== */
/* the torrent                                                         */
/* ================================================================== */
enum { CS_IDLE, CS_CONNECTED, CS_BANNED };
enum { SRC_TRACKER = 1, SRC_DHT = 2, SRC_PEX = 4, SRC_LINK = 8 };

typedef struct {
    baddr   a;
    uint8_t st, src, fails, tried, good;
    int     hashfails;
    double  retry;
} cand;

enum { PS_CONNECTING = 1, PS_HANDSHAKE, PS_ACTIVE };

typedef struct { int piece, begin, len; double t; } preq;

#define CXL_N 64            /* requests we took back from a peer, remembered */
typedef struct { int piece, begin; } pblk;

typedef struct {
    SOCKET   s;
    baddr    a;
    int      ci, st;
    double   t_start, t_recv, t_send, t_block, t_snub;
    uint8_t *rb;
    size_t   rlen, rcap;
    uint8_t *wb;
    size_t   wpos, wlen, wcap;
    uint8_t  am_choking, am_interested, choked, interested;
    uint8_t  ext, have_all, snubbed, seed, optimistic;
    int      ut_md, ut_pex, reqq;
    int64_t  md_size;
    uint8_t *bf;                /* np bits once metadata is known */
    int      nhave;
    uint8_t *pre;               /* bitfield / haves seen before the metadata */
    size_t   pre_len;
    int      pre_isbf;
    preq     req[MAX_PIPE];
    int      nreq;
    pblk     cxl[CXL_N];        /* cancelled / released: may still arrive */
    int      ncxl, cxl_pos;
    preq     upq[MAX_UPQ];
    int      nup;
    int      md_piece, md_bad;
    double   md_t;
    int64_t  got, sent, got_sec, sent_sec, unrequested;
    double   rate, urate;
    uint8_t  id[20];
    int      dead;
    char     why[64];
} peer;

typedef struct {
    char   *url;
    int     udp;
    double  next, last;
    int     fails, busy, ok, logged, interval, minint, started;
} tracker;

/* ---- helper threads (tracker announces, DNS) ---- */
enum { JOB_ANNOUNCE, JOB_RESOLVE };

typedef struct job {
    volatile LONG refs, done;
    int        kind, ti;
    char      *url;
    uint8_t    ih[20], pid[20];
    int64_t    down, up, left;
    int        event;            /* 0 none, 1 completed, 2 started, 3 stopped */
    int        final;            /* "completed"/"stopped": not cancelled by Ctrl+C */
    uint32_t   key;
    ul_config *cfg;              /* private copies: the thread may outlive the torrent */
    ul_task   *t;
    char      *hosts[8];
    int        nhosts;
    /* results */
    int        ok, interval, minint, seeders, leechers;
    char       err[200], warn[200];
    baddr     *addr;
    int        naddr, caddr;
} job;

/* ---- DHT ---- */
#define DHT_N     200
#define DHT_PEND  32
#define DHT_ALPHA 8

typedef struct { uint8_t id[20]; baddr a; uint8_t st; } dnode;   /* st: 0 new 1 asked 2 answered 3 bad */
typedef struct { uint16_t tid; baddr a; double t; } dpend;

typedef struct {
    SOCKET  s;
    int     bound;
    uint8_t id[20];
    dnode   n[DHT_N];
    int     nn;
    dpend   pend[DHT_PEND];
    int     npend;
    baddr   routers[16];
    int     nrouters, resolving, resolved;
    double  resolve_next;
    int     searching, queries, rounds, found, total_found, logged;
    double  t_start, t_end;
    uint16_t tid;
} dht;

typedef struct {
    int      idx, len, nblk, nrecv, nfree;
    uint8_t *st;       /* per block: 0 missing, 1 requested, 2 received */
    uint8_t *dup;      /* requests in flight per block                  */
    uint8_t *from;     /* who[] index of the block's sender, 255 = ?    */
    uint8_t *data;
    baddr    who[16];
    int      nwho;
    struct sblk { baddr a; uint8_t h[8]; uint8_t ok; } *smart;
} apiece;

#define SPEED_SAMPLES 12
typedef struct { double t[SPEED_SAMPLES]; int64_t b[SPEED_SAMPLES]; int n, head; } speedo;

static double speedo_add(speedo *s, double t, int64_t bytes)
{
    s->t[s->head] = t;
    s->b[s->head] = bytes;
    s->head = (s->head + 1) % SPEED_SAMPLES;
    if (s->n < SPEED_SAMPLES) s->n++;
    int oldest = (s->head - s->n + SPEED_SAMPLES) % SPEED_SAMPLES;
    int newest = (s->head - 1 + SPEED_SAMPLES) % SPEED_SAMPLES;
    double dt = s->t[newest] - s->t[oldest];
    return dt > 0.2 ? (double)(s->b[newest] - s->b[oldest]) / dt : 0.0;
}

typedef struct {
    ul_task      *t;
    http_session *s;
    ul_config    *cfg;
    const char   *out_dir;
    double        now, t0;
    uint64_t      rng;
    uint8_t       ih[20], pid[20];
    char          ihex[41];
    char         *dn;                 /* magnet display name */
    uint32_t      key;

    /* metadata */
    int           have_meta;
    meta          m;
    char         *top;                /* file / folder name inside out_dir */
    char         *target;             /* out_dir\top */
    char         *state_path;
    int           priv;

    /* pieces */
    uint8_t      *have;
    int           nhave;
    int64_t       have_bytes, start_bytes;
    uint16_t     *avail;
    int32_t      *slot;               /* piece -> act[] index, -1 */
    apiece       *act[MAX_ACTIVE];
    int           nact, max_act, endgame;

    /* peers */
    peer         *p[MAX_PEERS];
    int           npeers, nactive, nseeds;
    cand         *c;
    int           nc;
    int           banned;
    baddr         optim;

    /* trackers, helpers, DHT */
    tracker       tr[MAX_TRACKERS];
    int           ntr;
    job          *jobs[MAX_JOBS];
    int           njobs;
    int           trk_logged;
    int           dht_on;
    dht           d;

    /* metadata download (magnets) */
    int64_t       md_size;
    double        t_md;               /* last metadata progress */
    uint8_t      *md, *md_st;
    baddr        *md_from;
    int           md_n, md_fails;
    int           md_solo;            /* after a mixed bad copy: one peer at a time */
    int           md_src_set;
    baddr         md_src;             /* that peer */

    /* statistics and timers */
    int64_t       down, up, wasted;
    int           hashfails;
    speedo        sp;
    double        dl_tokens, ul_tokens, ul_rate, speed;
    double        t_tick, t_pub, t_save, t_choke, t_optim, t_prune, t_connect, t_interest;
    double        t_last_peer, t_last_data, t_fill;
    baddr         boot[16];           /* DHT nodes listed in the .torrent */
    int           nboot;
    int           ever_peer, ever_useful, nuseful, dirty, fatal;
    char          fatal_msg[300];
    uint8_t      *iobuf;
    int           found_logged;
} bt;

static uint32_t bt_rand(bt *b)
{
    uint64_t x = b->rng;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    b->rng = x;
    return (uint32_t)((x * 0x2545F4914F6CDD1DULL) >> 32);
}

static int piece_len(const bt *b, int i)
{
    if (i == b->m.np - 1) return (int)(b->m.total - b->m.plen * (int64_t)(b->m.np - 1));
    return (int)b->m.plen;
}

static void bt_fail(bt *b, const char *fmt, ...) __attribute__((format(gnu_printf, 2, 3)));
static void bt_fail(bt *b, const char *fmt, ...)
{
    if (b->fatal) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b->fatal_msg, sizeof b->fatal_msg, fmt, ap);
    va_end(ap);
    b->fatal = 1;
}

/* ================================================================== */
/* helper threads                                                      */
/* ================================================================== */
static void job_release(job *j)
{
    if (InterlockedDecrement(&j->refs) != 0) return;
    free(j->url);
    for (int i = 0; i < j->nhosts; i++) free(j->hosts[i]);
    free(j->addr);
    if (j->cfg) cfg_free(j->cfg);
    if (j->t) task_free(j->t);
    free(j);
}

/* The final announces are sent after a pause, which on the console is the
   global Ctrl+C flag: they only stop for their own flag. */
static int job_aborted(const job *j) { return j->final ? j->t->abort != 0 : task_aborted(j->t); }

static void job_add_addr(job *j, const baddr *a)
{
    if (!addr_usable(a) || j->naddr >= 2000) return;
    if (j->naddr == j->caddr) {
        j->caddr = j->caddr ? j->caddr * 2 : 64;
        j->addr = xrealloc(j->addr, (size_t)j->caddr * sizeof(baddr));
    }
    j->addr[j->naddr++] = *a;
}

static void pct_encode(sbuf *s, const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t c = d[i];
        if (isalnum(c) || c == '.' || c == '-' || c == '_' || c == '~') sb_putc(s, (char)c);
        else sb_printf(s, "%%%02X", c);
    }
}

/* tracker reply (HTTP): bencoded dictionary */
static void announce_parse(job *j, const uint8_t *buf, size_t len)
{
    bdoc d;
    if (be_decode(&d, buf, len, 100000, NULL) < 0 || d.n[0].type != BE_DICT) {
        snprintf(j->err, sizeof j->err, "invalid reply from the tracker%s%s", d.err ? ": " : "", d.err ? d.err : "");
        be_free(&d);
        return;
    }
    size_t n;
    const uint8_t *s = be_str(&d, be_get(&d, 0, "failure reason"), &n);
    if (s) {
        char msg[160];
        clean_text(msg, sizeof msg, s, n);
        snprintf(j->err, sizeof j->err, "tracker says: %s", msg);
        be_free(&d);
        return;
    }
    if ((s = be_str(&d, be_get(&d, 0, "warning message"), &n))) clean_text(j->warn, sizeof j->warn, s, n);
    int64_t v;
    if (be_int(&d, be_get(&d, 0, "interval"), &v) && v > 0) j->interval = (int)(v > 86400 ? 86400 : v);
    if (be_int(&d, be_get(&d, 0, "min interval"), &v) && v > 0) j->minint = (int)(v > 86400 ? 86400 : v);
    if (be_int(&d, be_get(&d, 0, "complete"), &v) && v >= 0) j->seeders = (int)(v > INT_MAX ? INT_MAX : v);
    if (be_int(&d, be_get(&d, 0, "incomplete"), &v) && v >= 0) j->leechers = (int)(v > INT_MAX ? INT_MAX : v);
    int pe = be_get(&d, 0, "peers");
    baddr a;
    if ((s = be_str(&d, pe, &n))) {
        for (size_t i = 0; i + 6 <= n; i += 6) { addr_v4(&a, s + i, rd16(s + i + 4)); job_add_addr(j, &a); }
    } else if (be_type(&d, pe) == BE_LIST) {
        for (int k = d.n[pe].child; k >= 0; k = d.n[k].next) {
            char *ip = be_cstr(&d, be_get(&d, k, "ip"));
            int64_t port;
            if (ip && be_int(&d, be_get(&d, k, "port"), &port) && port > 0 && port < 65536 &&
                addr_from_text_ip(ip, (uint16_t)port, &a))
                job_add_addr(j, &a);
            free(ip);
        }
    }
    if ((s = be_str(&d, be_get(&d, 0, "peers6"), &n)))
        for (size_t i = 0; i + 18 <= n; i += 18) { addr_v6(&a, s + i, rd16(s + i + 16)); job_add_addr(j, &a); }
    j->ok = 1;
    be_free(&d);
}

static void http_announce(job *j)
{
    static const char *ev[] = { "", "completed", "started", "stopped" };
    sbuf u = { 0 };
    size_t ul = strcspn(j->url, "#");
    sb_putn(&u, j->url, ul);
    sb_putc(&u, memchr(j->url, '?', ul) ? '&' : '?');
    sb_puts(&u, "info_hash=");
    pct_encode(&u, j->ih, 20);
    sb_puts(&u, "&peer_id=");
    pct_encode(&u, j->pid, 20);
    sb_printf(&u, "&port=%d&uploaded=%lld&downloaded=%lld&left=%lld&corrupt=0&key=%08X"
                  "&numwant=%d&compact=1&no_peer_id=1&supportcrypto=0",
              CLIENT_PORT, (long long)j->up, (long long)j->down, (long long)j->left, j->key,
              j->event == 3 ? 0 : 100);
    if (j->event > 0 && j->event < 4) sb_printf(&u, "&event=%s", ev[j->event]);
    http_session *s = http_session_new(j->cfg, j->t);
    if (!s) { snprintf(j->err, sizeof j->err, "cannot start WinHTTP"); sb_free(&u); return; }
    size_t len = 0;
    char err[200];
    char *body = http_get_body(s, u.s, NULL, -1, -1, HTTP_TEXT | HTTP_SMALL, &len, NULL, 1, err, sizeof err, NULL);
    http_session_free(s);
    sb_free(&u);
    if (!body) { snprintf(j->err, sizeof j->err, "%s", err[0] ? err : "no reply"); return; }
    announce_parse(j, (const uint8_t *)body, len);
    free(body);
}

/* split "scheme://host:port/path" -> host, port */
static int url_host_port(const char *url, char *host, size_t hcap, char *port, size_t pcap)
{
    const char *p = strstr(url, "://");
    if (!p) return 0;
    p += 3;
    size_t n = strcspn(p, "/?#");
    const char *at = NULL;
    for (size_t i = 0; i < n; i++) if (p[i] == '@') at = p + i;
    if (at) { n -= (size_t)(at + 1 - p); p = at + 1; }
    const char *hs = p, *he, *ps = NULL;
    if (*p == '[') {
        const char *rb = memchr(p, ']', n);
        if (!rb) return 0;
        hs = p + 1;
        he = rb;
        if (rb + 1 < p + n && rb[1] == ':') ps = rb + 2;
    } else {
        const char *c = memchr(p, ':', n);
        he = c ? c : p + n;
        if (c) ps = c + 1;
    }
    if (he <= hs || (size_t)(he - hs) >= hcap) return 0;
    memcpy(host, hs, (size_t)(he - hs));
    host[he - hs] = 0;
    if (!ps || ps >= p + n) return 0;
    size_t pl = (size_t)(p + n - ps);
    if (pl == 0 || pl >= pcap) return 0;
    memcpy(port, ps, pl);
    port[pl] = 0;
    return 1;
}

/* wait for a datagram from `from`; >0 bytes, -1 timeout, -2 aborted */
static int udp_wait(job *j, SOCKET s, const baddr *from, uint8_t *buf, int cap, int ms)
{
    double end = now_sec() + ms / 1000.0;
    while (!job_aborted(j)) {
        double left = end - now_sec();
        if (left <= 0) return -1;
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        struct timeval tv = { 0, (long)((left > 0.25 ? 0.25 : left) * 1e6) };
        if (select(0, &rd, NULL, NULL, &tv) <= 0) continue;
        struct sockaddr_storage ss;
        memset(&ss, 0, sizeof ss);
        int sl = sizeof ss;
        int n = recvfrom(s, (char *)buf, cap, 0, (struct sockaddr *)&ss, &sl);
        if (n < 0 && WSAGetLastError() == WSAEMSGSIZE) n = cap;    /* longer than buf: use its start */
        if (n <= 0) continue;
        baddr a;
        if (!addr_from_sa(&a, (struct sockaddr *)&ss) || !addr_eq(&a, from)) continue;
        return n;
    }
    return -2;
}

/* BEP 15 */
static void udp_announce(job *j)
{
    char host[256], port[16];
    if (!url_host_port(j->url, host, sizeof host, port, sizeof port)) {
        snprintf(j->err, sizeof j->err, "invalid tracker address");
        return;
    }
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) {
        snprintf(j->err, sizeof j->err, "cannot resolve %s", host);
        return;
    }
    baddr ta;
    int have = 0;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next)       /* IPv4 first */
        if (ai->ai_family == AF_INET && addr_from_sa(&ta, ai->ai_addr)) { have = 1; break; }
    if (!have)
        for (struct addrinfo *ai = res; ai; ai = ai->ai_next)
            if (addr_from_sa(&ta, ai->ai_addr)) { have = 1; break; }
    freeaddrinfo(res);
    if (!have) { snprintf(j->err, sizeof j->err, "cannot resolve %s", host); return; }
    if (job_aborted(j)) return;

    struct sockaddr_storage ss;
    int sl = addr_to_sa(&ta, &ss);
    SOCKET s = socket(ss.ss_family, SOCK_DGRAM, IPPROTO_UDP);   /* bound by the first sendto */
    if (s == INVALID_SOCKET) { snprintf(j->err, sizeof j->err, "cannot create a UDP socket"); return; }
    BOOL off = FALSE;
    DWORD ret;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &ret, NULL, NULL);

    /* 20 bytes + 18 per IPv6 peer: 100 peers need 1820 bytes */
    uint8_t pkt[100], rb[8192];
    uint8_t conn[8] = { 0 };
    int stage = 0;
    for (; stage < 2 && !job_aborted(j); stage++) {
        int done = 0;
        uint32_t tid;                      /* the same for every attempt: a late answer still counts */
        rand_bytes(&tid, sizeof tid);
        for (int attempt = 0; attempt < 3 && !done && !job_aborted(j); attempt++) {
            int pl;
            if (stage == 0) {
                wr32(pkt, 0x417);              /* protocol id 0x41727101980 */
                wr32(pkt + 4, 0x27101980);
                wr32(pkt + 8, 0);
                wr32(pkt + 12, tid);
                pl = 16;
            } else {
                memcpy(pkt, conn, 8);
                wr32(pkt + 8, 1);
                wr32(pkt + 12, tid);
                memcpy(pkt + 16, j->ih, 20);
                memcpy(pkt + 36, j->pid, 20);
                wr64(pkt + 56, (uint64_t)j->down);
                wr64(pkt + 64, (uint64_t)j->left);
                wr64(pkt + 72, (uint64_t)j->up);
                wr32(pkt + 80, (uint32_t)j->event);
                wr32(pkt + 84, 0);
                wr32(pkt + 88, j->key);
                wr32(pkt + 92, j->event == 3 ? 0 : 100);
                wr16(pkt + 96, CLIENT_PORT);
                pl = 98;
            }
            if (sendto(s, (const char *)pkt, pl, 0, (struct sockaddr *)&ss, sl) != pl) continue;
            double end = now_sec() + 3.0 * (attempt + 1);
            while (!done) {
                int ms = (int)((end - now_sec()) * 1000);
                if (ms <= 0) break;
                int n = udp_wait(j, s, &ta, rb, (int)sizeof rb, ms);
                if (n < 0) break;
                if (n < 8 || rd32(rb + 4) != tid) continue;
                uint32_t action = rd32(rb);
                if (action == 3) {
                    char msg[160];
                    clean_text(msg, sizeof msg, rb + 8, (size_t)n - 8);
                    snprintf(j->err, sizeof j->err, "tracker says: %s", msg);
                    closesocket(s);
                    return;
                }
                if (stage == 0 && action == 0 && n >= 16) { memcpy(conn, rb + 8, 8); done = 1; }
                else if (stage == 1 && action == 1 && n >= 20) {
                    j->interval = (int)(rd32(rb + 8) > 86400 ? 86400 : rd32(rb + 8));
                    uint32_t le = rd32(rb + 12), se = rd32(rb + 16);   /* above INT_MAX: nonsense, unknown */
                    j->leechers = le > INT_MAX ? 0 : (int)le;
                    j->seeders = se > INT_MAX ? 0 : (int)se;
                    int step = ta.fam == 6 ? 18 : 6;
                    for (int i = 20; i + step <= n; i += step) {
                        baddr a;
                        if (step == 6) addr_v4(&a, rb + i, rd16(rb + i + 4));
                        else addr_v6(&a, rb + i, rd16(rb + i + 16));
                        job_add_addr(j, &a);
                    }
                    j->ok = 1;
                    done = 1;
                }
            }
        }
        if (!done) {
            if (!job_aborted(j)) snprintf(j->err, sizeof j->err, "no answer from the tracker");
            break;
        }
    }
    closesocket(s);
}

static void resolve_hosts(job *j)
{
    for (int i = 0; i < j->nhosts && !job_aborted(j); i++) {
        char host[256];
        const char *c = strrchr(j->hosts[i], ':');
        if (!c || (size_t)(c - j->hosts[i]) >= sizeof host) continue;
        memcpy(host, j->hosts[i], (size_t)(c - j->hosts[i]));
        host[c - j->hosts[i]] = 0;
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(host, c + 1, &hints, &res) != 0) continue;
        int k = 0;
        for (struct addrinfo *ai = res; ai && k < 4; ai = ai->ai_next) {
            baddr a;
            if (addr_from_sa(&a, ai->ai_addr)) { job_add_addr(j, &a); k++; }
        }
        freeaddrinfo(res);
    }
    j->ok = j->naddr > 0;
}

static volatile LONG g_final_jobs;   /* final announces still on their way */

static unsigned __stdcall job_main(void *arg)
{
    job *j = arg;
    if (j->kind == JOB_RESOLVE) resolve_hosts(j);
    else if (str_istarts(j->url, "udp://")) udp_announce(j);
    else http_announce(j);
    InterlockedExchange(&j->done, 1);
    int final = j->final;
    job_release(j);
    if (final) InterlockedDecrement(&g_final_jobs);
    return 0;
}

static job *job_new(bt *b, int kind)
{
    job *j = xcalloc(1, sizeof *j);
    j->kind = kind;
    j->ti = -1;
    j->t = task_new("");
    j->cfg = cfg_clone(b->cfg);
    /* the user's cookies / headers / referer belong to their web site */
    ul_config *c = j->cfg;
    free(c->cookie); c->cookie = NULL;
    free(c->cookie_host); c->cookie_host = NULL;
    free(c->referer); c->referer = NULL;
    for (int i = 0; i < c->nheaders; i++) free(c->headers[i]);
    c->nheaders = 0;
    for (int i = 0; i < c->ncookies; i++) {
        free(c->cookies[i].domain); free(c->cookies[i].path);
        free(c->cookies[i].name); free(c->cookies[i].value);
    }
    free(c->cookies); c->cookies = NULL; c->ncookies = 0;
    if (!c->user_agent) c->user_agent = xstrdup(UL_NAME "/" UL_VERSION);
    if (c->timeout_sec <= 0 || c->timeout_sec > 20) c->timeout_sec = 20;
    return j;
}

/* track = 1: we keep a reference and collect the result */
static int job_start(bt *b, job *j, int track)
{
    j->refs = track ? 2 : 1;
    HANDLE th = (HANDLE)_beginthreadex(NULL, 0, job_main, j, 0, NULL);
    if (!th) {
        j->refs = 1;
        job_release(j);
        return 0;
    }
    CloseHandle(th);
    if (track) b->jobs[b->njobs++] = j;
    return 1;
}

/* ================================================================== */
/* peer addresses                                                      */
/* ================================================================== */
static cand *cand_find(bt *b, const baddr *a)
{
    for (int i = 0; i < b->nc; i++) if (addr_eq(&b->c[i].a, a)) return &b->c[i];
    return NULL;
}

static int cand_add(bt *b, const baddr *a, int src)
{
    if (!addr_usable(a)) return 0;
    cand *c = cand_find(b, a);
    if (c) {
        c->src |= (uint8_t)src;
        if (c->st == CS_IDLE && c->fails < 4 && c->retry > b->now + 30) c->retry = b->now + 30;
        return 0;
    }
    int slot = -1;
    if (b->nc < MAX_CANDS) slot = b->nc++;
    else
        for (int i = 0; i < b->nc; i++)       /* reuse a dead address */
            if (b->c[i].st == CS_IDLE && b->c[i].fails >= 4) { slot = i; break; }
    if (slot < 0) return 0;
    c = &b->c[slot];
    memset(c, 0, sizeof *c);
    c->a = *a;
    c->src = (uint8_t)src;
    return 1;
}

static int connectable(bt *b)
{
    int n = 0;
    for (int i = 0; i < b->nc; i++)
        if (b->c[i].st == CS_IDLE && b->c[i].retry <= b->now && b->c[i].fails < 4) n++;
    return n;
}

/* ================================================================== */
/* peer messages                                                       */
/* ================================================================== */
static void peer_kill(peer *p, const char *why)
{
    if (p->dead) return;
    p->dead = 1;
    snprintf(p->why, sizeof p->why, "%s", why);
}

static void wb_put(peer *p, const void *d, size_t n)
{
    if (p->wpos && p->wlen + n > p->wcap) {
        memmove(p->wb, p->wb + p->wpos, p->wlen - p->wpos);
        p->wlen -= p->wpos;
        p->wpos = 0;
    }
    if (p->wlen + n > p->wcap) {
        size_t nc = p->wcap ? p->wcap : 16384;
        while (nc < p->wlen + n) nc *= 2;
        p->wb = xrealloc(p->wb, nc);
        p->wcap = nc;
    }
    memcpy(p->wb + p->wlen, d, n);
    p->wlen += n;
    if (p->wlen - p->wpos > (8u << 20)) peer_kill(p, "not reading");
}

static void put_msg(bt *b, peer *p, int id, const void *a, size_t an, const void *c, size_t cn)
{
    uint8_t h[5];
    wr32(h, (uint32_t)(1 + an + cn));
    h[4] = (uint8_t)id;
    wb_put(p, h, 5);
    if (an) wb_put(p, a, an);
    if (cn) wb_put(p, c, cn);
    p->t_send = b->now;
}

static void send_simple(bt *b, peer *p, int id) { put_msg(b, p, id, NULL, 0, NULL, 0); }

static void send_have(bt *b, peer *p, int idx)
{
    uint8_t x[4];
    wr32(x, (uint32_t)idx);
    put_msg(b, p, 4, x, 4, NULL, 0);
}

static void send_req(bt *b, peer *p, int id, int idx, int begin, int len)
{
    uint8_t x[12];
    wr32(x, (uint32_t)idx);
    wr32(x + 4, (uint32_t)begin);
    wr32(x + 8, (uint32_t)len);
    put_msg(b, p, id, x, 12, NULL, 0);
}

static void send_ext(bt *b, peer *p, int eid, const void *d, size_t n, const void *tail, size_t tn)
{
    uint8_t h[6];
    wr32(h, (uint32_t)(2 + n + tn));
    h[4] = 20;
    h[5] = (uint8_t)eid;
    wb_put(p, h, 6);
    if (n) wb_put(p, d, n);
    if (tn) wb_put(p, tail, tn);
    p->t_send = b->now;
}

static void send_ext_handshake(bt *b, peer *p)
{
    sbuf s = { 0 };
    sb_puts(&s, "d1:md11:ut_metadatai1e");
    if (!b->priv) sb_puts(&s, "6:ut_pexi2e");
    sb_puts(&s, "e");
    if (b->have_meta) sb_printf(&s, "13:metadata_sizei%zue", b->m.info_len);
    sb_printf(&s, "4:reqqi%de1:v", 250);
    be_put_str(&s, UL_NAME " " UL_VERSION, strlen(UL_NAME " " UL_VERSION));
    sb_putc(&s, 'e');
    send_ext(b, p, 0, s.s, s.len, NULL, 0);
    sb_free(&s);
}

static void ban_addr(bt *b, const baddr *a, const char *why)
{
    cand *c = cand_find(b, a);
    if (c && c->st == CS_BANNED) return;
    if (c) c->st = CS_BANNED;
    b->banned++;
    for (int i = 0; i < b->npeers; i++)
        if (addr_eq(&b->p[i]->a, a)) peer_kill(b->p[i], why);
    char as[80];
    tlog(b->t, LOG_DIM, "  banned peer %s: %s", addr_str(a, as, sizeof as), why);
}

/* ================================================================== */
/* pieces and requests                                                 */
/* ================================================================== */
static int peer_has(const peer *p, int i) { return p->bf && bit_get(p->bf, i); }

static int act_start(bt *b, int idx)
{
    apiece *a = xcalloc(1, sizeof *a);
    a->idx = idx;
    a->len = piece_len(b, idx);
    a->nblk = (a->len + BLOCK - 1) / BLOCK;
    a->nfree = a->nblk;
    a->st = xcalloc((size_t)a->nblk, 1);
    a->dup = xcalloc((size_t)a->nblk, 1);
    a->from = xmalloc((size_t)a->nblk);
    memset(a->from, 255, (size_t)a->nblk);
    a->data = xmalloc((size_t)a->len);
    b->act[b->nact] = a;
    b->slot[idx] = b->nact;
    return b->nact++;
}

static void act_remove(bt *b, int s)
{
    apiece *a = b->act[s];
    b->slot[a->idx] = -1;
    free(a->st); free(a->dup); free(a->from); free(a->data); free(a->smart);
    free(a);
    b->nact--;
    if (s != b->nact) {
        b->act[s] = b->act[b->nact];
        b->slot[b->act[s]->idx] = s;
    }
}

static int has_req(const peer *p, int piece, int begin)
{
    for (int r = 0; r < p->nreq; r++) if (p->req[r].piece == piece && p->req[r].begin == begin) return 1;
    return 0;
}

static void drop_req(peer *p, int r)
{
    memmove(&p->req[r], &p->req[r + 1], (size_t)(p->nreq - r - 1) * sizeof(preq));
    p->nreq--;
}

/* One request of ours is no longer in flight. */
static void unrequest(bt *b, int piece, int begin)
{
    int s = b->have_meta ? b->slot[piece] : -1;
    if (s < 0) return;
    apiece *a = b->act[s];
    int j = begin / BLOCK;
    if (a->dup[j]) a->dup[j]--;
    if (a->dup[j] == 0 && a->st[j] == 1) { a->st[j] = 0; a->nfree++; }
}

/* A request we took back (cancelled, or dropped on a choke): the peer may
   have sent the block already, so when it arrives it was not unasked. */
static void cxl_add(peer *p, int piece, int begin)
{
    p->cxl[p->cxl_pos].piece = piece;
    p->cxl[p->cxl_pos].begin = begin;
    p->cxl_pos = (p->cxl_pos + 1) % CXL_N;
    if (p->ncxl < CXL_N) p->ncxl++;
}

static int cxl_take(peer *p, uint32_t piece, uint32_t begin)
{
    for (int k = 0; k < p->ncxl; k++)
        if (p->cxl[k].piece >= 0 && (uint32_t)p->cxl[k].piece == piece && (uint32_t)p->cxl[k].begin == begin) {
            p->cxl[k].piece = -1;
            return 1;
        }
    return 0;
}

static void release_requests(bt *b, peer *p, int cancel)
{
    for (int r = 0; r < p->nreq; r++) {
        unrequest(b, p->req[r].piece, p->req[r].begin);
        if (cancel) send_req(b, p, 8, p->req[r].piece, p->req[r].begin, p->req[r].len);
        cxl_add(p, p->req[r].piece, p->req[r].begin);
    }
    p->nreq = 0;
}

/* Choose the next block to ask p for: blocks of pieces already started,
   then the rarest piece nobody started, then (endgame) blocks others are
   still fetching. Returns the act[] slot, block in *blk; -1 = nothing. */
static int pick_block(bt *b, peer *p, int *blk)
{
    for (int k = 0; k < b->nact; k++) {
        apiece *a = b->act[k];
        if (a->nfree == 0 || !peer_has(p, a->idx)) continue;
        for (int j = 0; j < a->nblk; j++)
            if (a->st[j] == 0) { *blk = j; return k; }
    }
    int live = b->nact;
    if (live >= b->max_act)                         /* pieces nobody here has do not count */
        for (int k = 0; k < b->nact; k++) if (b->avail[b->act[k]->idx] == 0) live--;
    if (live < b->max_act && (int64_t)b->nact * b->m.plen < 2 * MEM_BUDGET && b->nact < MAX_ACTIVE) {
        int n = b->m.np, best = -1;
        unsigned bestav = UINT_MAX;
        int start = (int)(bt_rand(b) % (uint32_t)n);
        for (int k = 0; k < n; k++) {
            int i = start + k;
            if (i >= n) i -= n;
            if (bit_get(b->have, i) || b->slot[i] >= 0 || !peer_has(p, i)) continue;
            if (b->avail[i] < bestav) {
                bestav = b->avail[i];
                best = i;
                if (bestav <= 1) break;
            }
        }
        if (best >= 0) { *blk = 0; return act_start(b, best); }
    }
    if (b->nhave + b->nact >= b->m.np) {           /* endgame: everything is started */
        b->endgame = 1;
        int bk = -1, bj = -1, bd = 3;
        for (int k = 0; k < b->nact; k++) {
            apiece *a = b->act[k];
            if (!peer_has(p, a->idx)) continue;
            for (int j = 0; j < a->nblk; j++)
                if (a->st[j] == 1 && a->dup[j] < bd && !has_req(p, a->idx, j * BLOCK)) {
                    bk = k; bj = j; bd = a->dup[j];
                    if (bd <= 1) break;
                }
            if (bk >= 0 && bd <= 1) break;
        }
        if (bk >= 0) { *blk = bj; return bk; }
    }
    return -1;
}

static int pipe_target(const peer *p)
{
    int t = 4 + (int)(p->rate * 1.5 / BLOCK);
    if (t < MIN_PIPE) t = MIN_PIPE;
    if (t > MAX_PIPE) t = MAX_PIPE;
    if (p->reqq > 0 && t > p->reqq) t = p->reqq;
    return t;
}

static void fill_requests(bt *b, peer *p)
{
    if (p->st != PS_ACTIVE || p->dead || p->choked || !p->am_interested || !b->have_meta || p->snubbed) return;
    int target = pipe_target(p);
    while (p->nreq < target) {
        int j, k = pick_block(b, p, &j);
        if (k < 0) break;
        apiece *a = b->act[k];
        int begin = j * BLOCK, len = a->len - begin < BLOCK ? a->len - begin : BLOCK;
        if (a->st[j] == 0) { a->st[j] = 1; a->nfree--; }
        if (a->dup[j] < 255) a->dup[j]++;
        preq *r = &p->req[p->nreq++];
        r->piece = a->idx;
        r->begin = begin;
        r->len = len;
        r->t = b->now;
        send_req(b, p, 6, a->idx, begin, len);
    }
}

static void update_interest(bt *b, peer *p)
{
    if (!b->have_meta || p->st != PS_ACTIVE || p->dead || !p->bf) return;
    int want = 0;
    if (p->nhave > 0 && b->nhave < b->m.np) {
        int nb = nbytes_of(b->m.np);
        for (int i = 0; i < nb && !want; i++) if (p->bf[i] & ~b->have[i]) want = 1;
    }
    if (want != p->am_interested) {
        p->am_interested = (uint8_t)want;
        send_simple(b, p, want ? 2 : 3);
    }
}

static void digest8(const uint8_t *d, size_t n, uint8_t out[8])
{
    uint8_t h[20];
    sha1(d, n, h);
    memcpy(out, h, 8);
}

static int blk_len(const apiece *a, int j) { return a->len - j * BLOCK < BLOCK ? a->len - j * BLOCK : BLOCK; }

static void piece_failed(bt *b, int s)
{
    apiece *a = b->act[s];
    b->hashfails++;
    b->wasted += a->len;
    /* remember who sent which block: once the piece verifies, whoever sent
       a block that differs from the good one is the culprit */
    if (!a->smart) a->smart = xcalloc((size_t)a->nblk, sizeof *a->smart);
    for (int j = 0; j < a->nblk; j++) {
        a->smart[j].ok = 0;
        if (a->from[j] < a->nwho) {
            a->smart[j].a = a->who[a->from[j]];
            digest8(a->data + (size_t)j * BLOCK, (size_t)blk_len(a, j), a->smart[j].h);
            a->smart[j].ok = 1;
        }
    }
    if (b->hashfails <= 5)
        tlog(b->t, LOG_WARN, "  piece %d failed the SHA-1 check - discarded, downloading it again", a->idx);
    else if (b->hashfails % 25 == 0)
        tlog(b->t, LOG_DIM, "  %d pieces failed the SHA-1 check so far", b->hashfails);
    int nwho = a->nwho;
    baddr who[16];
    memcpy(who, a->who, sizeof who);
    for (int j = 0; j < a->nblk; j++) {
        a->st[j] = a->dup[j] ? 1 : 0;
        a->from[j] = 255;
    }
    a->nrecv = 0;
    a->nwho = 0;
    a->nfree = 0;
    for (int j = 0; j < a->nblk; j++) if (a->st[j] == 0) a->nfree++;
    for (int k = 0; k < nwho; k++) {
        cand *c = cand_find(b, &who[k]);
        int fails = c ? ++c->hashfails : 1;
        if (nwho == 1) ban_addr(b, &who[k], "sent a corrupt piece");
        else if (fails >= 8) ban_addr(b, &who[k], "involved in too many corrupt pieces");
    }
}

static void broadcast_have(bt *b, int idx)
{
    for (int i = 0; i < b->npeers; i++) {
        peer *q = b->p[i];
        if (q->st == PS_ACTIVE && !q->dead && !peer_has(q, idx)) send_have(b, q, idx);
    }
}

static void piece_done(bt *b, int s)
{
    apiece *a = b->act[s];
    uint8_t h[20];
    if (!sha1(a->data, (size_t)a->len, h)) { bt_fail(b, "SHA-1 is not available on this system"); return; }
    if (memcmp(h, b->m.hashes + (size_t)a->idx * 20, 20) != 0) { piece_failed(b, s); return; }
    if (a->smart) {
        for (int j = 0; j < a->nblk; j++) {
            if (!a->smart[j].ok) continue;
            uint8_t d8[8];
            digest8(a->data + (size_t)j * BLOCK, (size_t)blk_len(a, j), d8);
            if (memcmp(d8, a->smart[j].h, 8) != 0) ban_addr(b, &a->smart[j].a, "sent corrupt data");
        }
    }
    if (stor_io(&b->m, (int64_t)a->idx * b->m.plen, a->data, a->len, 1) != 0) {
        bt_fail(b, "%s", b->m.ioerr);
        return;
    }
    int idx = a->idx;
    bit_set(b->have, idx);
    b->nhave++;
    b->have_bytes += a->len;
    b->dirty = 1;
    b->t_last_data = b->now;
    act_remove(b, s);
    broadcast_have(b, idx);
}

static int who_index(apiece *a, const baddr *x)
{
    for (int k = 0; k < a->nwho; k++) if (addr_eq(&a->who[k], x)) return k;
    if (a->nwho >= 16) return 255;
    a->who[a->nwho] = *x;
    return a->nwho++;
}

static void on_block(bt *b, peer *p, uint32_t idx, uint32_t begin, const uint8_t *data, uint32_t len)
{
    int r = -1;
    for (int k = 0; k < p->nreq; k++)
        if ((uint32_t)p->req[k].piece == idx && (uint32_t)p->req[k].begin == begin) { r = k; break; }
    if (r >= 0) drop_req(p, r);
    p->t_block = b->now;
    p->got += len;
    p->got_sec += len;
    b->down += len;
    if (r < 0 && !cxl_take(p, idx, begin)) {
        p->unrequested += len;
        if (p->unrequested > (8 << 20)) { peer_kill(p, "sends data nobody asked for"); return; }
    }
    if (!b->have_meta || idx >= (uint32_t)b->m.np) { b->wasted += len; return; }
    int s = b->slot[idx];
    if (s < 0) { b->wasted += len; return; }
    apiece *a = b->act[s];
    if (begin % BLOCK || begin >= (uint32_t)a->len) { b->wasted += len; return; }
    int j = (int)(begin / BLOCK);
    if ((int)len != blk_len(a, j)) {
        b->wasted += len;
        if (r >= 0) {                    /* our request is gone: the block must be free to ask again */
            unrequest(b, (int)idx, (int)begin);
            peer_kill(p, "sent a block of the wrong size");
        }
        return;
    }
    if (r >= 0 && a->dup[j]) a->dup[j]--;
    if (a->st[j] == 2) { b->wasted += len; return; }       /* endgame duplicate */
    if (a->st[j] == 0) a->nfree--;
    memcpy(a->data + begin, data, len);
    a->st[j] = 2;
    a->nrecv++;
    a->from[j] = (uint8_t)who_index(a, &p->a);
    if (a->dup[j] > 0) {                 /* endgame: cancel the copies still in flight */
        for (int i = 0; i < b->npeers; i++) {
            peer *q = b->p[i];
            if (q == p) continue;
            for (int k = 0; k < q->nreq; k++)
                if ((uint32_t)q->req[k].piece == idx && (uint32_t)q->req[k].begin == begin) {
                    if (!q->dead) send_req(b, q, 8, (int)idx, (int)begin, (int)len);
                    drop_req(q, k);
                    cxl_add(q, (int)idx, (int)begin);
                    break;
                }
        }
        a->dup[j] = 0;
    }
    if (a->nrecv == a->nblk) piece_done(b, s);
}

/* ================================================================== */
/* metadata from peers (BEP 9)                                         */
/* ================================================================== */
static int setup_meta(bt *b, const char *state_name, const uint8_t *bits, size_t bits_len);
static void peer_meta_ready(bt *b, peer *p);

static void md_reset(bt *b)
{
    free(b->md); free(b->md_st); free(b->md_from);
    b->md = b->md_st = NULL;
    b->md_from = NULL;
    b->md_size = 0;
    b->md_n = 0;
    b->md_src_set = 0;
    for (int i = 0; i < b->npeers; i++) b->p[i]->md_piece = -1;
}

/* One peer at a time (after a bad copy that came from several): p may ask
   unless another usable peer is the one we are getting it from. */
static int md_take_source(bt *b, peer *p)
{
    if (b->md_src_set && !addr_eq(&b->md_src, &p->a))
        for (int i = 0; i < b->npeers; i++) {
            peer *q = b->p[i];
            if (q != p && addr_eq(&q->a, &b->md_src) && q->st == PS_ACTIVE && !q->dead && q->ut_md &&
                q->md_bad < 3 && q->md_size == b->md_size)
                return 0;
        }
    b->md_src = p->a;
    b->md_src_set = 1;
    return 1;
}

static void md_request(bt *b, peer *p)
{
    if (b->have_meta || p->st != PS_ACTIVE || p->dead || !p->ut_md || p->md_size <= 0 || p->md_piece >= 0 ||
        p->md_bad >= 3)
        return;
    if (b->md_size == 0) {
        /* go with the size most peers agree on (peers that failed us count less) */
        int64_t best = p->md_size;
        int bestv = -1;
        for (int i = 0; i < b->npeers; i++) {
            peer *q = b->p[i];
            if (q->st != PS_ACTIVE || q->dead || !q->ut_md || q->md_size <= 0 || q->md_bad >= 3) continue;
            int v = 0;
            for (int k = 0; k < b->npeers; k++) {
                peer *r = b->p[k];
                if (r->st == PS_ACTIVE && !r->dead && r->ut_md && r->md_size == q->md_size && r->md_bad < 3) v += 3 - r->md_bad;
            }
            if (v > bestv || (v == bestv && q == p)) { bestv = v; best = q->md_size; }
        }
        b->md_size = best;
        b->md_n = (int)((b->md_size + MD_PIECE - 1) / MD_PIECE);
        b->md = xmalloc((size_t)b->md_size);
        b->md_st = xcalloc((size_t)b->md_n, 1);
        b->md_from = xcalloc((size_t)b->md_n, sizeof(baddr));
        b->t_md = b->now;
    }
    if (p->md_size != b->md_size) return;
    int start = (int)(bt_rand(b) % (uint32_t)b->md_n), pick = -1;
    for (int k = 0; k < b->md_n; k++) {
        int i = (start + k) % b->md_n;
        if (b->md_st[i] == 0) { pick = i; break; }
    }
    if (pick < 0) return;
    if (b->md_solo && !md_take_source(b, p)) return;
    b->md_st[pick] = 1;
    p->md_piece = pick;
    p->md_t = b->now;
    char msg[64];
    int n = snprintf(msg, sizeof msg, "d8:msg_typei0e5:piecei%dee", pick);
    send_ext(b, p, p->ut_md, msg, (size_t)n, NULL, 0);
}

static void md_complete(bt *b)
{
    uint8_t h[20];
    sha1(b->md, (size_t)b->md_size, h);
    if (memcmp(h, b->ih, 20) != 0) {
        b->md_fails++;
        /* Only a copy that came from one peer alone proves that peer bad. A
           mixed one cannot tell the liar from the honest peers, so nobody is
           banned and the next copies come from one peer at a time. */
        int nsrc = b->md_n > 0;
        for (int i = 1; i < b->md_n && nsrc < 2; i++)
            if (!addr_eq(&b->md_from[i], &b->md_from[0])) nsrc = 2;
        tlog(b->t, LOG_WARN, "  the metadata received from peers does not match the info-hash - trying again%s",
             nsrc > 1 && !b->md_solo ? ", one peer at a time" : "");
        if (nsrc == 1) ban_addr(b, &b->md_from[0], "sent bad metadata");
        else b->md_solo = 1;
        md_reset(b);
        if (b->md_fails >= 8) bt_fail(b, "could not get valid metadata for this torrent from its peers");
        return;
    }
    char err[300];
    meta m;
    if (meta_parse(&m, b->md, (size_t)b->md_size, err, sizeof err) != 0) {
        bt_fail(b, "%s", err);
        md_reset(b);
        return;
    }
    char sz[32];
    tlog(b->t, LOG_INFO, "  metadata received (%s)", fmt_size(b->md_size, sz, sizeof sz));
    md_reset(b);
    b->m = m;
    b->have_meta = 1;
    /* The metadata stays known even when the setup fails or is paused: the
       peers switch to it (so they all have a piece map), nothing asks for
       metadata again, and stopping saves it with the progress. */
    setup_meta(b, NULL, NULL, 0);
    for (int i = 0; i < b->npeers; i++)
        if (b->p[i]->st == PS_ACTIVE && !b->p[i]->dead) peer_meta_ready(b, b->p[i]);
}

static void on_metadata_msg(bt *b, peer *p, const uint8_t *d, size_t n)
{
    bdoc x;
    size_t used = 0;
    if (be_decode(&x, d, n, 64, &used) < 0 || x.n[0].type != BE_DICT) { be_free(&x); return; }
    int64_t type = -1, piece = -1, total = -1;
    be_int(&x, be_get(&x, 0, "msg_type"), &type);
    be_int(&x, be_get(&x, 0, "piece"), &piece);
    int has_total = be_int(&x, be_get(&x, 0, "total_size"), &total);
    be_free(&x);
    if (type == 0) {                                  /* a peer asks us */
        if (!p->ut_md) return;
        char hdr[96];
        if (b->have_meta && piece >= 0 && piece < (1 << 20) && piece * MD_PIECE < (int64_t)b->m.info_len) {
            size_t off = (size_t)piece * MD_PIECE, k = b->m.info_len - off;
            if (k > MD_PIECE) k = MD_PIECE;
            int hn = snprintf(hdr, sizeof hdr, "d8:msg_typei1e5:piecei%de10:total_sizei%zuee", (int)piece, b->m.info_len);
            send_ext(b, p, p->ut_md, hdr, (size_t)hn, b->m.info + off, k);
        } else {
            int hn = snprintf(hdr, sizeof hdr, "d8:msg_typei2e5:piecei%dee", (int)(piece < 0 ? 0 : piece > 1 << 20 ? 0 : piece));
            send_ext(b, p, p->ut_md, hdr, (size_t)hn, NULL, 0);
        }
        return;
    }
    if (b->have_meta || b->md_size == 0 || piece < 0 || piece >= b->md_n) return;
    int i = (int)piece;
    if (type == 2) {                                  /* reject */
        if (p->md_piece == i) { b->md_st[i] = 0; p->md_piece = -1; }
        p->md_bad++;
        return;
    }
    if (type != 1) return;
    if (p->md_piece != i) return;                     /* only what we asked this peer for */
    /* the data must have the size of the piece; total_size, when given (it
       is not needed: the size we went with decides), must be that size */
    int64_t expect = i == b->md_n - 1 ? b->md_size - (int64_t)i * MD_PIECE : MD_PIECE;
    if ((int64_t)(n - used) != expect || (has_total && total != b->md_size)) {
        b->md_st[i] = 0;
        p->md_piece = -1;
        p->md_bad++;
        return;
    }
    p->md_piece = -1;
    if (b->md_st[i] == 2) return;
    memcpy(b->md + (size_t)i * MD_PIECE, d + used, (size_t)expect);
    b->md_st[i] = 2;
    b->t_md = b->now;
    b->md_from[i] = p->a;
    for (int k = 0; k < b->md_n; k++) if (b->md_st[k] != 2) { md_request(b, p); return; }
    md_complete(b);
}

/* ================================================================== */
/* incoming messages                                                   */
/* ================================================================== */
static void peer_meta_ready(bt *b, peer *p)
{
    int np = b->m.np, nb = nbytes_of(np);
    if (p->bf) {
        for (int i = 0; i < np; i++) if (bit_get(p->bf, i)) b->avail[i]--;
        free(p->bf);
    }
    p->bf = xcalloc((size_t)nb, 1);
    p->nhave = 0;
    if (p->have_all) memset(p->bf, 0xFF, (size_t)nb);
    else if (p->pre) {
        if (p->pre_isbf && p->pre_len != (size_t)nb) { peer_kill(p, "invalid bitfield"); return; }
        if (!p->pre_isbf) {
            for (size_t i = (size_t)nb; i < p->pre_len; i++)
                if (p->pre[i]) { peer_kill(p, "invalid have message"); return; }
            if ((np & 7) && p->pre_len >= (size_t)nb && (p->pre[nb - 1] & (0xFF >> (np & 7)))) {
                peer_kill(p, "invalid have message");
                return;
            }
        }
        memcpy(p->bf, p->pre, p->pre_len < (size_t)nb ? p->pre_len : (size_t)nb);
    }
    clear_spare_bits(p->bf, np);
    free(p->pre);
    p->pre = NULL;
    p->pre_len = 0;
    for (int i = 0; i < nb; i++) {
        if (!p->bf[i]) continue;
        for (int k = 0; k < 8; k++) {
            int idx = i * 8 + k;
            if (idx < np && bit_get(p->bf, idx)) { b->avail[idx]++; p->nhave++; }
        }
    }
    p->seed = p->nhave == np;
    update_interest(b, p);
}

static void on_have(bt *b, peer *p, uint32_t idx)
{
    if (!b->have_meta) {
        if (idx >= MAX_PRE_BITS) { peer_kill(p, "invalid have message"); return; }
        size_t need = idx / 8 + 1;
        if (p->pre_isbf) {
            if (need > p->pre_len) { peer_kill(p, "invalid have message"); return; }
        } else if (need > p->pre_len) {
            p->pre = xrealloc(p->pre, need);
            memset(p->pre + p->pre_len, 0, need - p->pre_len);
            p->pre_len = need;
        }
        bit_set(p->pre, (int)idx);
        return;
    }
    if (idx >= (uint32_t)b->m.np) { peer_kill(p, "invalid have message"); return; }
    if (bit_get(p->bf, (int)idx)) return;
    bit_set(p->bf, (int)idx);
    p->nhave++;
    b->avail[idx]++;
    p->seed = p->nhave == b->m.np;
    if (!p->am_interested && !bit_get(b->have, (int)idx)) {
        p->am_interested = 1;
        send_simple(b, p, 2);
    }
}

static void on_bitfield(bt *b, peer *p, const uint8_t *d, uint32_t n)
{
    if (!b->have_meta) {
        if (n > MAX_PRE_BITS / 8) { peer_kill(p, "invalid bitfield"); return; }
        free(p->pre);
        p->pre = xmalloc(n ? n : 1);
        memcpy(p->pre, d, n);
        p->pre_len = n;
        p->pre_isbf = 1;
        return;
    }
    int np = b->m.np;
    if (n != (uint32_t)nbytes_of(np)) { peer_kill(p, "invalid bitfield"); return; }
    for (int i = 0; i < np; i++) if (bit_get(p->bf, i)) b->avail[i]--;
    memcpy(p->bf, d, n);
    clear_spare_bits(p->bf, np);
    p->nhave = 0;
    for (int i = 0; i < np; i++) if (bit_get(p->bf, i)) { b->avail[i]++; p->nhave++; }
    p->seed = p->nhave == np;
    update_interest(b, p);
}

static void on_have_all(bt *b, peer *p)
{
    p->have_all = 1;
    if (!b->have_meta) return;
    int np = b->m.np;
    for (int i = 0; i < np; i++) if (!bit_get(p->bf, i)) { bit_set(p->bf, i); b->avail[i]++; }
    p->nhave = np;
    p->seed = 1;
    update_interest(b, p);
}

static void on_request(bt *b, peer *p, uint32_t idx, uint32_t begin, uint32_t len)
{
    if (len > 131072) { peer_kill(p, "oversized request"); return; }
    if (!b->have_meta || idx >= (uint32_t)b->m.np || !bit_get(b->have, (int)idx) || p->am_choking) return;
    uint32_t pl = (uint32_t)piece_len(b, (int)idx);
    if (len == 0 || begin >= pl || len > pl - begin || p->nup >= MAX_UPQ) return;
    preq *r = &p->upq[p->nup++];
    r->piece = (int)idx;
    r->begin = (int)begin;
    r->len = (int)len;
    r->t = b->now;
}

static void on_cancel(peer *p, uint32_t idx, uint32_t begin, uint32_t len)
{
    for (int k = 0; k < p->nup; k++)
        if ((uint32_t)p->upq[k].piece == idx && (uint32_t)p->upq[k].begin == begin && (uint32_t)p->upq[k].len == len) {
            memmove(&p->upq[k], &p->upq[k + 1], (size_t)(p->nup - k - 1) * sizeof(preq));
            p->nup--;
            return;
        }
}

static void on_pex(bt *b, peer *p, const uint8_t *d, size_t n)
{
    if (b->priv) return;
    bdoc x;
    if (be_decode(&x, d, n, 64, NULL) < 0 || x.n[0].type != BE_DICT) { be_free(&x); return; }
    size_t l;
    const uint8_t *s;
    baddr a;
    int added = 0;
    if ((s = be_str(&x, be_get(&x, 0, "added"), &l)))
        for (size_t i = 0; i + 6 <= l && added < 200; i += 6, added++) { addr_v4(&a, s + i, rd16(s + i + 4)); cand_add(b, &a, SRC_PEX); }
    if ((s = be_str(&x, be_get(&x, 0, "added6"), &l)))
        for (size_t i = 0; i + 18 <= l && added < 200; i += 18, added++) { addr_v6(&a, s + i, rd16(s + i + 16)); cand_add(b, &a, SRC_PEX); }
    be_free(&x);
    (void)p;
}

static void on_ext_handshake(bt *b, peer *p, const uint8_t *d, size_t n)
{
    bdoc x;
    if (be_decode(&x, d, n, 2000, NULL) < 0 || x.n[0].type != BE_DICT) { be_free(&x); return; }
    int m = be_get(&x, 0, "m");
    int64_t v;
    if (be_type(&x, m) == BE_DICT) {
        if (be_int(&x, be_get(&x, m, "ut_metadata"), &v) && v >= 0 && v <= 255) p->ut_md = (int)v;
        if (be_int(&x, be_get(&x, m, "ut_pex"), &v) && v >= 0 && v <= 255) p->ut_pex = (int)v;
    }
    if (be_int(&x, be_get(&x, 0, "metadata_size"), &v) && v > 0 && v <= MD_MAX) p->md_size = v;
    if (be_int(&x, be_get(&x, 0, "reqq"), &v) && v > 0) p->reqq = v > 1000 ? 1000 : (int)v;
    be_free(&x);
    if (!b->have_meta) md_request(b, p);
}

static void on_ext(bt *b, peer *p, int eid, const uint8_t *d, size_t n)
{
    if (eid == 0) on_ext_handshake(b, p, d, n);
    else if (eid == EXT_UT_METADATA) on_metadata_msg(b, p, d, n);
    else if (eid == EXT_UT_PEX) on_pex(b, p, d, n);
}

static void handle_msg(bt *b, peer *p, const uint8_t *m, uint32_t len)
{
    if (len == 0) return;                                   /* keep-alive */
    uint8_t id = m[0];
    const uint8_t *pl = m + 1;
    uint32_t n = len - 1;
    switch (id) {
    case 0:                                                 /* choke */
        if (n) goto bad;
        p->choked = 1;
        release_requests(b, p, 0);
        break;
    case 1:                                                 /* unchoke */
        if (n) goto bad;
        p->choked = 0;
        p->t_block = b->now;
        break;
    case 2: if (n) goto bad; p->interested = 1; break;
    case 3: if (n) goto bad; p->interested = 0; p->nup = 0; break;
    case 4: if (n != 4) goto bad; on_have(b, p, rd32(pl)); break;
    case 5: on_bitfield(b, p, pl, n); break;
    case 6: if (n != 12) goto bad; on_request(b, p, rd32(pl), rd32(pl + 4), rd32(pl + 8)); break;
    case 7: if (n < 8) goto bad; on_block(b, p, rd32(pl), rd32(pl + 4), pl + 8, n - 8); break;
    case 8: if (n != 12) goto bad; on_cancel(p, rd32(pl), rd32(pl + 4), rd32(pl + 8)); break;
    case 9: break;                                          /* DHT port */
    case 0x0E: if (n) goto bad; on_have_all(b, p); break;   /* fast extension, tolerated */
    case 0x0F: if (n) goto bad; break;
    case 0x10:                                              /* reject request */
        if (n != 12) goto bad;
        for (int k = 0; k < p->nreq; k++)
            if ((uint32_t)p->req[k].piece == rd32(pl) && (uint32_t)p->req[k].begin == rd32(pl + 4)) {
                unrequest(b, p->req[k].piece, p->req[k].begin);
                drop_req(p, k);
                break;
            }
        break;
    case 20: if (n < 1) goto bad; on_ext(b, p, pl[0], pl + 1, n - 1); break;
    default: break;
    }
    return;
bad:
    peer_kill(p, "protocol violation");
}

/* bytes per second this torrent may download: its own -l limit and the
   window's limiter (#2), whichever is lower; 0 = unlimited. The limiter is
   applied through this non-blocking token bucket (the peer loop must not
   sleep), so a torrent keeps to it on its own. */
static int64_t dl_rate(const bt *b)
{
    int64_t own = b->cfg->speed_limit, all = speed_limit_get();
    if (own <= 0) return all > 0 ? all : 0;
    return all > 0 && all < own ? all : own;
}

static void peer_ready(bt *b, peer *p)        /* their handshake arrived */
{
    p->st = PS_ACTIVE;
    p->t_start = b->now;
    b->c[p->ci].good = 1;
    b->c[p->ci].fails = 0;
    b->ever_peer = 1;
    if (b->have_meta) {
        int nb = nbytes_of(b->m.np);
        p->bf = xcalloc((size_t)nb, 1);
        if (b->nhave > 0) put_msg(b, p, 5, b->have, (size_t)nb, NULL, 0);
    }
    if (p->ext) send_ext_handshake(b, p);
}

static void peer_parse(bt *b, peer *p)
{
    size_t pos = 0;
    if (p->st == PS_HANDSHAKE) {
        if (p->rlen < 68) return;
        const uint8_t *h = p->rb;
        if (h[0] != 19 || memcmp(h + 1, "BitTorrent protocol", 19) != 0) { peer_kill(p, "not a BitTorrent peer"); return; }
        if (memcmp(h + 28, b->ih, 20) != 0) { peer_kill(p, "wrong torrent"); return; }
        if (memcmp(h + 48, b->pid, 20) == 0) { peer_kill(p, "connected to ourselves"); b->c[p->ci].st = CS_BANNED; return; }
        for (int i = 0; i < b->npeers; i++) {
            peer *q = b->p[i];
            if (q != p && q->st == PS_ACTIVE && !q->dead && memcmp(q->id, h + 48, 20) == 0) {
                peer_kill(p, "duplicate connection");
                return;
            }
        }
        memcpy(p->id, h + 48, 20);
        p->ext = (h[25] & 0x10) != 0;
        pos = 68;
        peer_ready(b, p);
    }
    while (!p->dead && p->rlen - pos >= 4) {
        uint32_t len = rd32(p->rb + pos);
        if (len > MAX_MSG) { peer_kill(p, "message too large"); break; }
        if (p->rlen - pos - 4 < len) break;
        handle_msg(b, p, p->rb + pos + 4, len);
        pos += 4 + (size_t)len;
    }
    if (pos) {
        memmove(p->rb, p->rb + pos, p->rlen - pos);
        p->rlen -= pos;
    }
}

/* ================================================================== */
/* sockets                                                             */
/* ================================================================== */
static peer *peer_connect(bt *b, int ci)
{
    cand *c = &b->c[ci];
    struct sockaddr_storage ss;
    int sl = addr_to_sa(&c->a, &ss);
    c->tried = 1;
    SOCKET s = socket(ss.ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { c->fails++; c->retry = b->now + 60; return NULL; }
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    int r = connect(s, (struct sockaddr *)&ss, sl);      /* outgoing only: binds an ephemeral port */
    if (r != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(s);
        c->fails++;
        c->retry = b->now + 30.0 * c->fails * c->fails;
        return NULL;
    }
    peer *p = xcalloc(1, sizeof *p);
    p->s = s;
    p->a = c->a;
    p->ci = ci;
    p->st = r == 0 ? PS_HANDSHAKE : PS_CONNECTING;
    p->t_start = p->t_recv = p->t_send = p->t_block = b->now;
    p->am_choking = 1;
    p->choked = 1;
    p->md_piece = -1;
    p->reqq = 250;
    uint8_t hs[68];
    hs[0] = 19;
    memcpy(hs + 1, "BitTorrent protocol", 19);
    memset(hs + 20, 0, 8);
    hs[25] |= 0x10;                                       /* extension protocol (BEP 10) */
    memcpy(hs + 28, b->ih, 20);
    memcpy(hs + 48, b->pid, 20);
    wb_put(p, hs, sizeof hs);
    c->st = CS_CONNECTED;
    b->p[b->npeers++] = p;
    return p;
}

static void peer_close(bt *b, int i)
{
    peer *p = b->p[i];
    release_requests(b, p, 0);
    if (p->md_piece >= 0 && b->md_st && p->md_piece < b->md_n && b->md_st[p->md_piece] == 1)
        b->md_st[p->md_piece] = 0;
    if (b->have_meta && p->bf) {
        int nb = nbytes_of(b->m.np);
        for (int k = 0; k < nb; k++) {
            if (!p->bf[k]) continue;
            for (int q = 0; q < 8; q++) {
                int idx = k * 8 + q;
                if (idx < b->m.np && bit_get(p->bf, idx) && b->avail[idx]) b->avail[idx]--;
            }
        }
    }
    cand *c = &b->c[p->ci];
    if (c->st != CS_BANNED) {
        c->st = CS_IDLE;
        if (p->st != PS_ACTIVE) {
            c->fails++;
            c->retry = b->now + 30.0 * c->fails * c->fails;
        } else {
            c->retry = b->now + (b->now - p->t_start < 30 ? 120 : 60);
        }
    }
    closesocket(p->s);
    free(p->rb);
    free(p->wb);
    free(p->bf);
    free(p->pre);
    free(p);
    b->p[i] = b->p[--b->npeers];
}

static void peer_flush(bt *b, peer *p)
{
    (void)b;
    while (!p->dead && p->wpos < p->wlen) {
        size_t k = p->wlen - p->wpos;
        int n = send(p->s, (const char *)p->wb + p->wpos, (int)(k > (1u << 20) ? (1u << 20) : k), 0);
        if (n < 0) {
            if (WSAGetLastError() != WSAEWOULDBLOCK) peer_kill(p, "connection lost");
            break;
        }
        p->wpos += (size_t)n;
    }
    if (p->wpos == p->wlen) p->wpos = p->wlen = 0;
}

static void peer_recv(bt *b, peer *p)
{
    for (int round = 0; round < 8 && !p->dead; round++) {
        size_t need = 65536;
        if (p->rlen >= 4 && p->st == PS_ACTIVE) {          /* room for the whole pending message */
            uint32_t len = rd32(p->rb);
            if (len <= MAX_MSG && len + 4 > need) need = len + 4;
        }
        if (p->rcap < need) {
            p->rb = xrealloc(p->rb, need);
            p->rcap = need;
        }
        size_t room = p->rcap - p->rlen;
        if (room == 0) {
            if (p->rcap >= MAX_MSG + 4) { peer_kill(p, "message too large"); return; }
            p->rcap *= 2;
            if (p->rcap > MAX_MSG + 4) p->rcap = MAX_MSG + 4;
            p->rb = xrealloc(p->rb, p->rcap);
            room = p->rcap - p->rlen;
        }
        if (dl_rate(b) > 0) {
            if (b->dl_tokens <= 0) break;
            if ((double)room > b->dl_tokens) room = (size_t)b->dl_tokens + 1;
        }
        int n = recv(p->s, (char *)p->rb + p->rlen, (int)(room > (1u << 20) ? (1u << 20) : room), 0);
        if (n == 0) { peer_kill(p, "closed by the peer"); return; }
        if (n < 0) {
            if (WSAGetLastError() != WSAEWOULDBLOCK) peer_kill(p, "connection lost");
            return;
        }
        p->rlen += (size_t)n;
        p->t_recv = b->now;
        b->dl_tokens -= n;
        peer_parse(b, p);
        if ((size_t)n < room) break;
    }
}

static void serve_uploads(bt *b, peer *p)
{
    while (p->nup > 0 && !p->am_choking && !p->dead && p->wlen - p->wpos < 65536 && b->ul_tokens > 0) {
        preq r = p->upq[0];
        memmove(&p->upq[0], &p->upq[1], (size_t)(p->nup - 1) * sizeof(preq));
        p->nup--;
        uint8_t *buf = b->iobuf;
        wr32(buf, (uint32_t)(9 + r.len));
        buf[4] = 7;
        wr32(buf + 5, (uint32_t)r.piece);
        wr32(buf + 9, (uint32_t)r.begin);
        if (stor_io(&b->m, (int64_t)r.piece * b->m.plen + r.begin, buf + 13, r.len, 0) != 0) continue;
        wb_put(p, buf, (size_t)(13 + r.len));
        p->t_send = b->now;
        p->sent += r.len;
        p->sent_sec += r.len;
        b->up += r.len;
        b->ul_tokens -= r.len;
    }
}

/* ================================================================== */
/* trackers                                                            */
/* ================================================================== */
static void tracker_add(bt *b, const char *url)
{
    if (!url || b->ntr >= MAX_TRACKERS) return;
    size_t n = strlen(url);
    if (n < 10 || n > 2000) return;
    for (size_t i = 0; i < n; i++) if ((unsigned char)url[i] <= 32 || url[i] == 127) return;
    int udp = str_istarts(url, "udp://");
    if (!udp && !str_istarts(url, "http://") && !str_istarts(url, "https://")) return;
    for (int i = 0; i < b->ntr; i++) if (!strcmp(b->tr[i].url, url)) return;
    tracker *t = &b->tr[b->ntr++];
    memset(t, 0, sizeof *t);
    t->url = xstrdup(url);
    t->udp = udp;
}

static job *announce_job(bt *b, int ti, int event)
{
    job *j = job_new(b, JOB_ANNOUNCE);
    j->ti = ti;
    j->url = xstrdup(b->tr[ti].url);
    memcpy(j->ih, b->ih, 20);
    memcpy(j->pid, b->pid, 20);
    j->down = b->down;
    j->up = b->up;
    j->left = b->have_meta ? b->m.total - b->have_bytes : (int64_t)1 << 30;
    j->event = event;
    j->key = b->key;
    return j;
}

static int dht_wants_helpers(const bt *b);

static void trackers_tick(bt *b)
{
    int shortp = b->nactive < 15 && connectable(b) < 10;
    int cap = MAX_JOBS - dht_wants_helpers(b);          /* those stay free for the DHT */
    for (int i = 0; i < b->ntr && b->njobs < cap; i++) {
        tracker *t = &b->tr[i];
        if (t->busy) continue;
        int due = b->now >= t->next;
        int early = shortp && t->ok && b->now - t->last >= (t->minint > 90 ? t->minint : 90);
        if (!due && !early) continue;
        job *j = announce_job(b, i, t->started ? 0 : 2);
        if (!job_start(b, j, 1)) { t->next = b->now + 60; continue; }
        t->busy = 1;
        t->last = b->now;
    }
}

static void tracker_done(bt *b, job *j)
{
    if (j->ti < 0 || j->ti >= b->ntr) return;
    tracker *t = &b->tr[j->ti];
    t->busy = 0;
    char *host = url_host(t->url);
    if (j->ok) {
        t->ok++;
        t->started = 1;
        t->fails = 0;
        int iv = j->interval > 0 ? j->interval : 1800;
        if (iv < 120) iv = 120;
        if (iv > 3600) iv = 3600;
        t->interval = iv;
        t->minint = j->minint;
        t->next = b->now + iv;
        int added = 0;
        for (int i = 0; i < j->naddr; i++) added += cand_add(b, &j->addr[i], SRC_TRACKER);
        if (!t->logged && b->trk_logged < 8) {
            t->logged = 1;
            b->trk_logged++;
            if (j->seeders || j->leechers)
                tlog(b->t, LOG_DIM, "  tracker %s: %d peers (%d seeds, %d downloading)", host, j->naddr, j->seeders, j->leechers);
            else
                tlog(b->t, LOG_DIM, "  tracker %s: %d peers", host, j->naddr);
            if (j->warn[0]) tlog(b->t, LOG_DIM, "  tracker %s warns: %s", host, j->warn);
        }
    } else {
        t->fails++;
        int f = t->fails > 6 ? 6 : t->fails;
        t->next = b->now + 30.0 * (1 << (f - 1));
        if (!t->logged && b->trk_logged < 8) {
            t->logged = 1;
            b->trk_logged++;
            tlog(b->t, LOG_DIM, "  tracker %s: %s", host, j->err[0] ? j->err : "failed");
        }
    }
    free(host);
}

void torrent_flush_announces(int ms)
{
    for (int waited = 0; g_final_jobs > 0 && waited < ms; waited += 50) Sleep(50);
}

/* fire and forget: "completed" / "stopped". Ctrl+C does not cancel them
   (that is when "stopped" matters), only their own short timeout. */
static void trackers_final(bt *b, int event)
{
    int n = 0;
    for (int i = 0; i < b->ntr && n < 8; i++) {
        if (!b->tr[i].ok) continue;
        job *j = announce_job(b, i, event);
        j->final = 1;
        j->t->ignore_ctrl_c = 1;
        InterlockedIncrement(&g_final_jobs);
        if (job_start(b, j, 0)) n++;
        else InterlockedDecrement(&g_final_jobs);
    }
}

/* ================================================================== */
/* DHT (BEP 5): iterative get_peers toward the info-hash               */
/* ================================================================== */
static const char *dht_router_names[] = {
    "router.bittorrent.com:6881", "router.utorrent.com:6881",
    "dht.transmissionbt.com:6881", "dht.libtorrent.org:25401",
};

#define DHT_NROUTERS ((int)(sizeof dht_router_names / sizeof *dht_router_names))

/* helper threads to keep free for looking up the DHT routers */
static int dht_wants_helpers(const bt *b)
{
    return b->dht_on && !b->d.resolved && !b->d.resolving ? DHT_NROUTERS : 0;
}

static int dht_closer(const uint8_t *target, const uint8_t *x, const uint8_t *y)  /* x closer than y? */
{
    for (int i = 0; i < 20; i++) {
        uint8_t a = x[i] ^ target[i], c = y[i] ^ target[i];
        if (a != c) return a < c;
    }
    return 0;
}

static void dht_open(bt *b)
{
    dht *d = &b->d;
    memset(d, 0, sizeof *d);
    d->s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);         /* no bind(): sendto picks a port */
    if (d->s == INVALID_SOCKET) { b->dht_on = 0; return; }
    u_long nb = 1;
    ioctlsocket(d->s, FIONBIO, &nb);
    BOOL off = FALSE;
    DWORD ret;
    WSAIoctl(d->s, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &ret, NULL, NULL);
    rand_bytes(d->id, 20);
    b->dht_on = 1;
}

static void dht_close(bt *b)
{
    if (b->d.s && b->d.s != INVALID_SOCKET) closesocket(b->d.s);
    b->d.s = INVALID_SOCKET;
    b->dht_on = 0;
}

/* 1 if the query went out */
static int dht_query(bt *b, const baddr *a)
{
    dht *d = &b->d;
    if (d->npend >= DHT_PEND || a->fam != 4) return 0;
    uint16_t tid = ++d->tid;
    uint8_t msg[128];
    size_t n = 0;
#define PUT(s, l) do { memcpy(msg + n, s, l); n += l; } while (0)
    PUT("d1:ad2:id20:", 12);
    PUT(d->id, 20);
    PUT("9:info_hash20:", 14);
    PUT(b->ih, 20);
    PUT("e1:q9:get_peers2:roi1e1:t2:", 27);
    uint8_t t2[2] = { (uint8_t)(tid >> 8), (uint8_t)tid };
    PUT(t2, 2);
    PUT("1:y1:qe", 7);
#undef PUT
    struct sockaddr_storage ss;
    int sl = addr_to_sa(a, &ss);
    if (sendto(d->s, (const char *)msg, (int)n, 0, (struct sockaddr *)&ss, sl) != (int)n) return 0;
    d->bound = 1;
    d->pend[d->npend].tid = tid;
    d->pend[d->npend].a = *a;
    d->pend[d->npend].t = b->now;
    d->npend++;
    d->queries++;
    return 1;
}

static dnode *dht_find(dht *d, const baddr *a)
{
    for (int i = 0; i < d->nn; i++) if (addr_eq(&d->n[i].a, a)) return &d->n[i];
    return NULL;
}

static void dht_add(bt *b, const uint8_t *id, const baddr *a)
{
    dht *d = &b->d;
    if (!memcmp(id, d->id, 20) || dht_find(d, a)) return;
    int pos = d->nn;
    while (pos > 0 && dht_closer(b->ih, id, d->n[pos - 1].id)) pos--;
    if (pos >= DHT_N) return;
    if (d->nn == DHT_N) d->nn--;
    memmove(&d->n[pos + 1], &d->n[pos], (size_t)(d->nn - pos) * sizeof(dnode));
    memcpy(d->n[pos].id, id, 20);
    d->n[pos].a = *a;
    d->n[pos].st = 0;
    d->nn++;
}

static void dht_on_msg(bt *b, const uint8_t *buf, size_t len, const baddr *from)
{
    dht *d = &b->d;
    bdoc x;
    if (be_decode(&x, buf, len, 1024, NULL) < 0 || x.n[0].type != BE_DICT) goto out;
    size_t yl = 0, tl = 0;
    const uint8_t *y = be_str(&x, be_get(&x, 0, "y"), &yl), *t = be_str(&x, be_get(&x, 0, "t"), &tl);
    if (!y || yl != 1 || !t || tl != 2) goto out;
    uint16_t tid = (uint16_t)(t[0] << 8 | t[1]);
    int pi = -1;
    for (int i = 0; i < d->npend; i++) if (d->pend[i].tid == tid && addr_eq(&d->pend[i].a, from)) { pi = i; break; }
    if (pi < 0) goto out;                     /* not an answer to anything we asked */
    d->pend[pi] = d->pend[--d->npend];
    dnode *nd = dht_find(d, from);
    if (y[0] != 'r') { if (nd) nd->st = 3; goto out; }
    int r = be_get(&x, 0, "r");
    size_t il;
    const uint8_t *id = be_str(&x, be_get(&x, r, "id"), &il);
    if (!id || il != 20) { if (nd) nd->st = 3; goto out; }
    if (nd) nd->st = 2;
    size_t nl;
    const uint8_t *nodes = be_str(&x, be_get(&x, r, "nodes"), &nl);
    if (nodes && nl % 26 == 0)
        for (size_t i = 0; i < nl; i += 26) {
            baddr a;
            addr_v4(&a, nodes + i + 20, rd16(nodes + i + 24));
            if (a.port && addr_public4(a.ip)) dht_add(b, nodes + i, &a);
        }
    int vals = be_get(&x, r, "values");
    if (be_type(&x, vals) == BE_LIST) {
        int cnt = 0;
        for (int k = x.n[vals].child; k >= 0 && cnt < 300; k = x.n[k].next, cnt++) {
            size_t vl;
            const uint8_t *v = be_str(&x, k, &vl);
            if (!v || vl != 6) continue;
            baddr a;
            addr_v4(&a, v, rd16(v + 4));
            if (cand_add(b, &a, SRC_DHT)) { d->found++; d->total_found++; }
        }
    }
out:
    be_free(&x);
}

static void dht_recv(bt *b)
{
    uint8_t buf[2048];
    for (int k = 0; k < 64; k++) {
        struct sockaddr_storage ss;
        int sl = sizeof ss;
        int n = recvfrom(b->d.s, (char *)buf, (int)sizeof buf, 0, (struct sockaddr *)&ss, &sl);
        if (n < 0) {
            int e = WSAGetLastError();
            if (e == WSAECONNRESET || e == WSAEMSGSIZE) continue;
            break;
        }
        baddr from;
        if (n > 0 && addr_from_sa(&from, (struct sockaddr *)&ss)) dht_on_msg(b, buf, (size_t)n, &from);
    }
}

static void dht_tick(bt *b)
{
    dht *d = &b->d;
    if (!b->dht_on) return;
    if (!d->resolved && !d->resolving && b->now >= d->resolve_next && b->njobs < MAX_JOBS) {
        /* one lookup per router, so one slow name does not hold up the others
           (with every helper busy, this waits for the next free one) */
        for (int i = 0; i < DHT_NROUTERS && b->njobs < MAX_JOBS; i++) {
            job *j = job_new(b, JOB_RESOLVE);
            j->hosts[j->nhosts++] = xstrdup(dht_router_names[i]);
            if (job_start(b, j, 1)) d->resolving++;
        }
        if (!d->resolving) d->resolve_next = b->now + 60;        /* no thread could be started */
    }
    for (int i = 0; i < d->npend;) {                 /* timeouts */
        if (b->now - d->pend[i].t > 3.0) {
            dnode *nd = dht_find(d, &d->pend[i].a);
            if (nd) nd->st = 3;
            d->pend[i] = d->pend[--d->npend];
        } else i++;
    }
    if (!d->searching) {
        double gap = d->rounds == 0 ? 0 : (b->nactive == 0 && d->found == 0) ? 15 : (b->nactive < 20 ? 60 : 600);
        if ((d->nrouters > 0 || d->nn > 0 || b->nboot > 0) && b->now - d->t_end >= gap) {
            int k = 0;                               /* forget bad nodes, ask the rest again */
            for (int i = 0; i < d->nn; i++) if (d->n[i].st != 3) { d->n[k] = d->n[i]; d->n[k].st = 0; k++; }
            d->nn = k;
            d->searching = 1;
            d->queries = 0;
            d->found = 0;
            d->t_start = b->now;
            for (int i = 0; i < d->nrouters; i++) dht_query(b, &d->routers[i]);
            for (int i = 0; i < b->nboot; i++) dht_query(b, &b->boot[i]);
        }
    }
    if (d->searching) {
        int window = 0;
        for (int i = 0; i < d->nn && d->npend < DHT_ALPHA && window < 16; i++) {
            if (d->n[i].st == 3) continue;
            window++;
            if (d->n[i].st == 0 && dht_query(b, &d->n[i].a)) d->n[i].st = 1;   /* unsent: try again */
        }
        if ((d->npend == 0 && b->now - d->t_start > 1.0) || d->queries > 400 || b->now - d->t_start > 60) {
            d->searching = 0;
            d->t_end = b->now;
            d->rounds++;
            if (!d->logged && (d->total_found > 0 || d->rounds >= 2)) {
                d->logged = 1;
                if (d->total_found) tlog(b->t, LOG_DIM, "  DHT: %d peers", d->total_found);
                else tlog(b->t, LOG_DIM, "  DHT: no peers yet%s", d->nn ? "" : " (no DHT node answered)");
            }
        }
    }
}

static void dht_routers_ready(bt *b, job *j)
{
    dht *d = &b->d;
    if (d->resolving > 0) d->resolving--;
    for (int i = 0; i < j->naddr && d->nrouters < 16; i++) {
        int dup = 0;
        for (int k = 0; k < d->nrouters; k++) if (addr_eq(&d->routers[k], &j->addr[i])) dup = 1;
        if (dup) continue;
        d->routers[d->nrouters++] = j->addr[i];
        if (d->searching) dht_query(b, &j->addr[i]);      /* join the search in progress */
    }
    if (d->nrouters) d->resolved = 1;
    else if (!d->resolving) d->resolve_next = b->now + 60;
}

static void jobs_poll(bt *b)
{
    for (int i = 0; i < b->njobs;) {
        job *j = b->jobs[i];
        if (!InterlockedCompareExchange(&j->done, 0, 0)) { i++; continue; }
        if (j->kind == JOB_RESOLVE) { if (b->dht_on) dht_routers_ready(b, j); }
        else tracker_done(b, j);
        job_release(j);
        b->jobs[i] = b->jobs[--b->njobs];
    }
}

/* The DHT goes first: with many trackers their announces would otherwise
   take every helper and hold up the DHT bootstrap. */
static void helpers_tick(bt *b)
{
    jobs_poll(b);
    dht_tick(b);
    trackers_tick(b);
}

/* ================================================================== */
/* resume state: "ULBT1 <info-hash hex>\n" + bencoded dictionary       */
/* ================================================================== */
static char *state_path_for(bt *b)
{
    char *n = xasprintf("%s.ulbt", b->top);
    char *p = path_join(b->out_dir, n);
    free(n);
    return p;
}

static void state_save(bt *b)
{
    if (!b->have_meta || !b->state_path) return;
    int nb = nbytes_of(b->m.np);
    sbuf s = { 0 };
    sb_printf(&s, "ULBT1 %s\n", b->ihex);
    sb_puts(&s, "d4:info");
    sb_putn(&s, (const char *)b->m.info, b->m.info_len);
    sb_puts(&s, "4:name");
    be_put_str(&s, b->top, strlen(b->top));
    /* where to look first next time: DHT nodes near this torrent, peers that worked */
    sbuf x = { 0 };
    for (int i = 0, n = 0; b->dht_on && i < b->d.nn && n < 64; i++) {
        if (b->d.n[i].st == 3 || b->d.n[i].a.fam != 4) continue;
        uint8_t e[26];
        memcpy(e, b->d.n[i].id, 20);
        memcpy(e + 20, b->d.n[i].a.ip, 4);
        wr16(e + 24, b->d.n[i].a.port);
        sb_putn(&x, (const char *)e, 26);
        n++;
    }
    sb_puts(&s, "5:nodes");
    be_put_str(&s, x.s ? x.s : "", x.len);
    sb_free(&x);
    sbuf y = { 0 };
    for (int i = 0, n = 0; i < b->nc && n < 200; i++) {
        cand *c = &b->c[i];
        if (!c->good || c->st == CS_BANNED) continue;
        uint8_t e[18];
        memcpy(e, c->a.ip, c->a.fam == 4 ? 4 : 16);
        wr16(e + (c->a.fam == 4 ? 4 : 16), c->a.port);
        sb_putn(c->a.fam == 4 ? &x : &y, (const char *)e, c->a.fam == 4 ? 6 : 18);
        n++;
    }
    sb_puts(&s, "5:peers");
    be_put_str(&s, x.s ? x.s : "", x.len);
    sb_puts(&s, "6:peers6");
    be_put_str(&s, y.s ? y.s : "", y.len);
    sb_free(&x);
    sb_free(&y);
    sb_puts(&s, "6:pieces");
    be_put_str(&s, b->have, (size_t)nb);
    sb_printf(&s, "5:totali%llde", (long long)b->m.total);
    sb_putc(&s, 'e');
    char *tmp = xasprintf("%s.tmp", b->state_path);
    wchar_t *w = wpath(tmp);
    HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    int ok = 0;
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        ok = WriteFile(h, s.s, (DWORD)s.len, &wr, NULL) && wr == s.len;
        CloseHandle(h);
        if (ok) ok = file_rename(tmp, b->state_path) == 0;
        if (!ok) file_delete(tmp);
    }
    free(tmp);
    sb_free(&s);
    if (ok) b->dirty = 0;
}

typedef struct {
    uint8_t *info; size_t info_len;
    char    *name;
    uint8_t *bits; size_t bits_len;
    uint8_t *nodes, *peers, *peers6;
    size_t   nodes_len, peers_len, peers6_len;
} bt_state;

static void state_clear(bt_state *st)
{
    free(st->info); free(st->name); free(st->bits);
    free(st->nodes); free(st->peers); free(st->peers6);
    memset(st, 0, sizeof *st);
}

static uint8_t *dup_str(const bdoc *d, int i, size_t *len)
{
    size_t n;
    const uint8_t *s = be_str(d, i, &n);
    *len = 0;
    if (!s) return NULL;
    uint8_t *r = xmalloc(n ? n : 1);
    memcpy(r, s, n);
    *len = n;
    return r;
}

static int state_load(const char *path, const uint8_t ih[20], bt_state *st)
{
    memset(st, 0, sizeof *st);
    int64_t fsz = file_size(path);
    if (fsz < 48 || fsz > MD_MAX + (16 << 20)) return -1;
    size_t len;
    char *data = read_file(path, &len);
    if (!data) return -1;
    char hex[41];
    hex_encode(ih, 20, hex);
    int rc = -1;
    bdoc d = { 0 };
    if (len < 47 || memcmp(data, "ULBT1 ", 6) != 0 || memcmp(data + 6, hex, 40) != 0 || data[46] != '\n') goto out;
    if (be_decode(&d, data + 47, len - 47, 4000000, NULL) < 0 || d.n[0].type != BE_DICT) goto out;
    int in = be_get(&d, 0, "info");
    if (be_type(&d, in) != BE_DICT) goto out;
    const uint8_t *raw = d.buf + d.n[in].off;
    size_t rl = d.n[in].end - d.n[in].off;
    uint8_t h[20];
    if (!sha1(raw, rl, h) || memcmp(h, ih, 20) != 0) goto out;
    size_t nl = 0, bl = 0;
    const uint8_t *nm = be_str(&d, be_get(&d, 0, "name"), &nl);
    const uint8_t *bits = be_str(&d, be_get(&d, 0, "pieces"), &bl);
    if (!nm || !nl || !bits) goto out;
    st->info = xmalloc(rl);
    memcpy(st->info, raw, rl);
    st->info_len = rl;
    st->name = be_cstr(&d, be_get(&d, 0, "name"));
    st->bits = xmalloc(bl ? bl : 1);
    memcpy(st->bits, bits, bl);
    st->bits_len = bl;
    st->nodes = dup_str(&d, be_get(&d, 0, "nodes"), &st->nodes_len);
    st->peers = dup_str(&d, be_get(&d, 0, "peers"), &st->peers_len);
    st->peers6 = dup_str(&d, be_get(&d, 0, "peers6"), &st->peers6_len);
    rc = 0;
out:
    be_free(&d);
    free(data);
    return rc;
}

/* the state file of this info-hash in out_dir, if any */
static char *state_find(const char *out_dir, const uint8_t ih[20])
{
    char hex[41], head[48];
    hex_encode(ih, 20, hex);
    char *pat = path_join(out_dir, "*.ulbt");
    wchar_t *w = wpath(pat);
    free(pat);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(w, &fd);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    char *found = NULL;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char *name = w_to_u8(fd.cFileName);
        char *p = path_join(out_dir, name);
        free(name);
        FILE *f = fopen_u8(p, "rb");
        if (f) {
            size_t n = fread(head, 1, 47, f);
            fclose(f);
            if (n == 47 && memcmp(head, "ULBT1 ", 6) == 0 && memcmp(head + 6, hex, 40) == 0) { found = p; break; }
        }
        free(p);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

/* ================================================================== */
/* setup once the metadata is known                                    */
/* ================================================================== */
static void publish(bt *b, int force);

/* Verify what is already on disk. 0 done, -2 aborted. */
static int recheck(bt *b)
{
    meta *m = &b->m;
    int64_t *fsz = xmalloc((size_t)m->nfiles * sizeof(int64_t));
    for (int i = 0; i < m->nfiles; i++) fsz[i] = m->files[i].pad ? m->files[i].len : file_size(m->files[i].path);
    uint8_t *buf = xmalloc((size_t)m->plen);
    double last = 0;
    int rc = 0;
    for (int i = 0; i < m->np; i++) {
        if (task_aborted(b->t)) { rc = -2; break; }
        int len = piece_len(b, i), n;
        fseg *sg = map_range(m->files, m->nfiles, (int64_t)i * m->plen, len, &n);
        int present = n > 0;
        for (int k = 0; k < n; k++)
            if (fsz[sg[k].fi] < sg[k].foff + sg[k].len) { present = 0; break; }
        free(sg);
        if (present && stor_io(m, (int64_t)i * m->plen, buf, len, 0) == 0) {
            uint8_t h[20];
            if (sha1(buf, (size_t)len, h) && memcmp(h, m->hashes + (size_t)i * 20, 20) == 0 && !bit_get(b->have, i)) {
                bit_set(b->have, i);
                b->nhave++;
                b->have_bytes += len;
            }
        }
        double now = now_sec();
        if (now - last > 0.5) {
            last = now;
            task_set_stage(b->t, "checking files %d%%", (int)((int64_t)(i + 1) * 100 / m->np));
            b->t->done = b->have_bytes;
            b->t->total = m->total;
            task_progress(b->t);
        }
    }
    meta_close_files(m);
    free(buf);
    free(fsz);
    return rc;
}

static int name_unusable(const char *s)
{
    for (; *s; s++) if (*s != '.' && *s != ' ') return 0;
    return 1;
}

static int pick_top(bt *b, const char *state_name)
{
    char *top = NULL;
    if (state_name && !name_unusable(state_name)) top = sanitize_filename(path_basename(state_name));
    else if (!name_unusable(b->m.disp)) top = sanitize_filename(b->m.disp);
    else if (b->dn && !name_unusable(b->dn)) top = sanitize_filename(b->dn);
    else top = xasprintf("torrent-%.8s", b->ihex);
    free(b->top);
    b->top = top;
    free(b->target);
    b->target = path_join(b->out_dir, top);
    return 0;
}

/* Is "<top>.ulbt" there and not ours? Then the name belongs to another
   torrent's unfinished download (or to some other file): not ours to
   overwrite, and neither are the files under that name. */
static int state_foreign(bt *b)
{
    char *p = state_path_for(b);
    int foreign = 0;
    if (file_exists(p)) {
        char head[47];
        FILE *f = fopen_u8(p, "rb");
        size_t n = f ? fread(head, 1, sizeof head, f) : 0;
        if (f) fclose(f);
        foreign = !(n == sizeof head && memcmp(head, "ULBT1 ", 6) == 0 && memcmp(head + 6, b->ihex, 40) == 0);
    }
    free(p);
    return foreign;
}

/* a top name that is not taken: "name (2).ext" / "name (2)" */
static void pick_unique_top(bt *b)
{
    for (int i = 2; i < 10000; i++) {
        char *cand = b->m.multi ? xasprintf("%s (%d)", b->top, i) : suffixed(b->top, i);
        char *path = path_join(b->out_dir, cand);
        char *st = xasprintf("%s.ulbt", path);
        int taken = file_exists(path) || file_exists(st);
        free(st);
        if (!taken) {
            free(b->top);
            b->top = cand;
            free(b->target);
            b->target = path;
            return;
        }
        free(path);
        free(cand);
    }
}

/* 0 ok (also when complete), -1 error (bt_fail), -2 aborted */
static int setup_meta(bt *b, const char *state_name, const uint8_t *bits, size_t bits_len)
{
    meta *m = &b->m;
    char err[300], a[32], c[32];
    memcpy(b->ih, m->ih, 20);
    b->priv = m->priv;
    if (b->priv && b->dht_on) dht_close(b);
    int np = m->np, nb = nbytes_of(np);
    b->have = xcalloc((size_t)nb, 1);
    b->avail = xcalloc((size_t)np, sizeof(uint16_t));
    b->slot = xmalloc((size_t)np * sizeof(int32_t));
    for (int i = 0; i < np; i++) b->slot[i] = -1;
    b->nhave = 0;
    b->have_bytes = 0;

    pick_top(b, state_name);
    int existing = !bits && file_exists(b->target);
    int foreign = !bits && state_foreign(b);
    if (meta_layout(m, b->out_dir, b->top, err, sizeof err) != 0) { bt_fail(b, "%s", err); return -1; }

    int nfiles = 0;
    for (int i = 0; i < m->nfiles; i++) if (!m->files[i].pad) nfiles++;
    tlog(b->t, LOG_HEAD, "%s", b->top);
    tlog(b->t, LOG_DIM, "  %s  |  %d file%s  |  %d pieces of %s  |  info-hash %s%s",
         fmt_size(m->total, a, sizeof a), nfiles, nfiles == 1 ? "" : "s", np,
         fmt_size(m->plen, c, sizeof c), b->ihex, m->priv ? "  |  private" : "");
    if (m->dropped) tlog(b->t, LOG_WARN, "  ignored %d unsafe path element%s (\"..\" etc.) in the torrent's file names",
                         m->dropped, m->dropped == 1 ? "" : "s");
    if (m->renamed) tlog(b->t, LOG_WARN, "  renamed %d file%s whose names collide", m->renamed, m->renamed == 1 ? "" : "s");
    task_set_file(b->t, b->top, b->target);
    b->t->total = m->total;

    if (bits) {
        if (bits_len == (size_t)nb) {
            memcpy(b->have, bits, (size_t)nb);
            clear_spare_bits(b->have, np);
        }
        int changed = 0;
        for (int i = 0; i < m->nfiles; i++) {      /* a file that changed size lost its pieces */
            bfile *f = &m->files[i];
            if (f->pad || f->len == 0 || file_size(f->path) == f->len) continue;
            int first = (int)(f->off / m->plen), last = (int)((f->off + f->len - 1) / m->plen), had = 0;
            for (int k = first; k <= last; k++) { had |= bit_get(b->have, k); bit_clr(b->have, k); }
            changed += had;                         /* (one never created yet lost nothing) */
        }
        for (int i = 0; i < np; i++)
            if (bit_get(b->have, i)) { b->nhave++; b->have_bytes += piece_len(b, i); }
        if (changed) tlog(b->t, LOG_WARN, "  %d file%s changed or went missing since last time - downloading %s again",
                          changed, changed == 1 ? "" : "s", changed == 1 ? "it" : "them");
        if (b->nhave)
            tlog(b->t, LOG_DIM, "  resuming at %.1f%% (%s already verified)", 100.0 * (double)b->have_bytes / (double)m->total,
                 fmt_size(b->have_bytes, a, sizeof a));
    } else if (foreign) {
        char *was = xstrdup(b->top);
        pick_unique_top(b);
        if (meta_layout(m, b->out_dir, b->top, err, sizeof err) != 0) { free(was); bt_fail(b, "%s", err); return -1; }
        tlog(b->t, LOG_DIM, "  another torrent is being saved as %s - saving as %s", was, b->top);
        free(was);
        task_set_file(b->t, b->top, b->target);
    } else if (existing && !b->cfg->overwrite) {
        tlog(b->t, LOG_DIM, "  %s already exists - checking what is already there", b->top);
        if (recheck(b) == -2) return -2;
        if (b->nhave == 0) {
            pick_unique_top(b);
            if (meta_layout(m, b->out_dir, b->top, err, sizeof err) != 0) { bt_fail(b, "%s", err); return -1; }
            tlog(b->t, LOG_DIM, "  nothing usable in it - saving as %s", b->top);
            task_set_file(b->t, b->top, b->target);
        } else {
            tlog(b->t, LOG_DIM, "  %.1f%% of it is already there (%s)", 100.0 * (double)b->have_bytes / (double)m->total,
                 fmt_size(b->have_bytes, a, sizeof a));
        }
    }
    b->start_bytes = b->have_bytes;
    free(b->state_path);
    b->state_path = state_path_for(b);
    if (b->nhave == np) {
        /* complete: still give every file its exact size (cut what is past
           the end, create missing empty files) */
        int r = stor_prepare(m, b->t);
        if (r == -2) return -2;
        if (r != 0) tlog(b->t, LOG_WARN, "  %s", m->ioerr);
        return 0;
    }

    int64_t need = m->total - b->have_bytes, fr = disk_free(b->target);
    if (fr >= 0 && fr < need) {
        bt_fail(b, "not enough disk space (need %s, have %s)", fmt_size(need, a, sizeof a), fmt_size(fr, c, sizeof c));
        return -1;
    }
    int r = stor_prepare(m, b->t);
    if (r == -2) return -2;
    if (r != 0) { bt_fail(b, "%s", m->ioerr); return -1; }
    state_save(b);
    b->max_act = 8;
    return 0;
}

/* ================================================================== */
/* periodic work                                                       */
/* ================================================================== */
static void publish(bt *b, int force)
{
    ul_task *t = b->t;
    if (!force && b->now - b->t_pub < 0.5) return;
    b->t_pub = b->now;
    b->speed = speedo_add(&b->sp, b->now, b->down);
    int np = b->m.np;
    unsigned char map[UL_SEGVIEW];
    memset(map, 0, sizeof map);
    if (b->have_meta && np > 0) {
        for (int i = 0; i < UL_SEGVIEW; i++) {
            int a = (int)((int64_t)np * i / UL_SEGVIEW), e = (int)((int64_t)np * (i + 1) / UL_SEGVIEW);
            if (e <= a) e = a + 1;
            if (e > np) e = np;
            int got = 0;
            for (int k = a; k < e; k++) got += bit_get(b->have, k);
            map[i] = (unsigned char)(got * 255 / (e - a));
        }
    }
    EnterCriticalSection(&t->lock);
    memcpy(t->segmap, map, sizeof map);
    LeaveCriticalSection(&t->lock);
    int active = 0, seeds = 0;
    for (int i = 0; i < b->npeers; i++)
        if (b->p[i]->st == PS_ACTIVE && !b->p[i]->dead) { active++; seeds += b->p[i]->seed; }
    b->nactive = active;
    b->nseeds = seeds;
    t->done = b->have_bytes;
    t->total = b->have_meta ? b->m.total : -1;
    t->speed = (LONG64)b->speed;
    t->eta = (b->have_meta && b->speed > 1) ? (LONG64)((double)(b->m.total - b->have_bytes) / b->speed) : -1;
    t->conns = b->nactive;
    if (!b->have_meta)
        task_set_stage(t, b->nactive ? "fetching metadata, %d peer%s" : "finding peers", b->nactive, b->nactive == 1 ? "" : "s");
    else if (!b->nactive)
        task_set_stage(t, "finding peers");
    else
        task_set_stage(t, "%d peer%s, %d seed%s", b->nactive, b->nactive == 1 ? "" : "s", b->nseeds, b->nseeds == 1 ? "" : "s");
    task_progress(t);
}

static void rechoke(bt *b)
{
    peer *list[MAX_PEERS];
    int n = 0;
    for (int i = 0; i < b->npeers; i++) {
        peer *p = b->p[i];
        if (p->st == PS_ACTIVE && !p->dead && p->interested) list[n++] = p;
    }
    for (int i = 1; i < n; i++) {              /* best uploaders to us first */
        peer *x = list[i];
        int k = i;
        while (k > 0 && list[k - 1]->rate < x->rate) { list[k] = list[k - 1]; k--; }
        list[k] = x;
    }
    if (b->now - b->t_optim >= 30 && n > 3) {
        b->optim = list[3 + (int)(bt_rand(b) % (uint32_t)(n - 3))]->a;
        b->t_optim = b->now;
    }
    for (int i = 0; i < b->npeers; i++) {
        peer *p = b->p[i];
        if (p->st != PS_ACTIVE || p->dead) continue;
        int pos = -1;
        for (int k = 0; k < n; k++) if (list[k] == p) { pos = k; break; }
        int un = b->nhave > 0 && pos >= 0 && (pos < 3 || addr_eq(&p->a, &b->optim));
        if (un && p->am_choking) { p->am_choking = 0; send_simple(b, p, 1); }
        else if (!un && !p->am_choking) { p->am_choking = 1; p->nup = 0; send_simple(b, p, 0); }
    }
}

static void prune_peers(bt *b)
{
    if (b->npeers < MAX_PEERS - 4 || connectable(b) < 5) return;
    for (int round = 0; round < 3; round++) {
        peer *worst = NULL;
        for (int i = 0; i < b->npeers; i++) {
            peer *p = b->p[i];
            if (p->dead || p->st != PS_ACTIVE || b->now - p->t_start < 90) continue;
            int useless = !p->am_interested || p->rate < 2048;
            if (!useless) continue;
            if (!worst || p->rate < worst->rate) worst = p;
        }
        if (!worst) break;
        peer_kill(worst, "too slow");
    }
}

static void tick(bt *b)
{
    double dt = b->now - b->t_tick;
    if (dt < 1.0) return;
    b->t_tick = b->now;
    int active = 0, seeds = 0, unchoked = 0, useful = 0;
    for (int i = 0; i < b->npeers; i++) {
        peer *p = b->p[i];
        if (p->dead) continue;
        p->rate = 0.5 * p->rate + 0.5 * (double)p->got_sec / dt;
        p->urate = 0.5 * p->urate + 0.5 * (double)p->sent_sec / dt;
        p->got_sec = p->sent_sec = 0;
        double age = b->now - p->t_start;
        if (p->st == PS_CONNECTING && age > 8) { peer_kill(p, "connect timeout"); continue; }
        if (p->st == PS_HANDSHAKE && age > 20) { peer_kill(p, "handshake timeout"); continue; }
        if (p->st != PS_ACTIVE) continue;
        if (b->now - p->t_recv > 180) { peer_kill(p, "silent for 3 minutes"); continue; }
        if (b->now - p->t_send > 100) { uint8_t z[4] = { 0 }; wb_put(p, z, 4); p->t_send = b->now; }
        if (p->nreq > 0 && b->now - p->t_block > 30 && b->now - p->req[0].t > 30) {
            release_requests(b, p, 1);              /* snubbed: let other peers fetch it */
            p->snubbed = 1;
            p->t_snub = b->now;
            if (b->nactive > 8) { peer_kill(p, "stopped sending data"); continue; }
        }
        if (p->snubbed && b->now - p->t_snub > 60) p->snubbed = 0;
        if (p->md_piece >= 0 && b->now - p->md_t > 20) {
            if (b->md_st && p->md_piece < b->md_n && b->md_st[p->md_piece] == 1) b->md_st[p->md_piece] = 0;
            p->md_piece = -1;
            p->md_bad++;
        }
        active++;
        if (p->seed) seeds++;
        if (b->have_meta ? p->nhave > 0 : (p->ut_md && p->md_size > 0 && p->md_bad < 3)) useful++;
        if (!p->choked && p->am_interested) unchoked++;
    }
    b->nactive = active;
    b->nseeds = seeds;
    b->nuseful = useful;
    if (useful) { b->t_last_peer = b->now; b->ever_useful = 1; }
    if (b->have_meta) {
        if (b->now - b->t_interest >= 5) {
            b->t_interest = b->now;
            for (int i = 0; i < b->npeers; i++) update_interest(b, b->p[i]);
        }
        int bpp = (int)((b->m.plen + BLOCK - 1) / BLOCK);
        int64_t want = 4 + (int64_t)unchoked * (2 + MAX_PIPE / bpp);
        int64_t mem = MEM_BUDGET / b->m.plen;
        if (mem < 2) mem = 2;
        if (want > mem) want = mem;
        if (want > MAX_ACTIVE) want = MAX_ACTIVE;
        b->max_act = (int)want;
        /* forget started pieces that no connected peer has and nobody sent anything for */
        for (int k = b->nact - 1; k >= 0; k--) {
            apiece *a = b->act[k];
            if (a->nrecv || b->avail[a->idx]) continue;
            int busy = 0;
            for (int j = 0; j < a->nblk && !busy; j++) busy = a->dup[j] != 0;
            if (!busy) act_remove(b, k);
        }
    }
    if (!b->have_meta && b->md_size && b->now - b->t_md > 30) {
        /* no metadata progress: if other peers claim a different size, the
           size we went with may be a lie - start over with theirs */
        int other = 0;
        for (int i = 0; i < b->npeers; i++) {
            peer *p = b->p[i];
            if (p->st == PS_ACTIVE && !p->dead && p->ut_md && p->md_size > 0 && p->md_size != b->md_size && p->md_bad < 3) other = 1;
        }
        if (other) {                /* (otherwise look again next second: such a peer may come) */
            for (int i = 0; i < b->npeers; i++)
                if (b->p[i]->md_size == b->md_size) b->p[i]->md_bad++;
            md_reset(b);
        }
    }
    if (b->now - b->t_choke >= 10) { b->t_choke = b->now; rechoke(b); }
    if (b->now - b->t_prune >= 30) { b->t_prune = b->now; prune_peers(b); }
    /* upload budget: a fraction of the download speed, so uploading never chokes the download */
    double ul = b->speed / 2;
    if (ul < 256 * 1024) ul = 256 * 1024;
    if (ul > 4 * 1024 * 1024) ul = 4 * 1024 * 1024;
    b->ul_rate = ul;
}

static void connect_more(bt *b)
{
    if (b->now - b->t_connect < 0.1 || b->npeers >= MAX_PEERS) return;
    b->t_connect = b->now;
    int half = 0;
    for (int i = 0; i < b->npeers; i++) if (b->p[i]->st == PS_CONNECTING) half++;
    int burst = MAX_HALFOPEN - half;
    if (burst > MAX_PEERS - b->npeers) burst = MAX_PEERS - b->npeers;
    if (burst > 12) burst = 12;
    for (int pass = 0; pass < 2 && burst > 0; pass++)
        for (int i = 0; i < b->nc && burst > 0; i++) {
            cand *c = &b->c[i];
            if (c->st != CS_IDLE || c->retry > b->now || c->fails >= 4 || (pass == 0 && c->tried)) continue;
            peer_connect(b, i);
            burst--;
        }
}

static void net_poll(bt *b, int wait_ms)
{
    fd_set rd, wr, ex;
    FD_ZERO(&rd);
    FD_ZERO(&wr);
    FD_ZERO(&ex);
    int cnt = 0;
    int can_read = dl_rate(b) <= 0 || b->dl_tokens > 0;
    for (int i = 0; i < b->npeers; i++) {
        peer *p = b->p[i];
        if (p->dead) continue;
        if (p->st == PS_CONNECTING) { FD_SET(p->s, &wr); FD_SET(p->s, &ex); cnt++; continue; }
        if (can_read) { FD_SET(p->s, &rd); cnt++; }
        if (p->wlen > p->wpos) { FD_SET(p->s, &wr); cnt++; }
    }
    if (b->dht_on && b->d.bound) { FD_SET(b->d.s, &rd); cnt++; }
    if (!cnt) { Sleep((DWORD)wait_ms); return; }
    struct timeval tv = { 0, wait_ms * 1000 };
    int r = select(0, &rd, &wr, &ex, &tv);
    if (r <= 0) { if (r < 0) Sleep(10); return; }
    b->now = now_sec();
    for (int i = 0; i < b->npeers; i++) {
        peer *p = b->p[i];
        if (p->dead) continue;
        if (p->st == PS_CONNECTING) {
            if (FD_ISSET(p->s, &ex)) { peer_kill(p, "connection refused"); continue; }
            if (FD_ISSET(p->s, &wr)) {
                p->st = PS_HANDSHAKE;
                p->t_start = p->t_recv = b->now;
                peer_flush(b, p);
            }
            continue;
        }
        if (FD_ISSET(p->s, &wr)) peer_flush(b, p);
        if (!p->dead && FD_ISSET(p->s, &rd)) peer_recv(b, p);
    }
    if (b->dht_on && b->d.bound && FD_ISSET(b->d.s, &rd)) dht_recv(b);
}

/* ================================================================== */
/* entry point                                                         */
/* ================================================================== */
static int load_torrent_file(bt *b, const char *source, const unsigned char *data, size_t len)
{
    char err[300];
    char *own = NULL;
    if (!data) {
        int64_t sz = file_size(source);
        if (sz < 0) { tlog(b->t, LOG_ERR, "cannot open %s", source); return 1; }
        if (sz > TORRENT_MAX) { tlog(b->t, LOG_ERR, "%s is too large to be a .torrent file", source); return 1; }
        own = read_file(source, &len);
        if (!own) { tlog(b->t, LOG_ERR, "cannot read %s", source); return 1; }
        data = (const unsigned char *)own;
    }
    if (len > TORRENT_MAX) { free(own); tlog(b->t, LOG_ERR, "the .torrent file is too large"); return 1; }
    bdoc d;
    int rc = 1;
    if (be_decode(&d, data, len, 4000000, NULL) < 0 || d.n[0].type != BE_DICT) {
        tlog(b->t, LOG_ERR, "not a valid .torrent file (%s)", d.err ? d.err : "not a dictionary");
        goto out;
    }
    int in = be_get(&d, 0, "info");
    if (be_type(&d, in) != BE_DICT) {
        tlog(b->t, LOG_ERR, "not a valid .torrent file (no info dictionary)");
        goto out;
    }
    if (meta_parse(&b->m, data + d.n[in].off, d.n[in].end - d.n[in].off, err, sizeof err) != 0) {
        tlog(b->t, LOG_ERR, "%s", err);
        goto out;
    }
    b->have_meta = 1;
    memcpy(b->ih, b->m.ih, 20);
    char *an = be_cstr(&d, be_get(&d, 0, "announce"));
    int al = be_get(&d, 0, "announce-list");
    if (be_type(&d, al) == BE_LIST)
        for (int tier = d.n[al].child; tier >= 0; tier = d.n[tier].next) {
            if (be_type(&d, tier) == BE_LIST) {
                for (int k = d.n[tier].child; k >= 0; k = d.n[k].next) {
                    char *u = be_cstr(&d, k);
                    if (u) tracker_add(b, str_trim(u));
                    free(u);
                }
            } else if (be_type(&d, tier) == BE_STR) {
                char *u = be_cstr(&d, tier);
                if (u) tracker_add(b, str_trim(u));
                free(u);
            }
        }
    if (an) tracker_add(b, str_trim(an));
    free(an);
    int nodes = be_get(&d, 0, "nodes");             /* BEP 5 bootstrap nodes for trackerless torrents */
    if (be_type(&d, nodes) == BE_LIST)
        for (int k = d.n[nodes].child; k >= 0 && b->nboot < 16; k = d.n[k].next) {
            int hn = d.n[k].child;
            if (be_type(&d, k) != BE_LIST || hn < 0) continue;
            char *host = be_cstr(&d, hn);
            int64_t port;
            baddr a;
            if (host && be_int(&d, d.n[hn].next, &port) && port > 0 && port < 65536 &&
                addr_from_text_ip(host, (uint16_t)port, &a) && a.fam == 4)
                b->boot[b->nboot++] = a;
            free(host);
        }
    rc = 0;
out:
    be_free(&d);
    free(own);
    return rc;
}

static void bt_free(bt *b)
{
    for (int i = b->npeers - 1; i >= 0; i--) peer_close(b, i);
    for (int i = 0; i < b->njobs; i++) {
        InterlockedExchange(&b->jobs[i]->t->abort, 1);   /* let it end quickly; nobody waits */
        job_release(b->jobs[i]);
    }
    b->njobs = 0;
    if (b->dht_on) dht_close(b);
    while (b->nact) act_remove(b, b->nact - 1);
    for (int i = 0; i < b->ntr; i++) free(b->tr[i].url);
    meta_free(&b->m);
    free(b->have); free(b->avail); free(b->slot); free(b->c);
    free(b->md); free(b->md_st); free(b->md_from);
    free(b->top); free(b->target); free(b->state_path); free(b->dn);
    free(b->iobuf);
    free(b);
}

static int bt_main(bt *b)
{
    ul_task *t = b->t;
    char a[32], c[32];
    int rc = 1;
    b->now = now_sec();
    b->t_tick = b->t_last_peer = b->t_last_data = b->t_fill = b->now;
    b->ul_rate = 256 * 1024;
    b->max_act = 8;
    for (;;) {
        b->now = now_sec();
        if (task_aborted(t)) { rc = 2; break; }
        if (b->fatal) { rc = 1; break; }
        if (b->have_meta && b->nhave == b->m.np) { rc = 0; break; }

        helpers_tick(b);
        connect_more(b);

        net_poll(b, 50);
        b->now = now_sec();
        {                                           /* refill the rate buckets */
            double dt = b->now - b->t_fill;
            if (dt < 0 || dt > 1) dt = 1;
            b->t_fill = b->now;
            int64_t rate = dl_rate(b);
            if (rate > 0) {
                double cap = (double)rate * 0.5;
                b->dl_tokens += (double)rate * dt;
                if (b->dl_tokens > cap) b->dl_tokens = cap;
            }
            b->ul_tokens += b->ul_rate * dt;
            if (b->ul_tokens > b->ul_rate) b->ul_tokens = b->ul_rate;
        }

        for (int i = b->npeers - 1; i >= 0; i--)
            if (b->p[i]->dead) peer_close(b, i);
        for (int i = 0; i < b->npeers && !b->fatal && !task_aborted(t); i++) {
            peer *p = b->p[i];
            if (p->dead || p->st != PS_ACTIVE) continue;
            if (b->have_meta) {
                fill_requests(b, p);
                if (p->nup) serve_uploads(b, p);
            } else {
                md_request(b, p);
            }
            if (p->wlen > p->wpos) peer_flush(b, p);
        }
        tick(b);
        publish(b, 0);
        if (b->have_meta && b->dirty && b->now - b->t_save >= 5) { b->t_save = b->now; state_save(b); }

        if (!b->nuseful && b->now - b->t_last_peer > NO_PEERS_SECS) {
            int mins = (int)(NO_PEERS_SECS / 60);
            if (!b->ever_peer)
                bt_fail(b, "no peers found in %d minutes - the torrent may have no seeders, "
                           "or trackers/DHT are blocked on this network", mins);
            else if (!b->ever_useful && !b->have_meta)
                bt_fail(b, "none of the peers found in %d minutes could send this torrent's metadata", mins);
            else if (!b->ever_useful)
                bt_fail(b, "none of the peers found in %d minutes has any part of this torrent", mins);
            else
                bt_fail(b, "lost every useful peer for %d minutes - nobody is sharing this torrent right now", mins);
        } else if (!b->have_meta && b->now - b->t0 > MD_STALL_SECS && b->ever_peer) {
            bt_fail(b, "could not get the torrent's metadata from its peers in %d minutes", (int)(MD_STALL_SECS / 60));
        } else if (b->have_meta && b->now - b->t_last_data > STALL_SECS) {
            bt_fail(b, "no data for %d minutes - the peers do not have the missing pieces", (int)(STALL_SECS / 60));
        }
    }

    con_progress_end();
    double elapsed = now_sec() - b->t0;
    if (rc == 0) {
        publish(b, 1);
        for (int i = b->npeers - 1; i >= 0; i--) peer_close(b, i);
        stor_finish(&b->m);
        if (b->state_path) file_delete(b->state_path);
        int64_t got = b->have_bytes - b->start_bytes;
        if (got > 0) {
            trackers_final(b, 1);
            char sp[32];
            tlog(t, LOG_OK, "  Done: %s in %s (avg %s/s)", fmt_size(b->m.total, a, sizeof a),
                 fmt_dur(elapsed, c, sizeof c), fmt_size((int64_t)(elapsed > 0 ? (double)got / elapsed : 0), sp, sizeof sp));
        } else {
            tlog(t, LOG_OK, "  Already downloaded");
        }
        if (b->hashfails) tlog(t, LOG_DIM, "  %d corrupt piece%s were discarded and downloaded again%s",
                               b->hashfails, b->hashfails == 1 ? "" : "s", b->banned ? "; bad peers were banned" : "");
        tlog(t, LOG_DIM, "  Saved to %s", b->target);
        return 0;
    }
    if (b->have_meta) {
        meta_close_files(&b->m);
        state_save(b);
    }
    trackers_final(b, 3);
    if (rc == 2) {
        tlog(t, LOG_WARN, "paused - start it again to resume.");
        return 2;
    }
    tlog(t, LOG_ERR, "%s", b->fatal_msg[0] ? b->fatal_msg : "the torrent download failed");
    if (b->have_meta && b->nhave) tlog(t, LOG_INFO, "  Progress is saved - start it again to resume.");
    return 1;
}

int torrent_run(http_session *s, ul_config *cfg, const char *source, const unsigned char *data, size_t len)
{
    ul_task *t = http_task(s);
    if (!net_init()) { tlog(t, LOG_ERR, "cannot start Windows networking"); return 1; }
    uint8_t probe[20];
    if (!sha1("", 0, probe)) { tlog(t, LOG_ERR, "SHA-1 is not available on this system"); return 1; }

    bt *b = xcalloc(1, sizeof *b);
    b->t = t;
    b->s = s;
    b->cfg = cfg;
    b->out_dir = cfg->out_dir ? cfg->out_dir : ".";
    b->now = b->t0 = now_sec();
    rand_bytes(&b->rng, sizeof b->rng);
    b->rng |= 1;
    rand_bytes(&b->key, sizeof b->key);
    memcpy(b->pid, "-UL1100-", 8);
    for (int i = 8; i < 20; i++) b->pid[i] = (uint8_t)"0123456789abcdefghijklmnopqrstuvwxyz"[bt_rand(b) % 36];
    b->c = xcalloc(MAX_CANDS, sizeof(cand));
    b->iobuf = xmalloc(131072 + 64);
    b->d.s = INVALID_SOCKET;

    int rc = 1;
    if (is_magnet(source)) {
        magnet mg;
        char err[200];
        if (magnet_parse(source, &mg, err, sizeof err) != 0) { tlog(t, LOG_ERR, "%s", err); goto done; }
        memcpy(b->ih, mg.ih, 20);
        if (mg.dn && *mg.dn) b->dn = xstrdup(mg.dn);
        for (int i = 0; i < mg.ntr; i++) tracker_add(b, mg.tr[i]);
        for (int i = 0; i < mg.npe; i++) {
            baddr a;
            if (addr_parse(mg.pe[i], &a)) cand_add(b, &a, SRC_LINK);
        }
        magnet_free(&mg);
    } else {
        if (load_torrent_file(b, source, data, len) != 0) goto done;
    }
    hex_encode(b->ih, 20, b->ihex);

    /* saved progress for this info-hash? */
    char *sp = state_find(b->out_dir, b->ih);
    bt_state st = { 0 };
    int have_state = sp && state_load(sp, b->ih, &st) == 0;
    if (sp && !have_state) tlog(t, LOG_WARN, "the saved progress (%s) is damaged - ignoring it", path_basename(sp));
    if (have_state && !b->have_meta) {
        char err[300];
        if (meta_parse(&b->m, st.info, st.info_len, err, sizeof err) == 0) b->have_meta = 1;
        else { state_clear(&st); have_state = 0; }
    }
    if (have_state && sp) {
        /* the state file belongs to the name it was saved under */
        const char *bn = path_basename(sp);
        size_t bl = strlen(bn);
        if (bl > 5 && !str_iends(bn, ".ulbt")) bl = 0;
        if (bl > 5) {                       /* (a file named just ".ulbt": the name saved in it) */
            free(st.name);
            st.name = xstrndup(bn, bl - 5);
        }
    }
    free(sp);

    if (!b->have_meta || !b->m.priv) dht_open(b);
    if (have_state) {                       /* peers and DHT nodes that worked last time */
        baddr a;
        for (size_t i = 0; i + 6 <= st.peers_len; i += 6) { addr_v4(&a, st.peers + i, rd16(st.peers + i + 4)); cand_add(b, &a, SRC_LINK); }
        for (size_t i = 0; i + 18 <= st.peers6_len; i += 18) { addr_v6(&a, st.peers6 + i, rd16(st.peers6 + i + 16)); cand_add(b, &a, SRC_LINK); }
        for (size_t i = 0; b->dht_on && i + 26 <= st.nodes_len; i += 26) {
            addr_v4(&a, st.nodes + i + 20, rd16(st.nodes + i + 24));
            if (a.port && addr_public4(a.ip)) dht_add(b, st.nodes + i, &a);
        }
    }
    if (!b->have_meta) {
        tlog(t, LOG_HEAD, "%s", b->dn ? b->dn : "magnet link");
        tlog(t, LOG_DIM, "  info-hash %s  |  %d tracker%s%s", b->ihex, b->ntr, b->ntr == 1 ? "" : "s",
             b->dht_on ? "  |  DHT" : "");
        tlog(t, LOG_INFO, "  fetching the torrent's metadata from peers...");
        task_set_file(t, b->dn ? b->dn : "magnet link", NULL);
    } else {
        int r = setup_meta(b, have_state ? st.name : NULL, have_state ? st.bits : NULL, have_state ? st.bits_len : 0);
        state_clear(&st);
        if (r == -2) {
            tlog(t, LOG_WARN, "paused - start it again to resume.");
            rc = 2;
            goto done;
        }
        if (r != 0) { tlog(t, LOG_ERR, "%s", b->fatal_msg); goto done; }
        if (b->ntr == 0 && !b->dht_on) {
            tlog(t, LOG_ERR, "this private torrent has no trackers - there is no way to find peers");
            goto done;
        }
    }
    state_clear(&st);
    if (b->have_meta && b->nhave == b->m.np) {
        if (b->state_path) file_delete(b->state_path);
        stor_finish(&b->m);
        tlog(t, LOG_OK, "  Already downloaded: %s", b->target);
        b->t->done = b->t->total = b->m.total;
        rc = 0;
        goto done;
    }
    rc = bt_main(b);
done:
    bt_free(b);
    return rc;
}
