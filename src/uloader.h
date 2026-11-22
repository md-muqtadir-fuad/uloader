/*
 * U-loader - fast multi-connection downloader for Windows
 * Shared declarations.
 */
#ifndef ULOADER_H
#define ULOADER_H

#define __USE_MINGW_ANSI_STDIO 1
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <winhttp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <limits.h>

/* 64-bit only: progress counters (volatile LONG64) are read by other
   threads without locks, which is tear-free only on 64-bit Windows */
_Static_assert(sizeof(void *) == 8, "U-loader is built for 64-bit Windows (x86_64 MinGW-w64)");

#define UL_NAME        "U-loader"
#define UL_VERSION     "1.3.0"
#define UL_MAX_CONN    32
#define UL_MAX_HEADERS 48
#define UL_SEGVIEW     64          /* resolution of the progress segment map */
#define UL_DEFAULT_UA  "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 " \
                       "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"

/* GUI <-> native host contract */
#ifndef UL_GUI_CLASS                    /* a test build may use its own class */
#define UL_GUI_CLASS      L"ULoaderMainWindow"
#endif
#define UL_GUI_EXE        "uloader-gui.exe"
#define UL_COPYDATA_JOB   0x554C4A42   /* WM_COPYDATA dwData; lpData = UTF-8 job file path */
#define UL_HOST_NAME      "com.uloader.host"

/* ------------------------------------------------------------------ */
/* configuration                                                       */
/* ------------------------------------------------------------------ */
typedef struct {
    char   *domain;       /* "example.com" or ".example.com" (subdomains too) */
    char   *path;
    int     secure;
    int64_t expires;      /* unix time, 0 = session */
    char   *name, *value;
} ul_cookie;

enum { MODE_AUTO = 0, MODE_DIRECT = 1, MODE_YTDLP = 2 };

#define UL_AUDIO_ONLY (-1)   /* max_height: the best audio, no video */

/* Download categories: files sorted into sub-folders by type (#1). */
enum { CAT_VIDEO, CAT_MUSIC, CAT_DOCUMENTS, CAT_COMPRESSED, CAT_PROGRAMS, CAT_PICTURES, UL_NCATS };

typedef struct {
    int     connections;      /* connections per file                   */
    int     retries;          /* retries per connection / segment       */
    int     timeout_sec;      /* network timeout                        */
    int64_t speed_limit;      /* bytes per second, 0 = unlimited        */
    int     insecure;         /* ignore TLS certificate errors          */
    int     overwrite;        /* overwrite existing files               */
    int     playlist;         /* let yt-dlp expand playlists            */
    int     force_direct;     /* never use the site extractor           */
    int     force_ytdlp;      /* always use the site extractor          */
    int     max_height;       /* video quality cap (e.g. 720), 0 = best, UL_AUDIO_ONLY */
    int     by_category;      /* save into out_dir\<Category>\ by file type */
    char   *out_name;         /* -o: output file name                   */
    char   *out_dir;          /* -d: output folder                      */
    char   *user_agent;
    char   *referer;
    char   *cookie;           /* raw "a=b; c=d" (sent only to cookie_host) */
    char   *cookie_host;      /* host the raw cookie belongs to          */
    char   *cookie_file;      /* Netscape cookies.txt (also given to yt-dlp) */
    ul_cookie *cookies;       /* parsed cookie jar                       */
    int     ncookies;
    char   *proxy;
    char   *format;           /* yt-dlp format selector                 */
    char   *ytdlp_args;       /* raw extra arguments for yt-dlp         */
    char   *headers[UL_MAX_HEADERS]; /* "Name: value"                   */
    int     nheaders;
} ul_config;

typedef struct {
    char *name[UL_MAX_HEADERS];
    char *value[UL_MAX_HEADERS];
    int   n;
} ul_headers;

/* ------------------------------------------------------------------ */
/* tasks: one download, shared by the console and the GUI              */
/* ------------------------------------------------------------------ */
enum { TS_QUEUED, TS_RUNNING, TS_PAUSED, TS_DONE, TS_ERROR };
enum { LOG_INFO, LOG_OK, LOG_WARN, LOG_ERR, LOG_HEAD, LOG_DIM };

