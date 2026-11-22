/*
 * U-loader - segmented multi-connection download engine.
 *
 * The file is split into segments, one per connection. Whenever a
 * connection finishes its segment it takes over the second half of the
 * largest remaining segment ("dynamic segmentation"),
 * so every connection stays busy until the very last byte.
 *
 * Progress is checkpointed to "<file>.ulstate" so an interrupted download
 * (pause, crash, network loss) resumes where it stopped.
 */
#include "uloader.h"
#include <process.h>
#include <winioctl.h>

#define BUF_SIZE   (256 * 1024)
#define MIN_SPLIT  (512 * 1024)    /* never create segments smaller than this */
#define MAX_SEGS   128

/* Splitting is only safe while MIN_SPLIT > one read: a worker that is
   mid-write can never overlap the half that was split off. */
typedef char split_guard[(MIN_SPLIT > BUF_SIZE) ? 1 : -1];

typedef struct {
    int64_t pos;     /* next byte to download        */
    int64_t end;     /* exclusive end of the segment */
    int     owner;   /* worker id, -1 = nobody       */
} seg_t;

typedef struct dl_ctx dl_ctx;

typedef struct {
    dl_ctx  *c;
    int      id;
    int      seg;    /* initial segment, -1 = none   */
    HANDLE   thread;
} worker_t;

struct dl_ctx {
    http_session     *s;
    ul_task          *t;
    const ul_config  *cfg;
    const dl_job     *job;
    const char       *url;            /* URL the workers use (after redirects) */
    volatile LONG     use_orig;       /* redirect target expired: use job->url */
    HANDLE            file;
    CRITICAL_SECTION  lock;
    seg_t             segs[MAX_SEGS];
    int               nsegs;
    int64_t           total;          /* -1 = unknown */
    int               ranges;         /* server supports byte ranges */
    http_response    *handoff;        /* probe response reused by worker 0 */
    volatile LONG64   sess_bytes;     /* bytes downloaded in this session */
    volatile LONG64   thr_bytes;      /* bytes counted by the speed limiter */
    double            t0;
    volatile LONG     fatal;
    volatile LONG     changed;        /* file changed on the server: start over next time */
    volatile LONG     range_refused;  /* a connection was refused a byte range */
    volatile LONG     nreq;           /* range requests made (the file came in pieces) */
    char              last_err[256];
    char             *etag;
    char             *lastmod;
};

#define ABORTED(c) (task_aborted((c)->t) || (c)->fatal)

/* a wait that also ends when another connection hit a fatal error */
static void ctx_sleep(dl_ctx *c, DWORD ms)
{
    while (ms > 0 && !ABORTED(c)) {
        DWORD step = ms > 100 ? 100 : ms;
        Sleep(step);
        ms -= step;
    }
}

/* ------------------------------------------------------------------ */
static void set_err(dl_ctx *c, const char *msg)
{
    EnterCriticalSection(&c->lock);
    snprintf(c->last_err, sizeof c->last_err, "%s", msg);
    LeaveCriticalSection(&c->lock);
}

static int64_t remaining_locked(dl_ctx *c)
{
    int64_t rem = 0;
    for (int i = 0; i < c->nsegs; i++)
        if (c->segs[i].end > c->segs[i].pos) rem += c->segs[i].end - c->segs[i].pos;
    return rem;
}

static int write_at(HANDLE f, const char *buf, DWORD n, int64_t off)
{
    while (n) {
        OVERLAPPED ov;
        ZeroMemory(&ov, sizeof ov);
        ov.Offset = (DWORD)(off & 0xFFFFFFFF);
        ov.OffsetHigh = (DWORD)((uint64_t)off >> 32);
        DWORD w = 0;
        if (!WriteFile(f, buf, n, &w, &ov) || w == 0) return 0;
        buf += w;
        n -= w;
        off += w;
    }
    return 1;
}

static void throttle(dl_ctx *c, int64_t n)
{
    int64_t lim = c->cfg->speed_limit;       /* -l: this download */
    if (lim > 0) {
        LONG64 tot = InterlockedExchangeAdd64(&c->thr_bytes, n) + n;
        double due = c->t0 + (double)tot / (double)lim;
        double now = now_sec();
        if (due > now) {
            double ms = (due - now) * 1000.0;
            ctx_sleep(c, (DWORD)(ms > 5000 ? 5000 : ms));
        }
    }
    speed_limit_take(c->t, n);               /* the window's limiter: all downloads together (#2) */
}

/* Give a finished worker something to do: an orphaned segment, or the
   back half of the biggest segment still in progress. */
