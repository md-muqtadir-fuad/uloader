/*
 * U-loader - video sites (YouTube, Facebook, TikTok, ... 1800+ sites).
 *
 * Every site hides its video links differently and changes them often, so
 * the link discovery is delegated to yt-dlp (which is maintained daily).
 * yt-dlp is only asked for the direct media URLs; the actual transfer is
 * done by U-loader's own multi-connection engine.
 */
#include "uloader.h"

static const char *VIDEO_SITES[] = {
    "youtube.com", "youtu.be", "youtube-nocookie.com", "facebook.com", "fb.watch",
    "instagram.com", "tiktok.com", "twitter.com", "x.com", "vimeo.com", "dailymotion.com",
    "dai.ly", "twitch.tv", "reddit.com", "redd.it", "soundcloud.com", "bilibili.com",
    "linkedin.com", "pinterest.com", "pin.it", "streamable.com", "vk.com", "vkvideo.ru",
    "ok.ru", "rumble.com", "bandcamp.com", "mixcloud.com", "ted.com", "threads.net",
    "bitchute.com", "odysee.com", "nicovideo.jp", "kick.com", "likee.video", "snapchat.com",
    "9gag.com", "imgur.com", "tumblr.com", "weibo.com", "douyin.com", "coub.com",
};

int is_video_site(const char *url)
{
    char *host = url_host(url);
    int hit = 0;
    for (size_t i = 0; i < sizeof VIDEO_SITES / sizeof *VIDEO_SITES && !hit; i++)
        hit = host_matches(host, VIDEO_SITES[i]);
    free(host);
    return hit;
}

static void parse_headers_json(const char *js, ul_headers *h)
{
    const char *p = json_skip_ws(js);
    if (*p != '{') return;
    p++;
    for (;;) {
        p = json_skip_ws(p);
        sbuf k = { 0 }, v = { 0 };
        if (!json_parse_string(&p, &k)) { sb_free(&k); return; }
        p = json_skip_ws(p);
        if (*p != ':') { sb_free(&k); return; }
        p = json_skip_ws(p + 1);
        if (json_parse_string(&p, &v)) {
            if (k.s && v.s) headers_set(h, k.s, v.s);
        } else {
            const char *e = json_skip(p);
            if (!e) { sb_free(&k); return; }
            p = e;
        }
        sb_free(&k);
        sb_free(&v);
        p = json_skip_ws(p);
        if (*p != ',') return;
        p++;
    }
}

/* ------------------------------------------------------------------ */
typedef struct {
    char      *ext, *vext, *aext, *title;
    ul_headers hd;
    char      *urls[4];
    int        nurls;
} yt_item;

static char *ytdlp_format(const ul_config *cfg, int have_ffmpeg)
{
    if (cfg->format) return xstrdup(cfg->format);
    char h[32] = "";
    if (cfg->max_height > 0) snprintf(h, sizeof h, "[height<=%d]", cfg->max_height);
    /* Prefer plain HTTP(S) files: those get the multi-connection boost. */
    const char *http = "[protocol^=http][protocol!*=dash]";
    /* best choice first, then progressively anything that still works */
    if (have_ffmpeg)
        return xasprintf("bv*%s%s+ba%s/b%s%s/bv*%s+ba/b%s/bv*+ba/b", h, http, http, h, http, h, h);
    /* no ffmpeg: one file that already has sound; else video + audio as separate files */
    return xasprintf("b%s%s/b%s/bv*%s+ba/b/bv*+ba", h, http, h, h);
}

/* last resort when yt-dlp says the requested format is not available */
#define YTDLP_ANY_FORMAT "bv*+ba/b/bv*/ba*"

