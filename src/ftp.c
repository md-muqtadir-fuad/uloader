/*
 * U-loader - FTP downloads (plain FTP: RFC 959, 2428, 3659).
 *
 * Plugs into the HTTP interface: ftp_request() opens its own control
 * connection, logs in, emulates a byte range with SIZE + REST and starts
 * one RETR over a passive data connection (EPSV, falling back to PASV).
 * The engine therefore gets parallel, resumable FTP downloads exactly as
 * it does for HTTP.
 *
 * Every wait is a select() of at most FTP_SLICE_MS, so a pause is noticed
 * within a fraction of a second, and the network timeout of the settings
 * applies to every single step (connect, each reply, each read).
 *
 * Set ULOADER_FTP_TRACE=1 to log the control dialogue to stderr.
 */
#include "uloader.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <process.h>

#define FTP_LINE_MAX    8192        /* longest reply line accepted            */
#define FTP_REPLY_MAX   (64 * 1024) /* longest (multi-line) reply accepted    */
#define FTP_REPLY_LINES 1000
#define FTP_TEXT_MAX    512         /* reply text kept for parsing / messages */
#define FTP_CMD_MAX     8192
#define FTP_SLICE_MS    200         /* how often waits look at the pause flag */
#define FTP_MAX_ADDRS   6           /* addresses tried per host name          */
#define FTP_BUSY_WAIT   30          /* s: wait for a free slot after a 421    */

#define ERR_CANCELLED   ERROR_OPERATION_ABORTED
#define ERR_PROTOCOL    ERROR_INVALID_DATA

enum { W_TIMEOUT = 0, W_A = 1, W_B = 2 };   /* wait_sock() results (or -1) */

typedef union {
    struct sockaddr         sa;
    struct sockaddr_in      v4;
    struct sockaddr_in6     v6;
    struct sockaddr_storage ss;
} sockaddr_any;

/* One server ("host:port"). Servers often cap the connections per client:
   after a "too many connections" reply the cap is learned, and further
   sessions queue here until one of ours closes, instead of hammering the
   server with logins (which can also get the address banned). */
typedef struct ftp_host {
    struct ftp_host *next;
    char  *key;
    int    logged_in;       /* our sessions past the login              */
    int    connecting;      /* our sessions being set up                */
    int    limit;           /* learned cap, 0 = none                    */
    double limit_t;         /* last "too many connections" reply        */
    double logout_t;        /* last time one of our sessions ended      */
    sockaddr_any good;      /* address that worked last time            */
    int    goodlen;
    double good_t;
    volatile LONG no_epsv;  /* EPSV is missing or its ports are unreachable */
} ftp_host;

#define FTP_LIMIT_KEEP  120.0   /* s: forget a learned cap after this long */
#define FTP_SLOT_GRACE  0.25    /* s: let the server notice a closed session */
#define FTP_ADDR_KEEP   300.0   /* s: reuse the last working address       */

typedef struct {
    http_session *s;
    ftp_host *host;
    SOCKET    ctl, data;
    int       tmo_ms;
    unsigned  id;
    sockaddr_any peer;              /* server address of the control connection */
    int       peerlen;
    char      in[4096];             /* control connection read buffer */
    int       inpos, inlen;
    char      text[FTP_TEXT_MAX];   /* text of the last reply */
    int       wsa;                  /* error code of the last failure */
    char      msg[256];             /* message of the last failure */
    int       busy;                 /* last failure was "too many connections" */
    int       admitted;             /* included in host->connecting */
    int       counted;              /* included in host->logged_in */
    int       no_epsv;
    int64_t   remaining;            /* bytes still to hand over, -1 = until the server ends */
    int       to_eof;               /* the range ends at the end of the file */
    int       xfer;                 /* RETR running on the server */
    int       complete;             /* every requested byte was delivered */
    int       done;                 /* end of data reported */
    int       ctl_dead;             /* control connection lost during the transfer */
    int       final;                /* final transfer reply (226...), 0 = not read */
    DWORD     fail;                 /* sticky read error */
} ftp_conn;

typedef struct {
    char *user, *pass, *host, *port;
    char *full;     /* "/dir/name" */
    char *dir;      /* "dir" (relative to the login folder, RFC 1738), "" = none */
    char *name;     /* "name" */
    int   utf8;     /* the path has non-ASCII bytes */
} ftp_url;

typedef struct {
    ftp_url     u;
    int         mode;       /* 0 = full path, 1 = CWD dir + bare name */
    int         no_fallback;
    const char *target;     /* argument for SIZE / MDTM / RETR */
    int64_t     size;       /* -1 = unknown */
    int         size_ok;    /* SIZE is implemented */
    int         want_mdtm;
    char       *mdtm;       /* HTTP date */
} ftp_req;

/* ------------------------------------------------------------------ */
/* global state                                                        */
/* ------------------------------------------------------------------ */
static INIT_ONCE          g_once = INIT_ONCE_STATIC_INIT;
static int                g_wsa_rc = -1;
static int                g_trace;
static volatile LONG      g_ids;
static CRITICAL_SECTION   g_hlock;
static CONDITION_VARIABLE g_hcv;
static ftp_host          *g_hosts;

static BOOL CALLBACK net_init_once(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    WSADATA wd;
    g_wsa_rc = WSAStartup(MAKEWORD(2, 2), &wd);
    InitializeCriticalSection(&g_hlock);
    InitializeConditionVariable(&g_hcv);
    const char *t = getenv("ULOADER_FTP_TRACE");
    g_trace = t && *t && strcmp(t, "0") != 0;
    return TRUE;
}

static int net_ready(char *err, size_t errlen)
{
    InitOnceExecuteOnce(&g_once, net_init_once, NULL, NULL);
    if (g_wsa_rc == 0) return 1;
    char e[160];
    snprintf(err, errlen, "cannot start Windows networking: %s", win_errstr((DWORD)g_wsa_rc, e, sizeof e));
    return 0;
}

static void trace(ftp_conn *c, char dir, const char *line)
{
    if (g_trace) fprintf(stderr, "[ftp %u] %c %s\n", c->id, dir, line);
}

/* ------------------------------------------------------------------ */
/* errors                                                              */
/* ------------------------------------------------------------------ */
static int ferr(ftp_conn *c, int code, const char *fmt, ...) __attribute__((format(gnu_printf, 3, 4)));
static int ferr(ftp_conn *c, int code, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->msg, sizeof c->msg, fmt, ap);
    va_end(ap);
    c->wsa = code;
    trace(c, '!', c->msg);
    return -1;
}