static int acquire_segment(dl_ctx *c, int id)
{
    int best = -1, slot = -1;
    int64_t best_rem = 0;
    EnterCriticalSection(&c->lock);
    if (!c->ranges) goto none;
    for (int i = 0; i < c->nsegs; i++) {
        int64_t rem = c->segs[i].end - c->segs[i].pos;
        if (c->segs[i].owner < 0 && rem > best_rem) { best = i; best_rem = rem; }
    }
    if (best >= 0) {
        c->segs[best].owner = id;
        LeaveCriticalSection(&c->lock);
        return best;
    }
    for (int i = 0; i < c->nsegs; i++) {
        int64_t rem = c->segs[i].end - c->segs[i].pos;
        if (c->segs[i].owner >= 0 && rem > best_rem) { best = i; best_rem = rem; }
    }
    if (best < 0 || best_rem < 2 * (int64_t)MIN_SPLIT) goto none;
    for (int i = 0; i < c->nsegs; i++)
        if (c->segs[i].owner < 0 && c->segs[i].pos >= c->segs[i].end) { slot = i; break; }
    if (slot < 0) {
        if (c->nsegs >= MAX_SEGS) goto none;
        slot = c->nsegs++;
    }
    int64_t mid = c->segs[best].pos + best_rem / 2;
    c->segs[slot].pos = mid;
    c->segs[slot].end = c->segs[best].end;
    c->segs[slot].owner = id;
    c->segs[best].end = mid;
    LeaveCriticalSection(&c->lock);
    return slot;
none:
    LeaveCriticalSection(&c->lock);
    return -1;
}

static void release_segment(dl_ctx *c, int si)
{
    EnterCriticalSection(&c->lock);
    c->segs[si].owner = -1;
    LeaveCriticalSection(&c->lock);
}

enum { FETCH_OK, FETCH_RETRY, FETCH_GIVEUP, FETCH_STOP };

/* Did the file change since the download started? The ETag of a range
   reply must differ from the first reply's. Load-balanced servers may
   disagree on ETags (inode based), so if both replies carry Last-Modified
   that must differ too. Last-Modified alone is not trusted: dynamic
   servers stamp every reply with the current time. */
static int changed_on_server(dl_ctx *c, const http_response *r)
{
    int have_e = c->etag && *c->etag && r->etag && *r->etag;
    int have_l = c->lastmod && *c->lastmod && r->last_modified && *r->last_modified;
    if (!have_e || strcmp(c->etag, r->etag) == 0) return 0;
    return !have_l || strcmp(c->lastmod, r->last_modified) != 0;
}

/* One HTTP request for segment si: stream bytes until the segment (or the
   request chunk) is finished, the connection drops, or we are told to stop. */