typedef struct ul_task ul_task;
struct ul_task {
    /* set by the creator */
    char  *url;
    void  *user;                                         /* front-end data      */
    void (*on_log)(ul_task *t, int kind, const char *msg); /* NULL = console    */
    void (*on_progress)(ul_task *t);                     /* NULL = console bar  */

    /* control: the front-end sets this to pause/stop the task */
    volatile LONG abort;
    int  ignore_ctrl_c;       /* 1 = g_abort does not stop it (a final tracker "stopped") */

    /* progress of the current file (written by the download thread) */
    volatile LONG64 done;     /* bytes                         */
    volatile LONG64 total;    /* bytes, -1 = unknown           */
    volatile LONG64 speed;    /* bytes per second              */
    volatile LONG64 eta;      /* seconds, -1 = unknown         */
    volatile LONG   conns;    /* active connections / peers    */
    volatile LONG   state;    /* TS_*                          */

    CRITICAL_SECTION lock;    /* guards everything below       */
    char  *name;              /* display name                  */
    char  *path;              /* final file / folder           */
    char   stage[96];         /* e.g. "audio stream", "seg 3/64", "12 peers" */
    char   status[512];       /* last message                  */
    unsigned char segmap[UL_SEGVIEW];   /* 0 = missing .. 255 = complete */
};

ul_task *task_new(const char *url);
void     task_free(ul_task *t);
/* Run the task in the calling thread. Returns 0 done, 1 error, 2 paused. */
int      task_run(ul_task *t, ul_config *cfg);
int      task_aborted(const ul_task *t);
void     task_sleep(const ul_task *t, DWORD ms);
void     task_set_file(ul_task *t, const char *name, const char *path);
void     task_set_stage(ul_task *t, const char *fmt, ...) __attribute__((format(gnu_printf, 2, 3)));
void     task_reset_progress(ul_task *t);
void     task_progress(ul_task *t);          /* fields updated -> notify front-end */
void     tlog(ul_task *t, int kind, const char *fmt, ...) __attribute__((format(gnu_printf, 3, 4)));

/* Job files: what the browser extension / native host hands over.
   Text, one "key=value" per line, first line "U-LOADER-JOB 1".
   Keys: url, mode (auto|direct|ytdlp), referer, ua, name, dir,
   connections, quality, cookies (path of a Netscape cookies.txt). */
int  job_load(const char *path, ul_config *cfg, char **url);
/* 1 if a job's cookie file is one the native host wrote (%TEMP%\ulc*.tmp),
   i.e. safe to delete when the job is finished or removed */
int  job_cookie_file_is_temp(const char *path);
char *job_write(const char *url, const char *mode, const char *referer, const char *ua,
                const char *name, const char *dir, int connections, int quality,
                const char *cookie_file);   /* returns path of a new temp job file */

ul_config *cfg_clone(const ul_config *c);
void       cfg_free(ul_config *c);           /* frees a clone (or any heap config) */
void       cfg_defaults(ul_config *c);

extern volatile LONG g_abort;   /* console Ctrl+C                    */
extern volatile LONG g_busy;    /* console: a download is running    */
extern int g_vt;                /* console understands ANSI escapes  */
extern int g_tty;               /* stdout is a console               */

/* ------------------------------------------------------------------ */
/* util.c                                                              */
/* ------------------------------------------------------------------ */
void   *xmalloc(size_t n);
void   *xcalloc(size_t a, size_t b);
void   *xrealloc(void *p, size_t n);
char   *xstrdup(const char *s);
char   *xstrndup(const char *s, size_t n);
char   *xasprintf(const char *fmt, ...) __attribute__((format(gnu_printf, 1, 2)));

wchar_t *u8_to_w(const char *s);
char    *w_to_u8(const wchar_t *w);
wchar_t *wpath(const char *p);    /* UTF-8 path -> wide, \\?\-prefixed when long */
int      utf8_valid(const unsigned char *s, size_t n);