static char *ytdlp_command(const char *exe, const ul_config *cfg, const char *url, const char *fmt)
{
    sbuf cmd = { 0 };
    cmd_append_arg(&cmd, exe);
    cmd_append_arg(&cmd, "--encoding");
    cmd_append_arg(&cmd, "utf-8");
    cmd_append_arg(&cmd, cfg->playlist ? "--yes-playlist" : "--no-playlist");
    cmd_append_arg(&cmd, "-f");
    cmd_append_arg(&cmd, fmt);
    /* YouTube needs a JavaScript runtime. Hand yt-dlp the Deno we can find
       (next to U-loader, on PATH, or installed after we started) */
    char *deno = find_tool("deno.exe");
    if (deno) {
        char *rt = xasprintf("deno:%s", deno);
        cmd_append_arg(&cmd, "--js-runtimes");
        cmd_append_arg(&cmd, rt);
        free(rt);
        free(deno);
    }
    if (cfg->referer) { cmd_append_arg(&cmd, "--referer"); cmd_append_arg(&cmd, cfg->referer); }
    if (cfg->cookie_file) { cmd_append_arg(&cmd, "--cookies"); cmd_append_arg(&cmd, cfg->cookie_file); }
    if (cfg->cookie) {
        char *hc = xasprintf("Cookie:%s", cfg->cookie);
        cmd_append_arg(&cmd, "--add-header");
        cmd_append_arg(&cmd, hc);
        free(hc);
    }
    for (int i = 0; i < cfg->nheaders; i++) {
        cmd_append_arg(&cmd, "--add-header");
        cmd_append_arg(&cmd, cfg->headers[i]);
    }
    if (cfg->user_agent) { cmd_append_arg(&cmd, "--user-agent"); cmd_append_arg(&cmd, cfg->user_agent); }
    if (cfg->proxy) { cmd_append_arg(&cmd, "--proxy"); cmd_append_arg(&cmd, cfg->proxy); }
    cmd_append_arg(&cmd, "--print");
    cmd_append_arg(&cmd, "ULVID\t%(ext)s\t%(requested_formats.0.ext|)s\t%(requested_formats.1.ext|)s\t"
                         "%(requested_formats.0.http_headers,http_headers)j\t%(title)s [%(id)s]");
    cmd_append_arg(&cmd, "--print");
    cmd_append_arg(&cmd, "urls");
    if (cfg->ytdlp_args) { sb_putc(&cmd, ' '); sb_puts(&cmd, cfg->ytdlp_args); }
    cmd_append_arg(&cmd, "--");
    cmd_append_arg(&cmd, url);
    return sb_steal(&cmd);
}

/* What yt-dlp's messages say about why nothing could be found. */
typedef struct {
    int  need_js, need_login, no_format, unsupported, geo;
    char error[400];
    char warn[3][300];
    int  nwarn;
} yt_diag;

static void diag_line(yt_diag *d, const char *line)
{
    if (str_istr(line, "JavaScript runtime") || str_istr(line, "JS runtime") ||
        str_istr(line, "js-runtimes") || str_istr(line, "n challenge") || str_istr(line, "nsig"))
        d->need_js = 1;
    if (str_istr(line, "Sign in to confirm") || str_istr(line, "not a bot") ||
        str_istr(line, "Private video") || str_istr(line, "members-only") ||
        str_istr(line, "confirm your age") || str_istr(line, "login required") || str_istr(line, "--cookies"))
        d->need_login = 1;
    if (str_istr(line, "Requested format is not available") || str_istr(line, "Only images are available"))
        d->no_format = 1;
    if (str_istr(line, "Unsupported URL")) d->unsupported = 1;
    if (str_istr(line, "not available in your country") || str_istr(line, "geo")) d->geo = 1;
    if (str_istarts(line, "ERROR:")) {
        snprintf(d->error, sizeof d->error, "%s", str_trim((char *)line + 6));
    } else if (str_istarts(line, "WARNING:") && d->nwarn < 3) {
        const char *w = line + 8;
        while (*w == ' ') w++;
        for (int i = 0; i < d->nwarn; i++) if (!strcmp(d->warn[i], w)) return;
        snprintf(d->warn[d->nwarn++], sizeof d->warn[0], "%s", w);
    }
}

static void diag_report(ul_task *t, const yt_diag *d, int code)
{
    if (d->error[0]) tlog(t, LOG_ERR, "yt-dlp: %s", d->error);
    else if (code != 0) tlog(t, LOG_ERR, "yt-dlp could not get this video (exit code %d).", code);
    else tlog(t, LOG_ERR, "no downloadable video found on this page.");
    for (int i = 0; i < d->nwarn; i++) tlog(t, LOG_DIM, "  yt-dlp warning: %s", d->warn[i]);
    if (d->need_js)
        tlog(t, LOG_INFO, "  -> YouTube needs a JavaScript runtime for yt-dlp. Install Deno once:\n"
                          "     winget install DenoLand.Deno   (or put deno.exe next to uloader.exe), then restart U-loader.");
    if (d->need_login)
        tlog(t, LOG_INFO, "  -> The site wants you signed in: send the link from the browser extension\n"
                          "     (right-click > Download with U-loader) so your login cookies come along.");
    if (d->no_format && !d->need_js && !d->need_login)
        tlog(t, LOG_INFO, "  -> yt-dlp got no downloadable formats. Usually the fix is to update yt-dlp\n"
                          "     (Settings > Install / update yt-dlp) and to install Deno: winget install DenoLand.Deno");
    if (d->geo) tlog(t, LOG_INFO, "  -> The video may be blocked in your country.");
    if (d->unsupported) tlog(t, LOG_INFO, "  -> yt-dlp does not know this site or page.");
    if (!d->need_js && !d->need_login && !d->no_format && !d->geo && !d->unsupported)
        tlog(t, LOG_INFO, "  Tip: update yt-dlp (Settings > Install / update yt-dlp, or uloader --get-ytdlp).");
}