static int neterr(ftp_conn *c, int code, const char *what)
{
    char e[160];
    win_errstr((DWORD)code, e, sizeof e);
    return ferr(c, code, "%s: %s", what, e);
}

/* "<what> (550 No such file)" */
static int refused(ftp_conn *c, int code, const char *what)
{
    return ferr(c, ERR_PROTOCOL, "%s (%d %.150s)", what, code, c->text);
}

static int has_busy_text(const char *t)
{
    return str_istr(t, "too many") || str_istr(t, "maximum number") || str_istr(t, "max number") ||
           str_istr(t, "try again later") || str_istr(t, "connection limit") || str_istr(t, "limit reached");
}

static int has_tls_text(const char *t)
{
    return str_istr(t, "TLS") || str_istr(t, "SSL") || str_istr(t, "encrypt") || str_istr(t, "FTPS");
}

static int busy(ftp_conn *c, int code)
{
    snprintf(c->msg, sizeof c->msg, "the FTP server allows no more connections right now (%d %.150s)",
             code, c->text);
    trace(c, '!', c->msg);
    c->busy = 1;
    return 503;
}

/* ------------------------------------------------------------------ */
/* sockets                                                             */
/* ------------------------------------------------------------------ */
/* Wait until a is readable (or writable) or b is readable. Returns W_A/W_B
   bits, W_TIMEOUT, or -1 (aborted / select failed, c->msg set). */
static int wait_sock(ftp_conn *c, SOCKET a, int for_write, SOCKET b, int tmo_ms)
{
    double end = now_sec() + tmo_ms / 1000.0;
    for (;;) {
        if (http_aborted(c->s)) return ferr(c, ERR_CANCELLED, "cancelled");
        double left = (end - now_sec()) * 1000.0;
        if (left <= 0) return W_TIMEOUT;
        int ms = left > FTP_SLICE_MS ? FTP_SLICE_MS : (int)left + 1;
        fd_set rs, ws, es;
        FD_ZERO(&rs);
        FD_ZERO(&ws);
        FD_ZERO(&es);
        if (for_write) FD_SET(a, &ws);
        else FD_SET(a, &rs);
        FD_SET(a, &es);
        if (b != INVALID_SOCKET) FD_SET(b, &rs);
        struct timeval tv;
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        int n = select(0, &rs, &ws, &es, &tv);
        if (n == SOCKET_ERROR) return neterr(c, WSAGetLastError(), "network error");
        if (n == 0) continue;
        int res = 0;
        if (FD_ISSET(a, &rs) || FD_ISSET(a, &ws) || FD_ISSET(a, &es)) res |= W_A;
        if (b != INVALID_SOCKET && FD_ISSET(b, &rs)) res |= W_B;
        if (res) return res;
    }
}

/* Non-blocking TCP connection. On failure: INVALID_SOCKET, c->wsa set
   (ERR_CANCELLED when paused). */
static SOCKET tcp_connect(ftp_conn *c, const struct sockaddr *sa, int salen)
{
    SOCKET sk = socket(sa->sa_family, SOCK_STREAM, IPPROTO_TCP);
    if (sk == INVALID_SOCKET) { c->wsa = WSAGetLastError(); return INVALID_SOCKET; }
    SetHandleInformation((HANDLE)sk, HANDLE_FLAG_INHERIT, 0);
    u_long nb = 1;
    ioctlsocket(sk, FIONBIO, &nb);
    if (connect(sk, sa, salen) == 0) return sk;
    int e = WSAGetLastError();
    if (e == WSAEWOULDBLOCK) {
        int w = wait_sock(c, sk, 1, INVALID_SOCKET, c->tmo_ms);
        if (w < 0) { closesocket(sk); return INVALID_SOCKET; }
        if (w == W_TIMEOUT) e = WSAETIMEDOUT;
        else {
            int soerr = 0, l = sizeof soerr;
            if (getsockopt(sk, SOL_SOCKET, SO_ERROR, (char *)&soerr, &l) == 0 && soerr == 0) return sk;
            e = soerr ? soerr : WSAECONNREFUSED;
        }
    }
    closesocket(sk);
    c->wsa = e;
    return INVALID_SOCKET;
}