static int fetch_once(dl_ctx *c, int si, char *buf, DWORD bufsz, char *err, size_t errlen)
{
    http_response r;
    int64_t pos, end, req_to = -1;

    EnterCriticalSection(&c->lock);
    pos = c->segs[si].pos;
    end = c->segs[si].end;
    LeaveCriticalSection(&c->lock);

    if (c->handoff && pos == 0) {
        r = *c->handoff;               /* keep streaming the probe response */
        free(c->handoff);
        c->handoff = NULL;
        if (c->ranges) req_to = end - 1;
    } else {
        int64_t from = -1;
        if (c->ranges) {
            from = pos;
            req_to = end - 1;
            if (c->job->chunk_size > 0 && req_to - from + 1 > c->job->chunk_size)
                req_to = from + c->job->chunk_size - 1;
        } else if (pos > 0) {
            /* no range support: a broken transfer must start over */
            EnterCriticalSection(&c->lock);
            c->segs[si].pos = 0;
            LeaveCriticalSection(&c->lock);
            pos = 0;
        }
        const char *u = c->use_orig ? c->job->url : c->url;
        InterlockedIncrement(&c->nreq);
        int rq = http_request(c->s, u, c->job->headers, from, req_to, 0, &r, err, errlen);
        if (rq != 0) return rq == -2 ? FETCH_GIVEUP : FETCH_RETRY;
        int ok;
        if (c->ranges)
            ok = (r.status == 206 && r.range_start == from &&
                  (r.range_total < 0 || r.range_total == c->total)) ||
                 (r.status == 200 && from == 0 && (r.content_length < 0 || r.content_length == c->total));
        else   /* a 206 without a Content-Range is a slice of unknown place, not the file */
            ok = r.status == 200 || (r.status == 206 && r.range_start == 0);
        /* the same file? (a file replaced on the server must not be mixed in) */
        if (ok && c->ranges && changed_on_server(c, &r)) {
            snprintf(err, errlen, "the file on the server changed during the download");
            http_close(&r);
            InterlockedExchange(&c->changed, 1);
            InterlockedExchange(&c->fatal, 1);
            return FETCH_STOP;
        }
        if (!ok) {
            int st = r.status;
            if (st == 206 && r.range_total >= 0 && r.range_total != c->total) {
                snprintf(err, errlen, "the file on the server changed size");
                http_close(&r);
                InterlockedExchange(&c->changed, 1);
                InterlockedExchange(&c->fatal, 1);
                return FETCH_STOP;
            }
            if (st == 200 || st == 206) {
                snprintf(err, errlen, "server ignored the byte range");
                InterlockedExchange(&c->range_refused, 1);
            }
            else snprintf(err, errlen, "HTTP %d", st);
            http_close(&r);
            /* an expired redirect target: go back through the original link */
            if (st >= 400 && st < 500 && !c->use_orig && strcmp(c->url, c->job->url) != 0) {
                InterlockedExchange(&c->use_orig, 1);
                return FETCH_RETRY;
            }
            /* 4xx (except timeout / rate-limit) and range refusals will not fix themselves */
            if ((st >= 400 && st < 500 && st != 408 && st != 429) || st == 200 || st == 206)
                return FETCH_GIVEUP;
            return FETCH_RETRY;
        }
    }

    int result = FETCH_OK;
    int64_t got_total = 0;
    for (;;) {
        DWORD got = 0;
        if (http_read(&r, buf, bufsz, &got) != 0) {
            win_errstr(GetLastError(), err, errlen);
            result = got_total > 0 && c->ranges ? FETCH_OK : FETCH_RETRY;
            break;
        }
        if (got == 0) {                                   /* end of response */
            EnterCriticalSection(&c->lock);
            pos = c->segs[si].pos;
            end = c->segs[si].end;
            if (c->total < 0) {                           /* size was unknown */
                c->segs[si].end = pos;
                c->total = pos;
                LeaveCriticalSection(&c->lock);
            } else {
                LeaveCriticalSection(&c->lock);
                if (pos < end && (req_to < 0 || pos <= req_to)) {
                    /* dropped before the requested range was complete; with
                       range support we just continue from where it stopped */
                    snprintf(err, errlen, "connection closed early");
                    result = (c->ranges && got_total > 0) ? FETCH_OK : FETCH_RETRY;
                }
            }
            break;
        }
        int64_t take = got;
        EnterCriticalSection(&c->lock);
        int64_t avail = c->segs[si].end - c->segs[si].pos;
        pos = c->segs[si].pos;
        LeaveCriticalSection(&c->lock);
        if (take > avail) take = avail;
        if (take > 0) {
            if (!write_at(c->file, buf, (DWORD)take, pos)) {
                char e[200];
                snprintf(err, errlen, "cannot write to disk: %s", win_errstr(GetLastError(), e, sizeof e));
                InterlockedExchange(&c->fatal, 1);
                result = FETCH_STOP;
                break;
            }
            EnterCriticalSection(&c->lock);
            c->segs[si].pos += take;
            pos = c->segs[si].pos;
            end = c->segs[si].end;
            LeaveCriticalSection(&c->lock);
            got_total += take;
            InterlockedExchangeAdd64(&c->sess_bytes, take);
            throttle(c, take);
        }
        if (take < (int64_t)got || pos >= end) {          /* segment done or shrunk */
            if (take < (int64_t)got && c->total >= 0 && pos >= c->total)   /* past the end of the file */
                snprintf(err, errlen, "server sent more data than announced");
            break;
        }
        if (ABORTED(c)) { result = FETCH_STOP; break; }
    }
    http_close(&r);
    return result;
}

static unsigned __stdcall worker_main(void *arg)
{
    worker_t *w = arg;
    dl_ctx *c = w->c;
    /* small reads under a low limit keep the speed even (no bursts) */
    int64_t glim = speed_limit_get();
    DWORD bufsz = ((c->cfg->speed_limit > 0 && c->cfg->speed_limit < 4 * BUF_SIZE) ||
                   (glim > 0 && glim < 4 * BUF_SIZE)) ? 16384 : BUF_SIZE;
    char *buf = xmalloc(bufsz);
    char err[256];
    int si = w->seg, attempt = 0;
    int64_t best = -1;      /* furthest position reached in this segment */

    while (!ABORTED(c)) {
        if (si < 0) {
            si = acquire_segment(c, w->id);
            if (si < 0) break;
            attempt = 0;
            best = -1;
        }
        EnterCriticalSection(&c->lock);
        int done = c->segs[si].pos >= c->segs[si].end;
        int64_t start_pos = c->segs[si].pos;
        LeaveCriticalSection(&c->lock);
        if (done) { release_segment(c, si); si = -1; continue; }
        if (best < start_pos) best = start_pos;

        err[0] = 0;
        int r = fetch_once(c, si, buf, bufsz, err, sizeof err);
        if (r == FETCH_STOP) { if (c->fatal && err[0]) set_err(c, err); break; }

        /* Only real forward progress resets the retry budget. Without range
           support every retry starts again at byte 0, which is not progress. */
        EnterCriticalSection(&c->lock);
        int64_t now_pos = c->segs[si].pos, now_end = c->segs[si].end;
        LeaveCriticalSection(&c->lock);
        int advanced = now_pos > best;
        if (advanced) best = now_pos;
        if (r == FETCH_OK && (advanced || now_pos >= now_end)) { attempt = 0; continue; }
        if (r == FETCH_OK) r = FETCH_RETRY;
        if (advanced) attempt = 0;
        set_err(c, err[0] ? err : "connection closed early");
        attempt++;
        if (r == FETCH_GIVEUP && attempt >= 2) break;
        if (attempt > c->cfg->retries) break;
        ctx_sleep(c, (DWORD)(attempt < 16 ? 500 * attempt : 8000));
    }
    if (si >= 0) release_segment(c, si);   /* let another connection finish it */
    free(buf);
    return 0;
}