typedef struct { char *s; size_t len, cap; } sbuf;
void  sb_putn(sbuf *b, const char *s, size_t n);
void  sb_puts(sbuf *b, const char *s);
void  sb_putc(sbuf *b, char c);
void  sb_printf(sbuf *b, const char *fmt, ...) __attribute__((format(gnu_printf, 2, 3)));
char *sb_steal(sbuf *b);
void  sb_free(sbuf *b);

int         str_istarts(const char *s, const char *prefix);
int         str_iends(const char *s, const char *suffix);
const char *str_istr(const char *hay, const char *needle);
char       *str_trim(char *s);

const char *fmt_size(int64_t n, char *buf, size_t len);
const char *fmt_eta(double sec, char *buf, size_t len);
int64_t     parse_size(const char *s);
double      now_sec(void);
const char *win_errstr(DWORD code, char *buf, size_t len);

/* JSON (just enough for yt-dlp output and browser messages) */
const char *json_skip_ws(const char *p);
const char *json_skip(const char *p);              /* skip one value; NULL on error */
const char *json_find(const char *obj, const char *key); /* value inside {..} or NULL */
char       *json_str(const char *val);             /* decoded string/literal, NULL for null */
int         json_parse_string(const char **pp, sbuf *out);
void        json_put_string(sbuf *b, const char *s);

/* cookies */
int   cookies_load(ul_config *cfg, const char *path);       /* Netscape format */
char *cookie_header_for(const ul_config *cfg, const char *url, int include_raw); /* NULL if none */
int   host_matches(const char *host, const char *domain);
char *url_origin(const char *url);      /* "https://host:443" - credentials belong to one origin */

/* URLs and file names */
char *url_decode(const char *s, size_t n);
char *url_host(const char *url);
char *url_filename(const char *url);          /* decoded last path segment or NULL */
char *url_resolve(const char *base, const char *ref);
char *sanitize_filename(const char *name);
const char *ext_for_content_type(const char *ct);

/* Categories (#1). A file name -> CAT_*, or -1 (stays in the folder itself). */
const char *category_name(int cat);               /* "Video", "Music", ...   */
const char *category_default_exts(int cat);
char       *category_exts(int cat);               /* current list, malloc'ed */
void        category_set_exts(int cat, const char *exts);   /* NULL = default */
int         category_of(const char *filename);
/* The folder `filename` is saved in: out_dir, or out_dir\<Category> when
   cfg->by_category is on. malloc'ed. */
char       *category_folder(const ul_config *cfg, const char *filename);

/* U-loader's data folder: ULOADER_DATA, else %APPDATA%\U-loader. malloc'ed. */
char *uloader_data_dir(void);
/* The browser extension has talked to U-loader (#13): the native host
   records it, the window asks. */
void browser_mark_connected(void);
int  browser_connected(void);

/* Speed limiter for all downloads of this process together (#2). */
void    speed_limit_set(int64_t bytes_per_sec);   /* 0 = off; applies at once */
int64_t speed_limit_get(void);
void    speed_limit_take(const ul_task *t, int64_t bytes);   /* waits as needed */

/* files */
char   *path_join(const char *dir, const char *name);
const char *path_basename(const char *p);
const char *path_ext(const char *p);         /* ".ext" or "" */
char   *path_strip_ext(const char *p);
char   *path_dirname(const char *p);
int     file_exists(const char *p);
int     dir_exists(const char *p);
int64_t file_size(const char *p);
int     file_delete(const char *p);
/* which server version a finished file is (hidden NTFS stream; dates untouched) */
void    file_meta_write(const char *p, const char *etag, const char *lastmod);
int     file_meta_matches(const char *p, const char *etag, const char *lastmod); /* 1 / 0 / -1 unknown */
int     file_rename(const char *from, const char *to);
int     dir_create(const char *p);
int     dir_remove(const char *p);
FILE   *fopen_u8(const char *p, const char *mode);
char   *read_file(const char *p, size_t *len);
char   *unique_path(const char *path);
int64_t disk_free(const char *dir_or_file);
char   *exe_dir(void);
char   *downloads_dir(void);
char   *find_tool(const char *exe_name);