static int send_all(ftp_conn *c, SOCKET sk, const char *p, size_t n)
{
    while (n) {
        int k = send(sk, p, n > 65536 ? 65536 : (int)n, 0);
        if (k > 0) { p += k; n -= (size_t)k; continue; }
        int e = WSAGetLastError();
        if (k == SOCKET_ERROR && e == WSAEWOULDBLOCK) {
            int w = wait_sock(c, sk, 1, INVALID_SOCKET, c->tmo_ms);
            if (w < 0) return -1;
            if (w == W_TIMEOUT) return ferr(c, WSAETIMEDOUT, "the FTP server stopped responding");
            continue;
        }
        return neterr(c, e ? e : WSAECONNRESET, "lost the connection to the FTP server");
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* host name lookup that a pause can interrupt                          */
/* ------------------------------------------------------------------ */
typedef struct {
    volatile LONG refs;
    HANDLE        done;
    wchar_t      *host, *port;
    ADDRINFOW    *res;
    int           rc;
} dns_job;

static void dns_unref(dns_job *j)
{
    if (InterlockedDecrement(&j->refs) != 0) return;
    if (j->res) FreeAddrInfoW(j->res);
    if (j->done) CloseHandle(j->done);
    free(j->host);
    free(j->port);
    free(j);
}

static void dns_hints(ADDRINFOW *h, int flags)
{
    memset(h, 0, sizeof *h);
    h->ai_family = AF_UNSPEC;
    h->ai_socktype = SOCK_STREAM;
    h->ai_protocol = IPPROTO_TCP;
    h->ai_flags = flags;
}

static unsigned __stdcall dns_thread(void *arg)
{
    dns_job *j = arg;
    ADDRINFOW hints;
    dns_hints(&hints, 0);
    j->rc = GetAddrInfoW(j->host, j->port, &hints, &j->res);
    SetEvent(j->done);
    dns_unref(j);
    return 0;
}

/* Returns a job holding the address list (release with dns_unref) or NULL. */
static dns_job *resolve(ftp_conn *c, const char *host, const char *port)
{
    dns_job *j = xcalloc(1, sizeof *j);
    j->refs = 1;
    j->host = u8_to_w(host);
    j->port = u8_to_w(port);
    ADDRINFOW hints;
    dns_hints(&hints, AI_NUMERICHOST);
    if (GetAddrInfoW(j->host, j->port, &hints, &j->res) == 0 && j->res) return j;   /* IP literal */
    j->res = NULL;
    j->rc = 0;

    HANDLE th = NULL;
    j->done = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (j->done) {
        j->refs = 2;
        th = (HANDLE)_beginthreadex(NULL, 0, dns_thread, j, 0, NULL);
        if (!th) j->refs = 1;
    }
    if (!th) {
        dns_hints(&hints, 0);
        j->rc = GetAddrInfoW(j->host, j->port, &hints, &j->res);
    } else {
        CloseHandle(th);
        double end = now_sec() + c->tmo_ms / 1000.0;
        while (WaitForSingleObject(j->done, FTP_SLICE_MS) != WAIT_OBJECT_0) {
            if (http_aborted(c->s)) { dns_unref(j); ferr(c, ERR_CANCELLED, "cancelled"); return NULL; }
            if (now_sec() > end) {
                dns_unref(j);
                ferr(c, WSAETIMEDOUT, "looking up %s took too long", host);
                return NULL;
            }
        }
    }
    if (j->rc != 0 || !j->res) {
        int rc = j->rc ? j->rc : WSAHOST_NOT_FOUND;
        dns_unref(j);
        char what[300];
        snprintf(what, sizeof what, "cannot find the server %s", host);
        neterr(c, rc, what);
        return NULL;
    }
    return j;
}

/* ------------------------------------------------------------------ */
/* per-server connection slots                                          */
/* ------------------------------------------------------------------ */
static ftp_host *host_get(const char *host, const char *port)
{
    char *key = xasprintf("%s:%s", host, port);
    for (char *p = key; *p; p++) *p = (char)tolower((unsigned char)*p);
    EnterCriticalSection(&g_hlock);
    ftp_host *h = g_hosts;
    while (h && strcmp(h->key, key) != 0) h = h->next;
    if (!h) {
        h = xcalloc(1, sizeof *h);
        h->key = key;
        key = NULL;
        h->next = g_hosts;
        g_hosts = h;
    }
    LeaveCriticalSection(&g_hlock);
    free(key);
    return h;
}

/* Wait for permission to open one more session to this server.
   1 = go ahead, 0 = the deadline passed, -1 = paused. */
static int host_admit(ftp_conn *c, double deadline)
{
    ftp_host *h = c->host;
    int rc;
    EnterCriticalSection(&g_hlock);
    for (;;) {
        double now = now_sec();
        if (h->limit && now - h->limit_t > FTP_LIMIT_KEEP) h->limit = 0;
        if (!h->limit || (h->logged_in + h->connecting < h->limit && now - h->logout_t >= FTP_SLOT_GRACE)) {
            h->connecting++;
            c->admitted = 1;
            rc = 1;
            break;
        }
        if (http_aborted(c->s)) { rc = -1; break; }
        if (now >= deadline) { rc = 0; break; }
        SleepConditionVariableCS(&g_hcv, &g_hlock, FTP_SLICE_MS);
    }
    LeaveCriticalSection(&g_hlock);
    return rc;
}

/* The session setup ended: logged in (ok) or not. */
static void host_settle(ftp_conn *c, int ok)
{
    if (!c->admitted) return;
    ftp_host *h = c->host;
    EnterCriticalSection(&g_hlock);
    h->connecting--;
    c->admitted = 0;
    if (ok) {
        h->logged_in++;
        c->counted = 1;
        if (h->limit && h->logged_in > h->limit) h->limit = h->logged_in;   /* it allows more */
    }
    WakeAllConditionVariable(&g_hcv);
    LeaveCriticalSection(&g_hlock);
}

static void host_logout(ftp_conn *c)
{
    if (!c->counted) return;
    EnterCriticalSection(&g_hlock);
    ftp_host *h = c->host;
    h->logged_in--;
    h->logout_t = now_sec();
    c->counted = 0;
    WakeAllConditionVariable(&g_hcv);
    LeaveCriticalSection(&g_hlock);
}

/* "Too many connections": learn the cap from our other open sessions.
   Returns how many of those there are (0 = the server is full of others). */
static int host_busy(ftp_conn *c)
{
    ftp_host *h = c->host;
    EnterCriticalSection(&g_hlock);
    int others = h->logged_in - (c->counted ? 1 : 0);
    if (!h->limit) h->limit = others > 0 ? others : 1;
    h->limit_t = now_sec();
    LeaveCriticalSection(&g_hlock);
    return others;
}

static void host_remember_addr(ftp_conn *c)
{
    ftp_host *h = c->host;
    EnterCriticalSection(&g_hlock);
    h->good = c->peer;
    h->goodlen = c->peerlen;
    h->good_t = now_sec();
    LeaveCriticalSection(&g_hlock);
}

static int host_recall_addr(ftp_conn *c, sockaddr_any *sa)
{
    ftp_host *h = c->host;
    int len = 0;
    EnterCriticalSection(&g_hlock);
    if (h->goodlen && now_sec() - h->good_t < FTP_ADDR_KEEP) {
        *sa = h->good;
        len = h->goodlen;
    }
    LeaveCriticalSection(&g_hlock);
    return len;
}

/* ------------------------------------------------------------------ */
/* control connection                                                  */
/* ------------------------------------------------------------------ */
/* One reply line without its line end. Returns its length or -1. */
static int ctl_getline(ftp_conn *c, char *line, int cap)
{
    int n = 0;
    for (;;) {
        while (c->inpos < c->inlen) {
            char ch = c->in[c->inpos++];
            if (ch == '\n') {
                if (n && line[n - 1] == '\r') n--;
                line[n] = 0;
                return n;
            }
            if (n >= cap - 1) return ferr(c, WSAEMSGSIZE, "the FTP server sent an overlong reply");
            line[n++] = ch ? ch : ' ';
        }
        int k = recv(c->ctl, c->in, (int)sizeof c->in, 0);
        if (k > 0) { c->inpos = 0; c->inlen = k; continue; }
        if (k == 0) return ferr(c, WSAECONNRESET, "the FTP server closed the connection");
        int e = WSAGetLastError();
        if (e != WSAEWOULDBLOCK) return neterr(c, e, "lost the connection to the FTP server");
        int w = wait_sock(c, c->ctl, 0, INVALID_SOCKET, c->tmo_ms);
        if (w < 0) return -1;
        if (w == W_TIMEOUT) return ferr(c, WSAETIMEDOUT, "the FTP server did not answer in time");
    }
}

static int code3(const char *l, int n)
{
    if (n < 3 || !isdigit((unsigned char)l[0]) || !isdigit((unsigned char)l[1]) ||
        !isdigit((unsigned char)l[2]) || (n > 3 && l[3] != ' ' && l[3] != '-'))
        return 0;
    return (l[0] - '0') * 100 + (l[1] - '0') * 10 + (l[2] - '0');
}

static void text_add(ftp_conn *c, size_t *tl, const char *s)
{
    if (*tl && *tl < sizeof c->text - 1) c->text[(*tl)++] = ' ';
    for (; *s && *tl < sizeof c->text - 1; s++)
        c->text[(*tl)++] = (unsigned char)*s < 32 ? ' ' : *s;
    c->text[*tl] = 0;
}

/* Read one (possibly multi-line) reply: "123-first\r\n...\r\n123 last\r\n".
   Returns the code (100..599) or -1. */
static int ftp_reply(ftp_conn *c)
{
    char line[FTP_LINE_MAX];
    int code = 0, lines = 0;
    size_t total = 0, tl = 0;
    c->text[0] = 0;
    for (;;) {
        int n = ctl_getline(c, line, (int)sizeof line);
        if (n < 0) return -1;
        trace(c, '<', line);
        total += (size_t)n + 2;
        if (++lines > FTP_REPLY_LINES || total > FTP_REPLY_MAX)
            return ferr(c, WSAEMSGSIZE, "the FTP server sent an overlong reply");
        int lc = code3(line, n);
        if (!code) {
            if (n == 0) continue;                       /* stray empty line */
            if (lc < 100 || lc > 599)
                return ferr(c, ERR_PROTOCOL, "unexpected answer from the FTP server: %.100s", line);
            code = lc;
            text_add(c, &tl, n > 4 ? line + 4 : "");
            if (n == 3 || line[3] == ' ') break;
            continue;
        }
        if (lc == code) {
            text_add(c, &tl, n > 4 ? line + 4 : "");
            if (n == 3 || line[3] == ' ') break;
        } else {
            text_add(c, &tl, line);
        }
    }
    return code;
}

static int ftp_cmd(ftp_conn *c, const char *fmt, ...) __attribute__((format(gnu_printf, 2, 3)));
static int ftp_cmd(ftp_conn *c, const char *fmt, ...)
{
    char buf[FTP_CMD_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    if (n < 0 || n >= (int)sizeof buf - 2) return ferr(c, ERROR_FILENAME_EXCED_RANGE, "the FTP path is too long");
    trace(c, '>', strncmp(buf, "PASS ", 5) ? buf : "PASS ****");
    buf[n++] = '\r';
    buf[n++] = '\n';
    if (send_all(c, c->ctl, buf, (size_t)n) != 0) return -1;
    return ftp_reply(c);
}

static int ctl_open(ftp_conn *c, const ftp_url *u)
{
    /* the address that worked for the previous session first: no lookup, and
       no slow refusal from an address the server does not listen on */
    sockaddr_any last;
    int lastlen = host_recall_addr(c, &last), e = 0;
    if (lastlen) {
        SOCKET sk = tcp_connect(c, &last.sa, lastlen);
        if (sk != INVALID_SOCKET) {
            c->ctl = sk;
            c->peer = last;
            c->peerlen = lastlen;
        } else {
            if (c->wsa == ERR_CANCELLED) return ferr(c, ERR_CANCELLED, "cancelled");
            e = c->wsa;
        }
    }
    if (c->ctl == INVALID_SOCKET) {
        dns_job *j = resolve(c, u->host, u->port);
        if (!j) return -1;
        int tried = 0;
        for (ADDRINFOW *ai = j->res; ai && tried < FTP_MAX_ADDRS; ai = ai->ai_next) {
            if ((ai->ai_family != AF_INET && ai->ai_family != AF_INET6) || !ai->ai_addr ||
                ai->ai_addrlen > sizeof c->peer)
                continue;
            if (lastlen && (int)ai->ai_addrlen == lastlen && !memcmp(ai->ai_addr, &last, (size_t)lastlen))
                continue;                               /* just failed */
            tried++;
            SOCKET sk = tcp_connect(c, ai->ai_addr, (int)ai->ai_addrlen);
            if (sk != INVALID_SOCKET) {
                c->ctl = sk;
                memset(&c->peer, 0, sizeof c->peer);
                memcpy(&c->peer, ai->ai_addr, ai->ai_addrlen);
                c->peerlen = (int)ai->ai_addrlen;
                break;
            }
            if (c->wsa == ERR_CANCELLED) { dns_unref(j); return ferr(c, ERR_CANCELLED, "cancelled"); }
            e = c->wsa;
        }
        dns_unref(j);
        if (c->ctl == INVALID_SOCKET) {
            char what[300];
            snprintf(what, sizeof what, "cannot connect to %s port %s", u->host, u->port);
            return neterr(c, e ? e : WSAHOST_NOT_FOUND, what);
        }
        host_remember_addr(c);
    }
    BOOL nd = TRUE;
    setsockopt(c->ctl, IPPROTO_TCP, TCP_NODELAY, (const char *)&nd, sizeof nd);
    return 0;
}

static void close_data(ftp_conn *c, int abortive)
{
    if (c->data == INVALID_SOCKET) return;
    if (abortive) {                 /* RST: the server stops sending at once */
        struct linger lg;
        lg.l_onoff = 1;
        lg.l_linger = 0;
        setsockopt(c->data, SOL_SOCKET, SO_LINGER, (const char *)&lg, sizeof lg);
    }
    closesocket(c->data);
    c->data = INVALID_SOCKET;
}

/* Close both connections without waiting for anything. */
static void conn_drop(ftp_conn *c, const char *bye)
{
    close_data(c, !(c->complete && c->to_eof));
    if (c->ctl != INVALID_SOCKET) {
        if (bye) {
            trace(c, '>', strncmp(bye, "ABOR", 4) ? "QUIT" : "ABOR, QUIT");
            send(c->ctl, bye, (int)strlen(bye), 0);     /* non-blocking: never waits */
        }
        closesocket(c->ctl);
        c->ctl = INVALID_SOCKET;
    }
    c->inpos = c->inlen = 0;
    host_logout(c);
}

/* ------------------------------------------------------------------ */
/* URL                                                                 */
/* ------------------------------------------------------------------ */
static int hexv(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* Percent-decode; CR, LF and NUL (command injection) set *bad. */
static char *pct_decode(const char *s, size_t n, int *bad)
{
    sbuf b = { 0 };
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '%' && i + 2 < n && hexv(s[i + 1]) >= 0 && hexv(s[i + 2]) >= 0) {
            ch = (unsigned char)(hexv(s[i + 1]) * 16 + hexv(s[i + 2]));
            i += 2;
        }
        if (ch == 0 || ch == '\r' || ch == '\n') { *bad = 1; ch = '_'; }
        sb_putc(&b, (char)ch);
    }
    return sb_steal(&b);
}

static void url_free(ftp_url *u)
{
    free(u->user);
    free(u->pass);
    free(u->host);
    free(u->port);
    free(u->full);
    free(u->dir);
    free(u->name);
    memset(u, 0, sizeof *u);
}

/* ftp://[user[:pass]@]host[:port]/path[;type=x] */
static int url_parse(const char *url, ftp_url *u, char *err, size_t errlen)
{
    memset(u, 0, sizeof *u);
    int bad = 0;
    if (!str_istarts(url, "ftp://")) goto invalid;
    const char *a = url + 6;
    size_t alen = strcspn(a, "/?#");
    const char *at = NULL;
    for (size_t i = 0; i < alen; i++) if (a[i] == '@') at = a + i;
    const char *hp = a;
    size_t hl = alen;
    if (at) {
        size_t ul = (size_t)(at - a);
        const char *colon = memchr(a, ':', ul);
        u->user = pct_decode(a, colon ? (size_t)(colon - a) : ul, &bad);
        if (colon) u->pass = pct_decode(colon + 1, ul - (size_t)(colon + 1 - a), &bad);
        hp = at + 1;
        hl = alen - ul - 1;
    }
    const char *port = NULL;
    size_t pl = 0;
    if (hl && hp[0] == '[') {                           /* [IPv6 literal] */
        const char *rb = memchr(hp, ']', hl);
        if (!rb) goto invalid;
        u->host = pct_decode(hp + 1, (size_t)(rb - hp - 1), &bad);
        const char *after = rb + 1, *end = hp + hl;
        if (after < end) {
            if (*after != ':') goto invalid;
            port = after + 1;
            pl = (size_t)(end - port);
        }
    } else {
        const char *colon = memchr(hp, ':', hl);
        u->host = pct_decode(hp, colon ? (size_t)(colon - hp) : hl, &bad);
        if (colon) { port = colon + 1; pl = hl - (size_t)(colon + 1 - hp); }
    }
    if (bad || !u->host[0] || strlen(u->host) > 255) goto invalid;
    for (const unsigned char *p = (const unsigned char *)u->host; *p; p++)
        if (*p <= ' ' || *p == 0x7F || strchr("/\\?#@[]", *p)) goto invalid;
    long pv = 21;
    if (pl) {
        if (pl > 5) goto invalid;
        pv = 0;
        for (size_t i = 0; i < pl; i++) {
            if (!isdigit((unsigned char)port[i])) goto invalid;
            pv = pv * 10 + (port[i] - '0');
        }
        if (pv < 1 || pv > 65535) goto invalid;
    }
    u->port = xasprintf("%ld", pv);

    const char *path = a + alen;
    size_t plen = strcspn(path, "?#");
    if (plen < 2 || path[0] != '/') goto nofile;
    const char *raw = path + 1;
    size_t rl = plen - 1;
    if (rl >= 7 && _strnicmp(raw + rl - 7, ";type=", 6) == 0) rl -= 7;     /* RFC 1738 */
    if (rl == 0 || raw[rl - 1] == '/') goto nofile;
    const char *ls = NULL;
    for (size_t i = 0; i < rl; i++) if (raw[i] == '/') ls = raw + i;
    if (ls) {
        u->dir = pct_decode(raw, (size_t)(ls - raw), &bad);
        u->name = pct_decode(ls + 1, rl - (size_t)(ls + 1 - raw), &bad);
    } else {
        u->dir = xstrdup("");
        u->name = pct_decode(raw, rl, &bad);
    }
    char *all = pct_decode(raw, rl, &bad);
    if (all[0] == '/') u->full = all;
    else { u->full = xasprintf("/%s", all); free(all); }
    if (bad) goto invalid;
    if (!u->name[0]) goto nofile;
    for (const unsigned char *p = (const unsigned char *)u->full; *p; p++) if (*p >= 0x80) u->utf8 = 1;

    if (!u->user || !u->user[0]) {
        free(u->user);
        u->user = xstrdup("anonymous");
    }
    if (!u->pass)
        u->pass = xstrdup(!_stricmp(u->user, "anonymous") || !_stricmp(u->user, "ftp") ? "anonymous@" : "");
    return 0;

invalid:
    snprintf(err, errlen, "invalid FTP link");
    url_free(u);
    return -1;
nofile:
    snprintf(err, errlen, "the FTP link points to a folder, not a file (folder listings are not supported)");
    url_free(u);
    return -1;
}

/* ------------------------------------------------------------------ */
/* reply parsing                                                       */
/* ------------------------------------------------------------------ */
/* "213 1234567" */
static int64_t parse_size_reply(const char *t)
{
    while (*t == ' ') t++;
    int64_t v = 0;
    int nd = 0;
    while (isdigit((unsigned char)*t)) {
        if (++nd > 18) return -1;
        v = v * 10 + (*t++ - '0');
    }
    if (!nd || (*t && *t != ' ')) return -1;
    return v;
}

/* "213 20240131235959[.123]" -> "Wed, 31 Jan 2024 23:59:59 GMT" */
static char *mdtm_to_http(const char *t)
{
    static const char *wd[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *mo[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    while (*t == ' ') t++;
    for (int i = 0; i < 14; i++) if (!isdigit((unsigned char)t[i])) return NULL;
    if (t[14] && t[14] != '.' && t[14] != ' ') return NULL;
    int v[6], w[6] = { 4, 2, 2, 2, 2, 2 };
    const char *p = t;
    for (int i = 0; i < 6; i++) {
        v[i] = 0;
        for (int k = 0; k < w[i]; k++) v[i] = v[i] * 10 + (*p++ - '0');
    }
    SYSTEMTIME st;
    FILETIME ft;
    memset(&st, 0, sizeof st);
    st.wYear = (WORD)v[0];
    st.wMonth = (WORD)v[1];
    st.wDay = (WORD)v[2];
    st.wHour = (WORD)v[3];
    st.wMinute = (WORD)v[4];
    st.wSecond = (WORD)v[5];
    if (st.wYear < 1970 || !SystemTimeToFileTime(&st, &ft) || !FileTimeToSystemTime(&ft, &st)) return NULL;
    return xasprintf("%s, %02u %s %04u %02u:%02u:%02u GMT", wd[st.wDayOfWeek % 7], (unsigned)st.wDay,
                     mo[(st.wMonth - 1) % 12], (unsigned)st.wYear, (unsigned)st.wHour,
                     (unsigned)st.wMinute, (unsigned)st.wSecond);
}

/* "229 Entering Extended Passive Mode (|||6446|)" */
static int parse_epsv(const char *t)
{
    const char *p = strchr(t, '(');
    if (!p) return -1;
    char d = p[1];
    if (d < 33 || d > 126 || isdigit((unsigned char)d) || p[2] != d || p[3] != d) return -1;
    p += 4;
    long v = 0;
    int nd = 0;
    while (isdigit((unsigned char)*p) && nd < 6) { v = v * 10 + (*p++ - '0'); nd++; }
    if (!nd || *p != d || v < 1 || v > 65535) return -1;
    return (int)v;
}

/* "227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)". Only the port is used:
   the address is always the control connection's (NAT'd servers announce
   private addresses; curl does the same). */
static int parse_pasv(const char *t)
{
    for (const char *p = t; *p; p++) {
        if (!isdigit((unsigned char)*p) || (p > t && isdigit((unsigned char)p[-1]))) continue;
        int v[6], k = 0;
        const char *q = p;
        while (k < 6) {
            int nd = 0, x = 0;
            while (isdigit((unsigned char)*q) && nd < 4) { x = x * 10 + (*q++ - '0'); nd++; }
            if (!nd || nd > 3 || x > 255) break;
            v[k++] = x;
            if (k < 6) {
                if (*q != ',') break;
                q++;
            }
        }
        if (k == 6 && !isdigit((unsigned char)*q)) {
            int port = v[4] * 256 + v[5];
            return port > 0 ? port : -1;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* protocol steps (0 = ok, -1 = error with c->msg, else an HTTP-like status) */
/* ------------------------------------------------------------------ */
static int login_failed(ftp_conn *c, int code)
{
    if (code == 421 || (code >= 400 && has_busy_text(c->text))) return busy(c, code);
    if (code == 534 || code == 522 || (code >= 500 && has_tls_text(c->text)))
        return ferr(c, ERROR_NOT_SUPPORTED, "the FTP server only accepts encrypted connections (FTPS), "
                    "which U-loader does not support (%d %.120s)", code, c->text);
    if (code == 530 || code == 332 || code == 532 || code == 331) {
        snprintf(c->msg, sizeof c->msg, "FTP login failed (%d %.150s)", code, c->text);
        trace(c, '!', c->msg);
        return 401;
    }
    return refused(c, code, "the FTP server refused the login");
}

/* connect, greeting, login, binary mode */
static int session_open(ftp_conn *c, const ftp_url *u)
{
    if (ctl_open(c, u) != 0) return -1;
    int code, n = 0;
    do code = ftp_reply(c); while (code >= 100 && code < 200 && ++n < 5);    /* "120 ready in 2 min" */
    if (code < 0) return -1;
    if (code == 421 || (code >= 400 && has_busy_text(c->text))) return busy(c, code);
    if (code != 220) return code >= 500 ? login_failed(c, code) : refused(c, code, "the FTP server refused the connection");

    code = ftp_cmd(c, "USER %s", u->user);
    if (code < 0) return -1;
    if (code == 331) {
        code = ftp_cmd(c, "PASS %s", u->pass);
        if (code < 0) return -1;
    }
    if (code != 230 && code != 202) return login_failed(c, code);
    host_settle(c, 1);

    if (u->utf8 && ftp_cmd(c, "OPTS UTF8 ON") < 0) return -1;    /* answer does not matter */
    code = ftp_cmd(c, "TYPE I");
    if (code < 0) return -1;
    if (code == 421) return busy(c, code);
    if (code / 100 != 2) return refused(c, code, "the FTP server refused binary mode");
    return 0;
}

/* Point q->target at the file (CWD in mode 1), then SIZE and MDTM.
   Returns 1 when SIZE says the full path does not exist (mode 0). */
static int locate(ftp_conn *c, ftp_req *q)
{
    int code;
    free(q->mdtm);
    q->mdtm = NULL;
    q->size = -1;
    q->target = q->mode == 0 ? q->u.full : q->u.name;
    if (q->mode == 1 && q->u.dir[0]) {
        code = ftp_cmd(c, "CWD %s", q->u.dir);
        if (code < 0) return -1;
        if (code == 421) return busy(c, code);
        if (code / 100 != 2) {
            snprintf(c->msg, sizeof c->msg, "folder not found on the FTP server (%d %.150s)", code, c->text);
            return code == 530 ? 401 : 404;
        }
    }
    if (q->size_ok) {
        code = ftp_cmd(c, "SIZE %s", q->target);
        if (code < 0) return -1;
        if (code == 421) return busy(c, code);
        if (code == 213) q->size = parse_size_reply(c->text);
        else if (code == 550 && q->mode == 0) return 1;
        else if (code == 500 || code == 502 || code == 504 || code == 202) q->size_ok = 0;
    }
    if (q->want_mdtm) {
        code = ftp_cmd(c, "MDTM %s", q->target);
        if (code < 0) return -1;
        if (code == 421) return busy(c, code);
        if (code == 213) q->mdtm = mdtm_to_http(c->text);
    }
    return 0;
}

/* Full path first; when that is unknown, CWD to the folder (RFC 1738
   style, relative to the login folder) and use the bare name. */
static int locate_first(ftp_conn *c, ftp_req *q)
{
    q->mode = 0;
    int rc = locate(c, q);
    if (rc != 1) return rc;
    q->mode = 1;
    rc = locate(c, q);
    if (rc == 404 || rc == 401) {
        /* no such folder that way either: let RETR of the full path decide */
        q->mode = 0;
        q->no_fallback = 1;
        q->target = q->u.full;
        q->size = -1;
        rc = 0;
    }
    return rc;
}

/* EPSV does not work with this server: later connections go straight to PASV
   (an unreachable EPSV port can cost a whole timeout per connection). */
static void epsv_broken(ftp_conn *c)
{
    c->no_epsv = 1;
    if (c->peer.sa.sa_family == AF_INET) InterlockedExchange(&c->host->no_epsv, 1);
}

/* Passive data connection, always to the control connection's address. */
static int open_data(ftp_conn *c)
{
    if (c->host->no_epsv && c->peer.sa.sa_family == AF_INET) c->no_epsv = 1;
    for (;;) {
        int port = -1, epsv = !c->no_epsv, code = 0;
        if (epsv) {
            code = ftp_cmd(c, "EPSV");
            if (code < 0) return -1;
            if (code == 421) return busy(c, code);
            if (code == 229) port = parse_epsv(c->text);
            if (port <= 0) { epsv_broken(c); epsv = 0; }
        }
        if (port <= 0) {
            if (c->peer.sa.sa_family != AF_INET)
                return refused(c, code, "the FTP server does not support passive mode over IPv6 (EPSV)");
            code = ftp_cmd(c, "PASV");
            if (code < 0) return -1;
            if (code == 421) return busy(c, code);
            if (code != 227 || (port = parse_pasv(c->text)) <= 0)
                return refused(c, code, "the FTP server refused passive mode");
        }
        sockaddr_any sa = c->peer;
        if (sa.sa.sa_family == AF_INET) sa.v4.sin_port = htons((u_short)port);
        else sa.v6.sin6_port = htons((u_short)port);
        SOCKET sk = tcp_connect(c, &sa.sa, c->peerlen);
        if (sk != INVALID_SOCKET) { c->data = sk; return 0; }
        if (c->wsa == ERR_CANCELLED) return ferr(c, ERR_CANCELLED, "cancelled");
        if (epsv && c->peer.sa.sa_family == AF_INET) { epsv_broken(c); continue; }   /* try PASV */
        char what[80];
        snprintf(what, sizeof what, "cannot open the FTP data connection (port %d)", port);
        return neterr(c, c->wsa, what);
    }
}

static int retr_failed(ftp_conn *c, int code)
{
    if (code == 421 || (code >= 400 && has_busy_text(c->text))) return busy(c, code);
    if (code == 550) {
        int denied = str_istr(c->text, "denied") || str_istr(c->text, "permission");
        snprintf(c->msg, sizeof c->msg, "%s (%d %.150s)",
                 denied ? "access denied by the FTP server" : "file not found on the FTP server", code, c->text);
        return denied ? 403 : 404;
    }
    if (code == 530 || code == 532) {
        snprintf(c->msg, sizeof c->msg, "FTP login required (%d %.150s)", code, c->text);
        return 401;
    }
    if (code == 425 || code == 426) return refused(c, code, "the FTP server could not open the data connection");
    return refused(c, code, "the FTP server refused the download");
}

/* Returns 0 with the transfer running and r filled, a status (no
   transfer), or -1. */
static int start_transfer(ftp_conn *c, ftp_req *q, int64_t from, int64_t to, http_response *r)
{
    int rc = locate_first(c, q);
    if (rc) return rc;
    for (;;) {
        int64_t size = q->size;
        if (size >= 0 && from > 0 && from >= size) {
            r->range_total = size;
            snprintf(c->msg, sizeof c->msg, "the requested range starts beyond the end of the file");
            return 416;
        }
        if (size == 0 && from <= 0) {           /* empty file: nothing to transfer */
            r->content_length = 0;
            c->complete = c->to_eof = 1;
            return 200;
        }
        if ((rc = open_data(c)) != 0) return rc;
        int rest_ok = 0, code;
        if (from > 0 || (from == 0 && size > 0)) {
            code = ftp_cmd(c, "REST %lld", (long long)from);
            if (code < 0) return -1;
            if (code == 421) return busy(c, code);
            rest_ok = code == 350;
        }
        code = ftp_cmd(c, "RETR %s", q->target);
        if (code < 0) return -1;
        if (code == 125 || code == 150) {
            c->xfer = 1;
            if (rest_ok && size >= 0) {
                int64_t last = size - 1;
                if (to >= from && to < last) last = to;
                r->status = 206;
                r->range_start = from;
                r->range_total = size;
                r->content_length = last - from + 1;
                c->to_eof = last == size - 1;
            } else if (rest_ok && from > 0) {   /* size unknown */
                r->status = 206;
                r->range_start = from;
                r->content_length = to >= from ? to - from + 1 : -1;
                c->to_eof = to < from;
            } else {                            /* whole file */
                r->status = 200;
                r->content_length = size;
                c->to_eof = 1;
            }
            c->remaining = r->content_length;
            return 0;
        }
        close_data(c, 1);
        if (code == 550 && q->mode == 0 && !q->no_fallback) {
            q->mode = 1;
            q->no_fallback = 1;
            if ((rc = locate(c, q)) != 0) return rc;
            continue;
        }
        return retr_failed(c, code);
    }
}

/* ------------------------------------------------------------------ */
/* public interface                                                    */
/* ------------------------------------------------------------------ */
int ftp_request(http_session *s, const char *url, int64_t from, int64_t to,
                http_response *r, char *err, size_t errlen)
{
    err[0] = 0;
    if (!net_ready(err, errlen)) return -1;
    if (http_aborted(s)) { snprintf(err, errlen, "cancelled"); return -1; }
    ftp_req q;
    memset(&q, 0, sizeof q);
    if (url_parse(url, &q.u, err, errlen) != 0) return -2;      /* retrying cannot help */
    if (from < 0) from = -1;
    if (from < 0 || to < from) to = -1;
    q.size_ok = 1;
    q.want_mdtm = from <= 0;

    ftp_conn *c = xcalloc(1, sizeof *c);
    c->s = s;
    c->ctl = c->data = INVALID_SOCKET;
    c->id = (unsigned)InterlockedIncrement(&g_ids);
    int ts = http_cfg(s)->timeout_sec;
    if (ts < 1) ts = 30;
    if (ts > 3600) ts = 3600;
    c->tmo_ms = ts * 1000;
    c->host = host_get(q.u.host, q.u.port);
    if (g_trace) {
        char t[160];
        snprintf(t, sizeof t, "request %s:%s from %lld to %lld", q.u.host, q.u.port, (long long)from, (long long)to);
        trace(c, '*', t);
    }

    /* When the server is full of our own sessions, queue for one of them to
       end (up to FTP_BUSY_WAIT s) rather than retrying logins. */
    double deadline = now_sec() + (ts > FTP_BUSY_WAIT ? ts : FTP_BUSY_WAIT);
    int rc;
    for (;;) {
        int w = host_admit(c, deadline);
        if (w < 0) { rc = ferr(c, ERR_CANCELLED, "cancelled"); break; }
        if (w == 0) {
            if (!c->busy) {
                snprintf(c->msg, sizeof c->msg, "no free connection to the FTP server (it limits connections)");
                c->busy = 1;
            }
            rc = 503;
            break;
        }
        c->busy = 0;
        rc = session_open(c, &q.u);
        host_settle(c, 0);                  /* no-op once logged in */
        if (rc != 503 || !c->busy) break;
        int others = host_busy(c);
        conn_drop(c, NULL);
        if (others <= 0) break;             /* full of other clients: the caller backs off */
    }
    if (rc == 0) rc = start_transfer(c, &q, from, to, r);

    if (rc < 0) {
        int permanent = c->wsa == ERROR_NOT_SUPPORTED;     /* e.g. the server demands FTPS */
        snprintf(err, errlen, "%s", c->msg[0] ? c->msg : "FTP error");
        conn_drop(c, "QUIT\r\n");
        free(c);
        free(q.mdtm);
        url_free(&q.u);
        r->content_length = r->range_start = r->range_total = -1;
        return permanent ? -2 : -1;
    }
    if (rc > 0) {                   /* answered without a transfer */
        conn_drop(c, "QUIT\r\n");
        c->done = 1;
        r->status = rc;
        if (c->msg[0]) snprintf(err, errlen, "%s", c->msg);
    } else {
        r->last_modified = q.mdtm;
        q.mdtm = NULL;
    }
    r->ftp = c;
    r->final_url = xstrdup(url);
    free(q.mdtm);
    url_free(&q.u);
    return 0;
}

static int read_fail(ftp_conn *c, DWORD code)
{
    c->fail = code ? code : (DWORD)WSAECONNRESET;
    if (g_trace) {
        char e[160];
        trace(c, '!', win_errstr(c->fail, e, sizeof e));
    }
    SetLastError(c->fail);
    return -1;
}

/* Data connection ended: decide whether that was the end of the file. */
static int data_eof(ftp_conn *c)
{
    close_data(c, 0);
    if (c->remaining > 0) {
        /* fewer bytes than announced: report the end, the caller sees the gap */
        trace(c, '*', "data connection closed early");
        c->done = 1;
        return 0;
    }
    /* size unknown: only the final reply tells a complete file from a cut one */
    for (int n = 0; !c->final; n++) {
        if (c->ctl_dead || n >= 5) return read_fail(c, WSAECONNRESET);
        int code = ftp_reply(c);
        if (code < 0) return read_fail(c, (DWORD)(c->wsa ? c->wsa : WSAECONNRESET));
        if (code >= 200) c->final = code;               /* skip stray 1xx */
    }
    c->xfer = 0;
    if (c->final / 100 != 2) return read_fail(c, WSAECONNRESET);
    c->complete = 1;
    c->done = 1;
    return 0;
}

int ftp_read(http_response *r, void *buf, DWORD len, DWORD *got)
{
    ftp_conn *c = r->ftp;
    *got = 0;
    if (!c) { SetLastError(ERROR_INVALID_HANDLE); return -1; }
    if (c->fail) { SetLastError(c->fail); return -1; }
    if (c->done) return 0;
    if (c->remaining == 0 || c->data == INVALID_SOCKET) {
        if (c->remaining == 0) c->complete = 1;
        c->done = 1;
        return 0;
    }
    DWORD want = len;
    if (c->remaining > 0 && (int64_t)want > c->remaining) want = (DWORD)c->remaining;
    if (want > (1u << 30)) want = 1u << 30;
    if (want == 0) return 0;
    for (;;) {
        int n = recv(c->data, buf, (int)want, 0);
        if (n > 0) {
            *got = (DWORD)n;
            if (c->remaining > 0) {
                c->remaining -= n;
                if (c->remaining == 0) c->complete = 1;
            }
            return 0;
        }
        if (n == 0) return data_eof(c);
        int e = WSAGetLastError();
        if (e != WSAEWOULDBLOCK) return read_fail(c, (DWORD)e);

        /* nothing buffered: wait, and also listen to the control connection
           in case the server gives up on the transfer */
        SOCKET watch = (c->final || c->ctl_dead) ? INVALID_SOCKET : c->ctl;
        int w = wait_sock(c, c->data, 0, watch, c->tmo_ms);
        if (w < 0) return read_fail(c, (DWORD)c->wsa);
        if (w == W_TIMEOUT) return read_fail(c, WSAETIMEDOUT);
        if ((w & W_B) && !(w & W_A)) {
            int code = ftp_reply(c);
            if (code < 0) {
                if (c->wsa == ERR_CANCELLED) return read_fail(c, ERR_CANCELLED);
                c->ctl_dead = 1;        /* keep reading; the data decides */
            } else if (code >= 200) {   /* a stray 1xx is ignored */
                c->final = code;
                c->xfer = 0;
                if (code / 100 != 2) return read_fail(c, WSAECONNRESET);   /* 426, 451... */
            }
        }
    }
}

void ftp_close(http_response *r)
{
    ftp_conn *c = r->ftp;
    if (!c) return;
    r->ftp = NULL;
    const char *bye = (c->xfer && !c->final && !(c->complete && c->to_eof)) ? "ABOR\r\nQUIT\r\n" : "QUIT\r\n";
    conn_drop(c, c->ctl_dead ? NULL : bye);
    free(c);
}
