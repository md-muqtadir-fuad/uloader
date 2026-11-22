"""U-loader test server: one HTTP server that can misbehave on purpose.

    python -I tests/server.py PORT ROOTDIR LOGFILE

Every request is logged to LOGFILE as one JSON object per line
(path, Range, Cookie, Host, User-Agent).

Paths (FILE is a file name inside ROOTDIR):
  /r/FILE            byte ranges supported         (?kbps=N throttles each connection)
  /n/FILE            ignores Range, always 200
  /c/FILE            no Content-Length, body ends when the connection closes
  /redir/N/FILE      N redirects, then /r/FILE
  /xhost/FILE        redirect to http://localhost:PORT/r/FILE (a different host)
  /loop              redirects to itself forever
  /drop/FILE         ranges, but every response is cut off after 100-300 KB
  /busy/FILE         every 2nd request answers 503
  /cd/FILE?name=X    Content-Disposition: attachment; filename="X"
  /cdstar/FILE?name=X  Content-Disposition with filename*=UTF-8''X
  /cdraw/FILE?h=X    Content-Disposition: X (exactly as given)
  /setcookie/FILE    302 to /needcookie/FILE with Set-Cookie: ulgate=open
  /needcookie/FILE   /r/FILE, but 403 without the ulgate cookie
  /empty             a zero-byte file (ranges supported)
  /page              an HTML page
  /status/CODE       answers with that status code
  /lie/FILE          claims a Content-Length 1000 bytes larger than the file
"""
import json
import os
import random
import re
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT, ROOT, LOG = int(sys.argv[1]), sys.argv[2], sys.argv[3]
_lock = threading.Lock()
_busy_counter = [0]