/* processes */
#define RUN_FAILED INT_MIN   /* run_process: could not start, or stopped because of a pause */

/* total * i / n without the 64-bit overflow of the plain formula
   (0 <= i <= n, n small: a segment or progress-map count) */
static inline int64_t share_of(int64_t total, int i, int n)
{
    return total / n * i + total % n * i / n;
}
void cmd_append_arg(sbuf *b, const char *arg);
int  run_process(const char *cmdline, char **captured_stdout, const ul_task *t);
int  ffmpeg_merge(const char *ffmpeg, const char *video, const char *audio, const char *out, const ul_task *t);
int  ffmpeg_remux(const char *ffmpeg, const char *in, const char *out, const ul_task *t);

/* headers */
void headers_set(ul_headers *h, const char *name, const char *value);
void headers_free(ul_headers *h);

/* console */
void  con_init(void);
int   con_width(void);
void  con_progress(const char *line);
void  con_progress_end(void);
char *con_read_line(void);
void  ul_log(int kind, const char *fmt, ...) __attribute__((format(gnu_printf, 2, 3)));
void  con_log(int kind, const char *msg);

/* ------------------------------------------------------------------ */
/* http.c - WinHTTP wrapper (ftp:// requests are routed to ftp.c)      */
/* ------------------------------------------------------------------ */
typedef struct http_session http_session;

typedef struct {
    HINTERNET hConnect, hRequest;
    void     *live;             /* registration for http_session_cancel */
    void     *ftp;              /* non-NULL: an FTP transfer (ftp.c)   */
    int       status;
    int64_t   content_length;   /* -1 = unknown                        */
    int64_t   range_start;      /* from Content-Range, -1 = none       */
    int64_t   range_total;      /* from Content-Range, -1 = unknown    */
    char     *content_type;
    char     *content_disposition;
    char     *etag;
    char     *last_modified;
    char     *final_url;        /* URL after redirects                 */
} http_response;

#define HTTP_TEXT  1  /* allow compressed responses (for playlists/pages) */
#define HTTP_SMALL 2  /* http_get_body: refuse bodies over 4 MB (tracker replies...) */

http_session    *http_session_new(const ul_config *cfg, ul_task *t);
void             http_session_free(http_session *s);
/* Abort every request of the session that is in progress (from any thread):
   blocked reads fail at once. Used when pausing. */
void             http_session_cancel(http_session *s);
/* Credentials (-H Authorization/Cookie, site headers) are only sent to the
   host of this URL; redirects and worker requests to other hosts get none.
   Set once per downloaded item; NULL = each request's own first URL. */
void             http_session_set_origin(http_session *s, const char *url);
ul_task         *http_task(http_session *s);
const ul_config *http_cfg(http_session *s);
int              http_aborted(http_session *s);
/* from < 0: no Range header. to < 0: open-ended range. Returns 0 on success,
   -1 on an error worth retrying, -2 on one that retrying cannot fix
   (invalid link, redirect loop, server requires FTPS...). */
int  http_request(http_session *s, const char *url, const ul_headers *hd,
                  int64_t from, int64_t to, int flags, http_response *r,
                  char *err, size_t errlen);
int  http_read(http_response *r, void *buf, DWORD len, DWORD *got);
void http_close(http_response *r);
/* counter (optional) is increased live as bytes arrive (for progress) */
char *http_get_body(http_session *s, const char *url, const ul_headers *hd,
                    int64_t from, int64_t to, int flags, size_t *outlen,
                    char **final_url, int retries, char *err, size_t errlen,
                    volatile LONG64 *counter);

/* ------------------------------------------------------------------ */
/* ftp.c - FTP transport (plugs into http_request/http_read/http_close)*/
/* ------------------------------------------------------------------ */
/* Emulates HTTP semantics: status 206 + range_start/range_total when the
   server supports REST and SIZE, 200 for a whole-file transfer, 404 for
   missing files (550), 401 for login failures (530). ftp_read returns
   got=0 once byte `to` has been delivered. */