/* ------------------------------------------------------------------ */
/* resume state                                                        */
/* ------------------------------------------------------------------ */
static void save_state(dl_ctx *c, const char *path)
{
    /* Snapshot first, flush second: a worker moves a segment forward only
       after its bytes were written, so everything this snapshot claims is
       in the file and the flush puts it on the disk. (Flushing first let
       the state claim bytes written after the flush - holes after a power
       cut.) */
    seg_t snap[MAX_SEGS];
    EnterCriticalSection(&c->lock);
    int n = c->nsegs;
    memcpy(snap, c->segs, sizeof(seg_t) * (size_t)n);
    int64_t total = c->total;
    LeaveCriticalSection(&c->lock);
    if (!FlushFileBuffers(c->file)) return;          /* keep the older, still true state */

    char *tmp = xasprintf("%s.tmp", path);
    FILE *f = fopen_u8(tmp, "wb");
    if (!f) { free(tmp); return; }
    fprintf(f, "ULSTATE 1\nurl %s\ntotal %lld\netag %s\nlastmod %s\n", c->job->url,
            (long long)total, c->etag ? c->etag : "", c->lastmod ? c->lastmod : "");
    for (int i = 0; i < n; i++)
        if (snap[i].pos < snap[i].end)
            fprintf(f, "seg %lld %lld\n", (long long)snap[i].pos, (long long)snap[i].end);
    fprintf(f, "end\n");
    int ok = fflush(f) == 0;
    if (fclose(f) != 0) ok = 0;
    if (ok) file_rename(tmp, path);
    else file_delete(tmp);
    free(tmp);
}

static int load_state(dl_ctx *c, const char *path)
{
    FILE *f = fopen_u8(path, "rb");
    if (!f) return 0;
    size_t cap = 65536;
    char *line = xmalloc(cap);
    int ok = 0, nseg = 0, ended = 0;
    int64_t total = -2;
    seg_t segs[MAX_SEGS];
    if (fgets(line, (int)cap, f) && strncmp(line, "ULSTATE 1", 9) == 0) {
        ok = 1;
        while (fgets(line, (int)cap, f)) {
            char *v = str_trim(line);
            if (strncmp(v, "total ", 6) == 0) {
                total = _strtoi64(v + 6, NULL, 10);
            } else if (strncmp(v, "etag ", 5) == 0 || strcmp(v, "etag") == 0) {
                const char *e = v[4] ? v + 5 : "";
                /* both sides must agree: a changed or vanished ETag means a new file */
                if ((*e || (c->etag && *c->etag)) && (!c->etag || strcmp(e, c->etag) != 0)) ok = 0;
            } else if (strncmp(v, "lastmod ", 8) == 0 || strcmp(v, "lastmod") == 0) {
                const char *e = v[7] ? v + 8 : "";
                if (*e && c->lastmod && *c->lastmod && strcmp(e, c->lastmod) != 0) ok = 0;
            } else if (strncmp(v, "seg ", 4) == 0) {
                long long a, b;
                if (nseg >= MAX_SEGS || sscanf(v + 4, "%lld %lld", &a, &b) != 2 ||
                    a < 0 || b <= a || b > c->total) { ok = 0; break; }
                for (int i = 0; i < nseg; i++)      /* reject overlapping ranges */
                    if (a < segs[i].end && segs[i].pos < b) ok = 0;
                segs[nseg].pos = a;
                segs[nseg].end = b;
                segs[nseg].owner = -1;
                nseg++;
            } else if (strcmp(v, "end") == 0) {
                ended = 1;
            }
        }
    }
    fclose(f);
    free(line);
    if (!ok || !ended || total != c->total) return 0;
    memcpy(c->segs, segs, sizeof(seg_t) * (size_t)nseg);
    c->nsegs = nseg;
    return 1;
}

/* ------------------------------------------------------------------ */
/* progress                                                            */
/* ------------------------------------------------------------------ */
#define SPEED_SAMPLES 12

typedef struct {
    double  t[SPEED_SAMPLES];
    int64_t b[SPEED_SAMPLES];
    int     n, head;
} speedo;

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