/* yt-dlp may print anything it likes; only keep plain extension words */
static char *safe_ext(const char *e, const char *fallback)
{
    size_t n = strlen(e);
    if (n == 0 || n > 8 || strcmp(e, "NA") == 0) return xstrdup(fallback);
    for (size_t i = 0; i < n; i++) if (!isalnum((unsigned char)e[i])) return xstrdup(fallback);
    return xstrdup(e);
}

int ytdlp_run(http_session *s, ul_config *cfg, const char *url)
{
    ul_task *t = http_task(s);
    char *exe = find_tool("yt-dlp.exe");
    if (!exe) {
        tlog(t, LOG_ERR, "this link needs the site extractor (yt-dlp), which is not installed.");
        tlog(t, LOG_INFO, "  Install it once with:   uloader --get-ytdlp   (or the button in the U-loader window)");
        return 1;
    }
    char *ffmpeg = find_tool("ffmpeg.exe");
    char *fmt = ytdlp_format(cfg, ffmpeg != NULL);

    tlog(t, LOG_DIM, "Finding the video with yt-dlp...");
    task_set_stage(t, "%s", "finding video");
    task_progress(t);

    yt_item *items = NULL;
    int n = 0, cap = 0, code = 0;
    yt_diag diag;
    memset(&diag, 0, sizeof diag);
    for (int attempt = 0; attempt < 2 && n == 0 && !task_aborted(t); attempt++) {
        if (attempt == 1) {
            /* the preferred formats do not exist: take whatever there is */
            if (!diag.no_format || cfg->format) break;
            tlog(t, LOG_DIM, "  the preferred formats are not offered - asking yt-dlp for any available format");
            free(fmt);
            fmt = xstrdup(YTDLP_ANY_FORMAT);
        }
        memset(&diag, 0, sizeof diag);
        char *cmd = ytdlp_command(exe, cfg, url, fmt);
        char *outp = NULL;
        code = run_process(cmd, &outp, t);
        free(cmd);

        /* parse "ULVID\t..." blocks followed by their URL lines */
        char *cur = outp, *line;
        while (cur && *cur) {
            char *nl = strchr(cur, '\n');
            if (nl) *nl = 0;
            line = str_trim(cur);
            cur = nl ? nl + 1 : cur + strlen(cur);
            if (strncmp(line, "ULVID\t", 6) == 0) {
                char *f[5];
                char *p = line + 6;
                int k = 0;
                for (; k < 4; k++) {
                    char *tab = strchr(p, '\t');
                    if (!tab) break;
                    *tab = 0;
                    f[k] = p;
                    p = tab + 1;
                }
                if (k < 4) continue;
                f[4] = p;
                if (n == cap) { cap = cap ? cap * 2 : 4; items = xrealloc(items, sizeof *items * (size_t)cap); }
                yt_item *it = &items[n++];
                memset(it, 0, sizeof *it);
                it->ext = safe_ext(f[0], "mp4");
                it->vext = safe_ext(f[1], "mp4");
                it->aext = safe_ext(f[2], "m4a");
                parse_headers_json(f[3], &it->hd);
                it->title = xstrdup(*f[4] ? f[4] : "video");
            } else if (n > 0 && (str_istarts(line, "http://") || str_istarts(line, "https://")) &&
                       !strpbrk(line, " \t\"<>") && items[n - 1].nurls < 4) {   /* real URLs have no spaces */
                items[n - 1].urls[items[n - 1].nurls++] = xstrdup(line);
            } else {
                diag_line(&diag, line);
            }
        }
        free(outp);
    }
    free(fmt);
    free(exe);
    task_set_stage(t, "%s", "");

    if (n == 0) {
        if (!task_aborted(t)) diag_report(t, &diag, code);
        free(ffmpeg);
        return task_aborted(t) ? 2 : 1;
    }
    /* found something, but yt-dlp warned that YouTube formats are limited */
    if (diag.need_js)
        tlog(t, LOG_WARN, "yt-dlp has no JavaScript runtime, so YouTube offers fewer/lower formats. "
                          "Install Deno for full quality: winget install DenoLand.Deno");
    if (n > 1) tlog(t, LOG_INFO, "Playlist: %d videos", n);

    int failures = 0, rc = 0;
    for (int i = 0; i < n && !task_aborted(t); i++) {
        yt_item *it = &items[i];
        if (it->nurls == 0) { tlog(t, LOG_ERR, "no media URL for \"%s\"", it->title); failures++; continue; }
        if (n > 1) tlog(t, LOG_INFO, "[%d/%d]", i + 1, n);

        char *final;
        if (cfg->out_name && n == 1) {
            char *clean = sanitize_filename(path_basename(cfg->out_name));
            final = path_join(cfg->out_dir, clean);
            free(clean);
        } else {
            char *nm = xasprintf("%s.%s", it->title, it->ext);
            char *clean = sanitize_filename(nm);
            final = path_join(cfg->out_dir, clean);
            free(nm);
            free(clean);
        }
        if (file_exists(final) && !cfg->overwrite) {
            tlog(t, LOG_OK, "Already downloaded: %s", path_basename(final));
            task_set_file(t, path_basename(final), final);
            free(final);
            continue;
        }

        /* YouTube & co. throttle long single requests: fetch in 10 MB chunks */
        const int64_t chunk = 10LL * 1024 * 1024;
        if (it->nurls == 1) {
            rc = download_url(s, cfg, it->urls[0], final, &it->hd, chunk, 0);
        } else {
            char *stem = path_strip_ext(final);
            char *vpath = xasprintf("%s.f-video.%s", stem, it->vext);
            char *apath = xasprintf("%s.f-audio.%s", stem, it->aext);
            task_set_stage(t, "%s", "video stream");
            rc = download_url(s, cfg, it->urls[0], vpath, &it->hd, chunk, 0);
            if (rc == 0) {
                task_set_stage(t, "%s", "audio stream");
                rc = download_url(s, cfg, it->urls[1], apath, &it->hd, chunk, 0);
            }
            if (rc == 0) {
                if (ffmpeg) {
                    tlog(t, LOG_DIM, "Merging video + audio...");
                    task_set_stage(t, "%s", "merging");
                    task_progress(t);
                    if (ffmpeg_merge(ffmpeg, vpath, apath, final, t) == 0) {
                        file_delete(vpath);
                        file_delete(apath);
                        task_set_file(t, path_basename(final), final);
                        t->done = t->total = file_size(final);
                        tlog(t, LOG_OK, "Saved: %s", final);
                    } else {
                        tlog(t, LOG_ERR, "ffmpeg could not merge the streams; kept the separate files.");
                        rc = task_aborted(t) ? 2 : 1;
                    }
                } else {
                    tlog(t, LOG_WARN, "ffmpeg not found - video and audio were saved as separate files.");
                }
            }
            task_set_stage(t, "%s", "");
            free(stem);
            free(vpath);
            free(apath);
        }
        free(final);
        if (rc == 2) break;
        if (rc != 0) failures++;
    }

    for (int i = 0; i < n; i++) {
        free(items[i].ext); free(items[i].vext); free(items[i].aext); free(items[i].title);
        headers_free(&items[i].hd);
        for (int k = 0; k < items[i].nurls; k++) free(items[i].urls[k]);
    }
    free(items);
    free(ffmpeg);
    if (task_aborted(t)) return 2;
    return failures ? 1 : 0;
}