int  ftp_request(http_session *s, const char *url, int64_t from, int64_t to,
                 http_response *r, char *err, size_t errlen);
int  ftp_read(http_response *r, void *buf, DWORD len, DWORD *got);
void ftp_close(http_response *r);

/* ------------------------------------------------------------------ */
/* engine.c - segmented multi-connection downloader                    */
/* ------------------------------------------------------------------ */
typedef struct {
    const char       *url;
    const char       *path;         /* final output path (UTF-8)          */
    const ul_headers *headers;      /* may be NULL                        */
    int               connections;
    int64_t           chunk_size;   /* max bytes per HTTP request, 0=none */
} dl_job;

/* probe (optional) is a response to GET url with "Range: bytes=0-";
   ownership passes to dl_run. Returns 0 ok, 1 error, 2 paused. */
int dl_run(http_session *s, const ul_config *cfg, const dl_job *job, http_response *probe);

/* ------------------------------------------------------------------ */
/* hls.c - HLS (m3u8) streams                                          */
/* ------------------------------------------------------------------ */
int hls_run(http_session *s, const ul_config *cfg, const char *url,
            const ul_headers *hd, const char *out_hint);

/* ------------------------------------------------------------------ */
/* extract.c - site extraction through yt-dlp                          */
/* ------------------------------------------------------------------ */
int is_video_site(const char *url);
int ytdlp_run(http_session *s, ul_config *cfg, const char *url);
int ytdlp_install(http_session *s, ul_config *cfg);
/* "yt-dlp -U" (#10): 1 updated, 0 already the latest, -1 failed or no
   yt-dlp. `msg` gets yt-dlp's last line or the reason. */
int ytdlp_self_update(const ul_task *t, char *msg, size_t msglen);

/* U-loader's own updates (#11): the latest release on GitHub (or
   ULOADER_UPDATE_URL, for tests). */
typedef struct {
    char    version[32];         /* "1.4.0" */
    char    page[512];           /* the release page */
    char    setup_url[1024];     /* the setup attached to it, "" = none */
    char    setup_name[128];
    int64_t setup_size;          /* bytes, -1 = unknown */
} ul_release;
int version_newer(const char *a, const char *b);   /* 1 if version a is newer than b */
/* 1 = a newer version is out, 0 = this is the latest, -1 = could not tell (err) */
int release_check(ul_release *rel, char *err, size_t errlen);
/* The picture heights (1080, 720, ...) the video at `url` is offered in,
   highest first, without downloading anything (#9). Returns how many were
   found (0 = none known), or -1 with `err` when yt-dlp is missing or failed.
   `t` (may be NULL) stops yt-dlp when it is aborted. */
int ytdlp_heights(const ul_config *cfg, const char *url, int *heights, int max, const ul_task *t,
                  char *err, size_t errlen);

/* ------------------------------------------------------------------ */
/* torrent.c - BitTorrent (.torrent files and magnet links)            */
/* ------------------------------------------------------------------ */
int is_magnet(const char *s);
/* source: magnet URI or path of a .torrent file; or data/len = contents
   of a .torrent fetched over HTTP (source is then its URL). Saves into
   cfg->out_dir. Progress/abort through http_task(s). 0 ok, 1 error, 2 paused. */
/* Give the "stopped" / "completed" messages to the trackers (sent in the
   background) up to `ms` to go out before the program exits. */
void torrent_flush_announces(int ms);
int torrent_run(http_session *s, ul_config *cfg, const char *source,
                const unsigned char *data, size_t len);

/* ------------------------------------------------------------------ */
/* task.c - dispatch                                                   */
/* ------------------------------------------------------------------ */
/* Download any URL: direct file (http/https/ftp), HLS playlist, torrent,
   or (when allow_extract) a web page handed to yt-dlp. */
int download_url(http_session *s, ul_config *cfg, const char *url,
                 const char *forced_path, const ul_headers *hd,
                 int64_t chunk, int allow_extract);

/* ------------------------------------------------------------------ */
/* host.c - browser integration (native messaging host)                */
/* ------------------------------------------------------------------ */
int native_host_main(void);
int host_install(int uninstall);

#endif