def log(handler):
    rec = {
        "path": handler.path,
        "range": handler.headers.get("Range"),
        "cookie": handler.headers.get("Cookie"),
        "host": handler.headers.get("Host"),
        "ua": handler.headers.get("User-Agent"),
        "referer": handler.headers.get("Referer"),
        "auth": handler.headers.get("Authorization"),
    }
    with _lock:
        with open(LOG, "a", encoding="utf-8") as f:
            f.write(json.dumps(rec) + "\n")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    # ------------------------------------------------------------------
    def simple(self, code, body=b"", ctype="text/plain", extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if body:
            self.wfile.write(body)

    def redirect(self, location):
        self.send_response(302)
        self.send_header("Location", location)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def send_file(self, path, ranges=True, kbps=0, cut=None, lie=0, close_delimited=False, extra=None,
                  ctype="application/octet-stream", stall_after=None):
        if not os.path.isfile(path):
            self.simple(404, b"not found")
            return
        size = os.path.getsize(path)
        start, end, status = 0, size - 1, 200
        m = re.match(r"bytes=(\d+)-(\d*)$", self.headers.get("Range") or "")
        if ranges and m:
            start = int(m.group(1))
            end = int(m.group(2)) if m.group(2) else size - 1
            end = min(end, size - 1)
            if start >= size:
                self.simple(416, b"", extra={"Content-Range": "bytes */%d" % size})
                return
            status = 206
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        if ranges:
            self.send_header("Accept-Ranges", "bytes")
        if close_delimited:
            self.send_header("Connection", "close")
            self.close_connection = True
        else:
            self.send_header("Content-Length", str(end - start + 1 + lie))
        if status == 206:
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
        self.send_header("ETag", '"%x-%x"' % (size, int(os.path.getmtime(path))))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        left = end - start + 1
        sent = 0
        delay = (65536 / (kbps * 1024)) if kbps else 0
        try:
            with open(path, "rb") as f:
                f.seek(start)
                while left > 0:
                    chunk = f.read(min(65536, left))
                    if not chunk:
                        break
                    if cut is not None and sent + len(chunk) > cut:
                        self.wfile.write(chunk[: max(0, cut - sent)])
                        self.wfile.flush()
                        self.close_connection = True
                        return
                    self.wfile.write(chunk)
                    sent += len(chunk)
                    left -= len(chunk)
                    if stall_after is not None and sent >= stall_after:
                        self.wfile.flush()
                        time.sleep(600)            # hang: the client must not wait for us
                        return
                    if delay:
                        time.sleep(delay)
        except (ConnectionResetError, ConnectionAbortedError, BrokenPipeError):
            self.close_connection = True

    # ------------------------------------------------------------------
    def do_GET(self):
        log(self)
        url = urllib.parse.urlsplit(self.path)
        q = urllib.parse.parse_qs(url.query)
        parts = [urllib.parse.unquote(p) for p in url.path.split("/") if p]
        kbps = int(q.get("kbps", ["0"])[0] or 0)

        def fpath(name):
            return os.path.join(ROOT, os.path.basename(name))

        if not parts:
            return self.simple(404)
        head = parts[0]
        if head == "r" and len(parts) == 2:
            return self.send_file(fpath(parts[1]), kbps=kbps)
        if head == "n" and len(parts) == 2:
            return self.send_file(fpath(parts[1]), ranges=False, kbps=kbps)
        if head == "c" and len(parts) == 2:
            return self.send_file(fpath(parts[1]), ranges=False, close_delimited=True, kbps=kbps)
        if head == "redir" and len(parts) == 3:
            n = int(parts[1])
            nxt = "/redir/%d/%s" % (n - 1, parts[2]) if n > 1 else "/r/%s" % parts[2]
            return self.redirect(nxt)
        if head == "xhost" and len(parts) == 2:
            return self.redirect("http://localhost:%d/r/%s" % (PORT, parts[1]))
        if head == "setcookie" and len(parts) == 2:
            # a gate: sets a cookie on the redirect, the file needs it
            self.send_response(302)
            self.send_header("Location", "/needcookie/%s" % parts[1])
            self.send_header("Set-Cookie", "ulgate=open; Path=/; HttpOnly")
            self.send_header("Set-Cookie", "ulgone=x; Max-Age=0")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if head == "needcookie" and len(parts) == 2:
            if "ulgate=open" not in (self.headers.get("Cookie") or ""):
                return self.simple(403, b"no cookie")
            return self.send_file(fpath(parts[1]), kbps=kbps)
        if head == "loop":
            return self.redirect("/loop")
        if head == "drop" and len(parts) == 2:
            return self.send_file(fpath(parts[1]), cut=random.randint(100, 300) * 1024, kbps=kbps)
        if head == "busy" and len(parts) == 2:
            with _lock:
                _busy_counter[0] += 1
                odd = _busy_counter[0] % 2
            if odd:
                return self.simple(503, b"busy", extra={"Retry-After": "1"})
            return self.send_file(fpath(parts[1]), kbps=kbps)
        if head in ("cd", "cdstar", "cdraw") and len(parts) == 2:
            name = q.get("name", ["x.bin"])[0]
            if head == "cdraw":
                cd = q.get("h", [""])[0]
            elif head == "cd":
                cd = 'attachment; filename="%s"' % name.replace('"', '\\"')
            else:
                cd = "attachment; filename*=UTF-8''%s" % urllib.parse.quote(name, safe="")
            return self.send_file(fpath(parts[1]), extra={"Content-Disposition": cd})
        if head == "empty":
            return self.simple(200, b"", ctype="application/octet-stream")
        if head == "page":
            return self.simple(200, b"<html><body>hello</body></html>", ctype="text/html; charset=utf-8")
        if head == "videopage" and len(parts) == 2:
            html = ('<html><body><video src="/v/%s" preload="auto" muted autoplay></video>'
                    '<a id="dl" href="/r/%s">file</a></body></html>' % (parts[1], parts[1]))
            return self.simple(200, html.encode(), ctype="text/html; charset=utf-8")
        if head == "tagpage" and len(parts) == 2:
            # a player like YouTube's: controls overlay sits on top of the video (?w=&h= size)
            w = int(q.get("w", ["640"])[0])
            h = int(q.get("h", [str(w * 9 // 16)])[0])
            # ?right=1: the player touches the right edge of the window (thumbnail grids)
            pos = "position:absolute;right:0;top:40px;" if q.get("right") else "position:relative;"
            html = ('<html><head><title>Video page</title></head><body style="margin:40px;background:#111">'
                    '<div style="' + pos + 'width:%dpx;height:%dpx">'
                    '<video id="v" src="/v/%s" style="width:%dpx;height:%dpx;background:#000" muted></video>'
                    '<div id="overlay" style="position:absolute;inset:0;background:transparent"></div>'
                    '</div><p style="color:#ccc">Hover the video.</p></body></html>' % (w, h, parts[1], w, h))
            return self.simple(200, html.encode(), ctype="text/html; charset=utf-8")
        if head == "v" and len(parts) == 2:
            return self.send_file(fpath(parts[1]), extra={"X-Kind": "video"}, ctype="video/mp4")
        if head == "status" and len(parts) == 2:
            return self.simple(int(parts[1]), b"status")
        if head == "old" and len(parts) == 2:
            # a file the server says was last modified in 2019
            return self.send_file(fpath(parts[1]), extra={"Last-Modified": "Tue, 01 Jan 2019 00:00:00 GMT"})
        if head == "lie" and len(parts) == 2:
            return self.send_file(fpath(parts[1]), ranges=False, lie=1000)
        if head == "toggle" and len(parts) == 2:
            # range support switched off while ROOT/_norange exists
            ranges = not os.path.exists(os.path.join(ROOT, "_norange"))
            return self.send_file(fpath(parts[1]), ranges=ranges, kbps=kbps)
        if head == "stall" and len(parts) == 2:
            # ranges; any request not starting at 0 sends 64 KB and then hangs
            m = re.match(r"bytes=(\d+)-", self.headers.get("Range") or "")
            if m and int(m.group(1)) > 0:
                return self.send_file(fpath(parts[1]), cut=None, kbps=0, stall_after=65536)
            return self.send_file(fpath(parts[1]), kbps=kbps or 300)
        return self.simple(404, b"unknown test path")


if __name__ == "__main__":
    ThreadingHTTPServer.daemon_threads = True
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
