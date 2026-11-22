"""Does an FFmpeg build do everything U-loader needs? (#12)

    python -I tests\\ffmpeg_e2e.py FFMPEG_TO_TEST [uloader.exe]

Makes real test media with a full FFmpeg (ffmpeg/ffprobe on PATH, e.g. winget
Gyan.FFmpeg), then runs U-loader with only FFMPEG_TO_TEST next to it:
  1. an HLS stream (H.264 + AAC in MPEG-TS)         -> converted to MP4
  2. an HLS stream with separate audio               -> joined into MP4
  3. a video site: H.264 MP4 + AAC M4A (fake yt-dlp) -> joined into MP4
  4. a video site: VP9 WebM + Opus WebM              -> joined into WebM
and checks every result with ffprobe (codecs, both streams, duration).
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SMALL = os.path.abspath(sys.argv[1])
ULOADER = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "uloader.exe"))
FULL = shutil.which("ffmpeg")
PROBE = shutil.which("ffprobe")
results = []


def ok(name, cond, detail=""):
    results.append(bool(cond))
    print(("PASS " if cond else "FAIL ") + name + ("  " + detail if detail else ""), flush=True)


def ff(*args):
    subprocess.run([FULL, "-hide_banner", "-loglevel", "error", "-y"] + list(args), check=True)


def probe(path):
    if not os.path.exists(path):
        return None
    out = subprocess.run([PROBE, "-v", "error", "-show_streams", "-show_format", "-of", "json", path],
                         stdout=subprocess.PIPE, check=False).stdout
    try:
        j = json.loads(out)
    except ValueError:
        return None
    return sorted(s.get("codec_name") for s in j.get("streams", [])), float(j.get("format", {}).get("duration", 0))


def good(path, codecs):
    p = probe(path)
    return p is not None and p[0] == sorted(codecs) and 2.5 < p[1] < 3.6, p


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def main():
    if not FULL or not PROBE:
        print("needs a full ffmpeg and ffprobe on PATH to make and check the test media")
        return 2
    work = tempfile.mkdtemp(prefix="uloader-ffmpeg-")
    media, app = os.path.join(work, "media"), os.path.join(work, "app")
    os.makedirs(media)
    os.makedirs(app)
    m = lambda n: os.path.join(media, n)
    src_v = ["-f", "lavfi", "-i", "testsrc=d=3:s=320x240:r=25"]
    src_a = ["-f", "lavfi", "-i", "sine=d=3:f=440"]
    h264 = ["-c:v", "libx264", "-pix_fmt", "yuv420p", "-g", "25"]
    ff(*src_v, *h264, "-an", m("v.mp4"))
    ff(*src_a, "-c:a", "aac", "-vn", m("a.m4a"))
    ff(*src_v, "-c:v", "libvpx-vp9", "-b:v", "200k", "-an", m("v.webm"))
    ff(*src_a, "-c:a", "libopus", "-vn", m("a.webm"))
    hls = ["-f", "hls", "-hls_time", "1", "-hls_playlist_type", "vod"]
    ff(*src_v, *src_a, *h264, "-c:a", "aac", *hls, "-hls_segment_filename", m("av%d.ts"), m("av.m3u8"))
    ff(*src_v, *h264, "-an", *hls, "-hls_segment_filename", m("vid%d.ts"), m("vid.m3u8"))
    ff(*src_a, "-c:a", "aac", "-vn", *hls, "-hls_segment_filename", m("aud%d.ts"), m("aud.m3u8"))
    with open(m("both.m3u8"), "w", newline="\n") as f:
        f.write('#EXTM3U\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="a",NAME="en",DEFAULT=YES,URI="aud.m3u8"\n'
                '#EXT-X-STREAM-INF:BANDWIDTH=500000,RESOLUTION=320x240,AUDIO="a"\nvid.m3u8\n')

    # U-loader with only the FFmpeg under test (and a fake yt-dlp) next to it
    shutil.copy2(ULOADER, os.path.join(app, "uloader.exe"))
    shutil.copy2(SMALL, os.path.join(app, "ffmpeg.exe"))
    fake = os.path.join(app, "yt-dlp.exe")
    subprocess.run(["gcc", "-O1", "-municode", os.path.join(HERE, "adversarial", "fake_ytdlp2.c"), "-o", fake], check=True)
    env = dict(os.environ)
    env["PATH"] = os.pathsep.join(p for p in env["PATH"].split(os.pathsep)
                                  if not any(os.path.isfile(os.path.join(p, x)) for x in ("ffmpeg.exe", "yt-dlp.exe")))
    port = free_port()
    srv = subprocess.Popen([sys.executable, "-I", os.path.join(HERE, "server.py"), str(port), media,
                            os.path.join(work, "srv.log")])
    base = "http://127.0.0.1:%d/r/" % port
    time.sleep(0.8)

    def uloader(out, *args, ytout=None):
        e = dict(env)
        if ytout is not None:
            p = os.path.join(work, "yt-%s.txt" % os.path.basename(out))
            with open(p, "w", encoding="utf-8", newline="\n") as f:
                f.write(ytout)
            e["FAKE_YTDLP_OUT"] = p
        r = subprocess.run([os.path.join(app, "uloader.exe"), "-d", out] + list(args),
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=e, timeout=120)
        return r.returncode, r.stdout.decode("utf-8", "replace")

    def ulvid(urls, ext, vext, aext, title):
        j = lambda v: json.dumps(v)
        return "\t".join(["ULVID2", j(ext), j(vext), j(aext), "{}", "NA", j(title), j("x"), j("\n".join(urls))]) + "\n"

    try:
        out = os.path.join(work, "o1")
        rc, log = uloader(out, base + "av.m3u8")
        res, p = good(os.path.join(out, "av.mp4"), ["h264", "aac"])
        ok("HLS stream (MPEG-TS) converted to MP4", rc == 0 and res, "%s %s" % (p, log.strip().splitlines()[-1:]))

        out = os.path.join(work, "o2")
        rc, log = uloader(out, base + "both.m3u8")
        res, p = good(os.path.join(out, "both.mp4"), ["h264", "aac"])
        ok("HLS stream with separate audio joined into MP4", rc == 0 and res, "%s %s" % (p, log.strip().splitlines()[-1:]))

        out = os.path.join(work, "o3")
        rc, log = uloader(out, "--ytdlp", "https://example.com/watch?v=x",
                          ytout=ulvid([base + "v.mp4", base + "a.m4a"], "mp4", "mp4", "m4a", "h264 aac"))
        res, p = good(os.path.join(out, "h264 aac [x].mp4"), ["h264", "aac"])
        ok("H.264 video + AAC audio joined into MP4", rc == 0 and res, "%s %s" % (p, log.strip().splitlines()[-1:]))

        out = os.path.join(work, "o4")
        rc, log = uloader(out, "--ytdlp", "https://example.com/watch?v=x",
                          ytout=ulvid([base + "v.webm", base + "a.webm"], "webm", "webm", "webm", "vp9 opus"))
        res, p = good(os.path.join(out, "vp9 opus [x].webm"), ["vp9", "opus"])
        ok("VP9 video + Opus audio joined into WebM", rc == 0 and res, "%s %s" % (p, log.strip().splitlines()[-1:]))
    finally:
        srv.kill()
    failed = results.count(False)
    print("\n%d/%d checks passed (FFmpeg under test: %s, %.1f MB)" % (len(results) - failed, len(results), SMALL,
                                                                    os.path.getsize(SMALL) / 1e6))
    shutil.rmtree(work, ignore_errors=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