/* Download (or update) yt-dlp.exe next to uloader.exe. */
int ytdlp_install(http_session *s, ul_config *cfg)
{
    ul_task *t = http_task(s);
    char *existing = find_tool("yt-dlp.exe");
    if (existing) {
        tlog(t, LOG_INFO, "yt-dlp found at %s - checking for updates...", existing);
        sbuf cmd = { 0 };
        cmd_append_arg(&cmd, existing);
        cmd_append_arg(&cmd, "-U");
        char *out = NULL;
        int r = run_process(cmd.s, &out, t);
        sb_free(&cmd);
        if (out) {
            char *last = str_trim(out);
            char *nl = strrchr(last, '\n');
            tlog(t, r == 0 ? LOG_OK : LOG_WARN, "%s", nl ? nl + 1 : last);
        }
        free(out);
        free(existing);
        return r == 0 ? 0 : 1;
    }
    char *dir = exe_dir();
    char *dest = path_join(dir, "yt-dlp.exe");
    free(dir);
    tlog(t, LOG_INFO, "Downloading yt-dlp (the site extractor)...");
    int saved = cfg->overwrite;
    cfg->overwrite = 1;
    int r = download_url(s, cfg, "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe",
                         dest, NULL, 0, 0);
    cfg->overwrite = saved;
    if (r == 0) tlog(t, LOG_OK, "yt-dlp installed: %s", dest);
    free(dest);
    return r;
}