/* Publish progress to the task. The segment map shows the holes being
   filled in by the different connections. */
static void publish(dl_ctx *c, double speed)
{
    seg_t snap[MAX_SEGS];
    int n, active = 0;
    int64_t total, rem;
    EnterCriticalSection(&c->lock);
    n = c->nsegs;
    memcpy(snap, c->segs, sizeof(seg_t) * (size_t)n);
    total = c->total;
    rem = remaining_locked(c);
    LeaveCriticalSection(&c->lock);
    for (int i = 0; i < n; i++) if (snap[i].owner >= 0 && snap[i].pos < snap[i].end) active++;

    ul_task *t = c->t;
    unsigned char map[UL_SEGVIEW];
    int64_t done;
    if (total > 0) {
        done = total - rem;
        for (int i = 0; i < UL_SEGVIEW; i++) {
            int64_t a = share_of(total, i, UL_SEGVIEW), b = share_of(total, i + 1, UL_SEGVIEW), miss = 0;
            for (int k = 0; k < n; k++) {
                int64_t lo = snap[k].pos > a ? snap[k].pos : a;
                int64_t hi = snap[k].end < b ? snap[k].end : b;
                if (hi > lo) miss += hi - lo;
            }
            map[i] = b > a ? (unsigned char)(255 - (miss * 255) / (b - a)) : 255;
        }
    } else {
        done = n ? snap[0].pos : 0;
        memset(map, 0, sizeof map);
    }
    EnterCriticalSection(&t->lock);
    memcpy(t->segmap, map, sizeof map);
    LeaveCriticalSection(&t->lock);
    t->done = done;
    t->total = total;
    t->speed = (LONG64)speed;
    t->eta = (total > 0 && speed > 1) ? (LONG64)((double)rem / speed) : -1;
    t->conns = active;
    task_progress(t);
}

static void set_sparse(HANDLE h, BOOLEAN on)
{
    struct { BOOLEAN SetSparse; } sb = { on };
    DWORD ret;
    DeviceIoControl(h, FSCTL_SET_SPARSE, &sb, sizeof sb, NULL, 0, &ret, NULL);
}

/* ------------------------------------------------------------------ */
/* Without an ETag nothing proves the server still has the file we started
   on. Compare one already-downloaded piece (up to 64 KB) with the server.
   Returns 0 only when it definitely differs. */
static int sample_matches(dl_ctx *c, const char *part)
{
    seg_t s[MAX_SEGS];
    int n = c->nsegs;
    memcpy(s, c->segs, sizeof(seg_t) * (size_t)n);
    for (int i = 1; i < n; i++)                       /* sort by position */
        for (int k = i; k > 0 && s[k].pos < s[k - 1].pos; k--) { seg_t x = s[k]; s[k] = s[k - 1]; s[k - 1] = x; }
    int64_t cursor = 0, from = -1, len = 0;
    for (int i = 0; i < n; i++) {                     /* first finished stretch */
        if (s[i].pos >= s[i].end) continue;
        if (s[i].pos > cursor) { from = cursor; len = s[i].pos - cursor; break; }
        if (s[i].end > cursor) cursor = s[i].end;
    }
    if (from < 0 && cursor < c->total) { from = cursor; len = c->total - cursor; }
    if (from < 0 || len <= 0) return 1;
    if (len > 65536) len = 65536;

    char err[256];
    size_t got = 0;
    char *remote = http_get_body(c->s, c->url, c->job->headers, from, from + len - 1, 0, &got, NULL, 2,
                                 err, sizeof err, NULL);
    if (!remote) return 1;                            /* cannot tell: keep the progress */
    char *local = xmalloc((size_t)len);
    FILE *f = fopen_u8(part, "rb");
    int same = 1;
    if (f && _fseeki64(f, from, SEEK_SET) == 0 && fread(local, 1, (size_t)len, f) == (size_t)len)
        same = got == (size_t)len && memcmp(local, remote, (size_t)len) == 0;
    if (f) fclose(f);
    free(local);
    free(remote);
    return same;
}

/* Without an ETag nothing told us the file stayed the same while it came
   in pieces (several connections, retries, an earlier run). Compare a few
   spots of the finished file with what the server has now: a file that
   was replaced meanwhile is a mix of two versions and must not be kept.
   Returns 0 only when a spot definitely differs. */
static int spots_match(dl_ctx *c, const char *part)
{
    if (c->total <= 0) return 1;
    FILE *f = fopen_u8(part, "rb");
    if (!f) return 1;
    enum { SPOT = 4096, SPOTS = 8 };
    char *local = xmalloc(SPOT);
    int same = 1;
    int64_t last = -1;
    for (int k = 0; k < SPOTS && same && !task_aborted(c->t); k++) {
        int64_t from = share_of(c->total, k, SPOTS);
        if (from == last) continue;
        last = from;
        int64_t len = c->total - from < SPOT ? c->total - from : SPOT;
        if (len <= 0) continue;
        char err[256];
        size_t got = 0;
        char *remote = http_get_body(c->s, c->url, c->job->headers, from, from + len - 1, 0, &got, NULL, 1,
                                     err, sizeof err, NULL);
        if (!remote) continue;                        /* cannot tell from this spot */
        if (_fseeki64(f, from, SEEK_SET) == 0 && fread(local, 1, (size_t)len, f) == (size_t)len)
            same = got == (size_t)len && memcmp(local, remote, (size_t)len) == 0;
        free(remote);
    }
    free(local);
    fclose(f);
    return same;
}

/* One complete attempt. *fallback is set when the server refused byte
   ranges to the extra connections: the caller retries with one connection. */
static int dl_run_once(http_session *s, const ul_config *cfg, const dl_job *job,
                       http_response *probe_in, int *fallback)
{
    ul_task *t = http_task(s);
    char err[256], a[32], b[32];
    http_response probe;
    *fallback = 0;
    err[0] = 0;
    if (probe_in) {
        probe = *probe_in;
    } else {
        int ok = 0;
        for (int attempt = 0; attempt <= 3 && !task_aborted(t); attempt++) {
            if (attempt) task_sleep(t, 1000);
            int rq = http_request(s, job->url, job->headers, 0, -1, 0, &probe, err, sizeof err);
            if (rq == 0) { ok = 1; break; }
            if (rq == -2) break;
        }
        if (!ok) { tlog(t, LOG_ERR, "%s", task_aborted(t) ? "cancelled" : err); return task_aborted(t) ? 2 : 1; }
    }
    /* A 206 that does not start at byte 0 cannot be streamed into the file
       from the start: ask again for the whole file without a range. */
    if (probe.status == 206 && probe.range_start != 0) {
        http_close(&probe);
        if (http_request(s, job->url, job->headers, -1, -1, 0, &probe, err, sizeof err) != 0) {
            tlog(t, LOG_ERR, "%s", err);
            return task_aborted(t) ? 2 : 1;
        }
        if (probe.status == 206) probe.status = 0;     /* still a partial answer: refuse below */
    }
    if (probe.status != 200 && probe.status != 206) {
        tlog(t, LOG_ERR, "server answered HTTP %d", probe.status);
        http_close(&probe);
        return 1;
    }

    dl_ctx *c = xcalloc(1, sizeof *c);
    InitializeCriticalSection(&c->lock);
    c->s = s;
    c->t = t;
    c->cfg = cfg;
    c->job = job;
    c->file = INVALID_HANDLE_VALUE;
    if (probe.status == 206 && probe.range_total >= 0 && probe.range_start == 0) {
        c->ranges = 1;
        c->total = probe.range_total;
    } else {
        c->ranges = 0;
        c->total = probe.status == 200 ? probe.content_length : -1;
    }
    c->etag = probe.etag ? xstrdup(probe.etag) : NULL;
    c->lastmod = probe.last_modified ? xstrdup(probe.last_modified) : NULL;
    char *worker_url = xstrdup(probe.final_url ? probe.final_url : job->url);
    c->url = worker_url;

    char *part = xasprintf("%s.ulpart", job->path);
    char *state = xasprintf("%s.ulstate", job->path);
    const char *name = path_basename(job->path);
    int resumed = 0, rc = 1;
    int64_t already = 0;
    worker_t *ws = NULL;
    HANDLE th[UL_MAX_CONN];
    int nth = 0;
    task_set_file(t, name, job->path);

    char *dir = path_dirname(job->path);
    dir_create(dir);
    free(dir);

    if (c->ranges && file_exists(state) && file_exists(part)) {
        resumed = load_state(c, state);
        if (!resumed) tlog(t, LOG_WARN, "the file on the server changed (or the saved state is damaged), starting over");
        else if (file_size(part) != c->total) {
            resumed = 0;
            c->nsegs = 0;
            tlog(t, LOG_WARN, "the partial file is damaged, starting over");
        } else if ((!c->etag || !*c->etag) && !sample_matches(c, part)) {
            resumed = 0;
            c->nsegs = 0;
            tlog(t, LOG_WARN, "the file on the server changed since the last run, starting over");
        }
    }
    /* starting fresh: an old checkpoint must never be applied to the new data */
    if (!resumed) file_delete(state);

    /* free space check */
    if (c->total > 0) {
        EnterCriticalSection(&c->lock);
        int64_t need = resumed ? remaining_locked(c) : c->total;
        LeaveCriticalSection(&c->lock);
        int64_t fr = disk_free(job->path);
        if (fr >= 0 && fr < need) {
            tlog(t, LOG_ERR, "not enough disk space (need %s, have %s)",
                 fmt_size(need, a, sizeof a), fmt_size(fr, b, sizeof b));
            http_close(&probe);
            goto out;
        }
    }

    wchar_t *wpart = wpath(part);
    c->file = CreateFileW(wpart, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                          resumed ? OPEN_EXISTING : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(wpart);
    if (c->file == INVALID_HANDLE_VALUE) {
        tlog(t, LOG_ERR, "cannot create %s: %s", part, win_errstr(GetLastError(), err, sizeof err));
        http_close(&probe);
        goto out;
    }
    if (!resumed && c->total > 0) {
        /* sparse + preallocated: writing at any offset is instant (no
           zero-filling of the gap on NTFS) */
        set_sparse(c->file, TRUE);
        LARGE_INTEGER li;
        li.QuadPart = c->total;
        if (!SetFilePointerEx(c->file, li, NULL, FILE_BEGIN) || !SetEndOfFile(c->file)) {
            /* e.g. a FAT32 drive and a file over 4 GB: say so now, not at 99% */
            tlog(t, LOG_ERR, "cannot make room for %s on this drive: %s", fmt_size(c->total, a, sizeof a),
                 win_errstr(GetLastError(), err, sizeof err));
            CloseHandle(c->file);
            c->file = INVALID_HANDLE_VALUE;
            file_delete(part);
            http_close(&probe);
            goto out;
        }
    }

    int conns = c->ranges ? job->connections : 1;
    if (conns > UL_MAX_CONN) conns = UL_MAX_CONN;
    if (c->total >= 0 && conns > c->total / MIN_SPLIT) conns = (int)(c->total / MIN_SPLIT);
    if (conns < 1) conns = 1;   /* small files are not worth splitting */

    if (resumed) {
        http_close(&probe);
        already = c->total - remaining_locked(c);
    } else {
        int n = 1;
        if (c->ranges && c->total > 0) {
            int64_t maxn = c->total / MIN_SPLIT;
            n = conns < maxn ? conns : (int)(maxn > 0 ? maxn : 1);
        }
        c->nsegs = n;
        for (int i = 0; i < n; i++) {
            c->segs[i].pos = c->total >= 0 ? share_of(c->total, i, n) : 0;
            c->segs[i].end = c->total >= 0 ? share_of(c->total, i + 1, n) : INT64_MAX;
            c->segs[i].owner = -1;
        }
        c->handoff = xmalloc(sizeof(http_response));
        *c->handoff = probe;               /* worker 0 keeps reading it */
    }

    /* header */
    tlog(t, LOG_HEAD, "%s", name);
    if (c->total >= 0)
        tlog(t, LOG_DIM, "  %s  |  %s  |  %d connection%s", fmt_size(c->total, a, sizeof a),
             c->ranges ? "resumable" : "not resumable (server has no range support)",
             conns, conns == 1 ? "" : "s");
    else
        tlog(t, LOG_DIM, "  size unknown  |  single connection");
    if (resumed)
        tlog(t, LOG_DIM, "  resuming at %.1f%% (%s already downloaded)",
             c->total ? 100.0 * (double)already / (double)c->total : 100.0, fmt_size(already, a, sizeof a));

    /* checkpoint right away: a crash in the first seconds is still resumable */
    if (c->ranges) save_state(c, state);

    /* start workers */
    ws = xcalloc((size_t)conns, sizeof(worker_t));
    c->t0 = now_sec();
    /* Every first owner is set before any thread runs. Otherwise a worker
       that finishes early "adopts" a segment whose worker has not started
       yet, and two connections write the same bytes. */
    EnterCriticalSection(&c->lock);
    for (int i = 0; i < conns; i++) {
        ws[i].c = c;
        ws[i].id = i;
        ws[i].seg = -1;
        if (i < c->nsegs && c->segs[i].pos < c->segs[i].end) {
            ws[i].seg = i;
            c->segs[i].owner = i;
        }
    }
    LeaveCriticalSection(&c->lock);
    for (int i = 0; i < conns; i++) {
        ws[i].thread = (HANDLE)_beginthreadex(NULL, 0, worker_main, &ws[i], 0, NULL);
        if (ws[i].thread) th[nth++] = ws[i].thread;
        else if (ws[i].seg >= 0) release_segment(c, ws[i].seg);   /* another worker takes it */
    }
    if (nth == 0) {
        tlog(t, LOG_ERR, "cannot start download threads");
        goto out;
    }

    speedo sp = { 0 };
    double last_save = now_sec(), speed = 0;
    int all_done = 0;
    while (!all_done) {
        DWORD wr = WaitForMultipleObjects((DWORD)nth, th, TRUE, 500);
        all_done = wr != WAIT_TIMEOUT;
        double tm = now_sec();
        speed = speedo_add(&sp, tm, c->sess_bytes);
        publish(c, speed);
        if (c->ranges && tm - last_save > 2.0) { save_state(c, state); last_save = tm; }
        if ((task_aborted(t) || c->fatal) && !all_done) {
            /* Stop now: close the connections the workers may be blocked on,
               and wait until every one of them has really exited (they use
               this context, the session and the settings). */
            while (WaitForMultipleObjects((DWORD)nth, th, TRUE, 200) == WAIT_TIMEOUT)
                http_session_cancel(s);
            all_done = 1;
        }
    }

    double elapsed = now_sec() - c->t0;
    EnterCriticalSection(&c->lock);
    int64_t rem = remaining_locked(c);
    LeaveCriticalSection(&c->lock);
    int complete = rem == 0 && !c->fatal && c->total >= 0;

    if (complete) {
        publish(c, elapsed > 0 ? (double)c->sess_bytes / elapsed : 0);
        con_progress_end();
        LARGE_INTEGER li;                 /* make sure the length is exact */
        li.QuadPart = c->total;
        if (SetFilePointerEx(c->file, li, NULL, FILE_BEGIN)) SetEndOfFile(c->file);
        set_sparse(c->file, FALSE);
        /* the file is dated when it was downloaded - now - (created and
           modified), never with the server's "last modified" date */
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        SetFileTime(c->file, &now, NULL, &now);
        FlushFileBuffers(c->file);
        CloseHandle(c->file);
        c->file = INVALID_HANDLE_VALUE;
        if (c->ranges && (!c->etag || !*c->etag) && c->nreq > 0 && !spots_match(c, part)) {
            file_delete(state);
            file_delete(part);
            tlog(t, LOG_ERR, "the file on the server changed during the download (the server gives no version "
                             "tag to notice that sooner) - the mixed data was discarded; start it again to get "
                             "the new version.");
            rc = 1;
        } else if (file_rename(part, job->path) != 0) {
            tlog(t, LOG_ERR, "download finished but could not rename %s: %s", part,
                 win_errstr(GetLastError(), err, sizeof err));
            rc = 1;
        } else {
            file_delete(state);
            file_meta_write(job->path, c->etag, c->lastmod);   /* for "already downloaded" checks */
            tlog(t, LOG_OK, "  Done: %s in %.1fs (avg %s/s)", fmt_size(c->total, a, sizeof a), elapsed,
                 fmt_size((int64_t)(elapsed > 0 ? (double)c->sess_bytes / elapsed : 0), b, sizeof b));
            tlog(t, LOG_DIM, "  Saved to %s", job->path);
            rc = 0;
        }
    } else {
        con_progress_end();
        int aborted = task_aborted(t) && !c->fatal;
        if (c->changed) {
            /* never mix two versions of a file: throw the partial data away */
            CloseHandle(c->file);
            c->file = INVALID_HANDLE_VALUE;
            file_delete(state);
            file_delete(part);
            tlog(t, LOG_ERR, "%s - the partial download was discarded; start it again to get the new version.",
                 c->last_err[0] ? c->last_err : "the file on the server changed");
            rc = 1;
        } else if (!aborted && c->range_refused && job->connections > 1 && c->ranges) {
            /* the server only lets one connection read the file */
            CloseHandle(c->file);
            c->file = INVALID_HANDLE_VALUE;
            file_delete(state);
            file_delete(part);
            *fallback = 1;
            rc = 1;
        } else {
            if (c->ranges) save_state(c, state);
            if (aborted) {
                tlog(t, LOG_WARN, c->ranges ? "paused - start it again to resume."
                                            : "stopped - this server cannot resume, so the next start begins again.");
                rc = 2;
            } else {
                tlog(t, LOG_ERR, "download failed: %s", c->last_err[0] ? c->last_err : "unknown error");
                if (c->ranges && !c->fatal) tlog(t, LOG_INFO, "  Progress is saved - start it again to resume.");
                rc = 1;
            }
        }
    }

out:
    for (int i = 0; i < nth; i++) CloseHandle(th[i]);
    free(ws);
    if (c->handoff) { http_close(c->handoff); free(c->handoff); }
    if (c->file != INVALID_HANDLE_VALUE) CloseHandle(c->file);
    DeleteCriticalSection(&c->lock);
    free(c->etag);
    free(c->lastmod);
    free(worker_url);
    free(c);
    free(part);
    free(state);
    return rc;
}

int dl_run(http_session *s, const ul_config *cfg, const dl_job *job, http_response *probe_in)
{
    int fallback = 0;
    int rc = dl_run_once(s, cfg, job, probe_in, &fallback);
    if (rc == 1 && fallback && !task_aborted(http_task(s))) {
        tlog(http_task(s), LOG_WARN, "the server allows only one connection for this file - continuing with one");
        dl_job one = *job;
        one.connections = 1;
        rc = dl_run_once(s, cfg, &one, NULL, &fallback);
    }
    return rc;
}
