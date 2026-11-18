"""Hostile/quirky HTTP server for U-loader tests.

usage: python -I evilserver.py PORT ROOT LOGFILE

GET /f/<name>?opt=..&opt=..   serves ROOT/<name>, re-read from disk on every request
  ranges=0          ignore Range (always 200 full body)
  r206first=1       honour Range only for "bytes=0-" (the probe); 200 full body otherwise
  etag=X            send ETag: "X"          (default: no ETag)
  lm=X              send Last-Modified: X   (default: none)
  cd=X              send Content-Disposition: X (url-decoded)
  ct=X              Content-Type (default application/octet-stream)
  delay=S           sleep S seconds per 64 KiB written (throttle)
  swap=NAME&swapafter=N   after N bytes were sent for this path, serve ROOT/NAME instead
  swapetag=Y        ...and send ETag \"Y\" for the swapped content
  liecl=N           claim Content-Length N (200, no ranges), send the real bytes, close
  probestart=K      on the probe (bytes=0-) answer 206 for bytes K-end (labelled correctly)
  stall=1           send headers then never send the body (stall=ranged: only requests with
                    Range start > 0; stall=ts: only *.ts files)
  dropafter=N       close the connection after N body bytes of every response
  chunked=1         no Content-Length, chunked encoding, 200 only
GET /redirloop             302 -> itself
GET /inf?ct=X              infinite chunked body
GET /hang                  accept, read the request, never answer
GET /slow?ct=X             1 byte every 4 s forever (slowloris)
GET /html                  text/html page
"""
import http.server
import os
import re
import sys
import threading
import time
import urllib.parse

port, root, log = int(sys.argv[1]), sys.argv[2], sys.argv[3]
sent = {}
lock = threading.Lock()


def logline(s):
    with lock:
        with open(log, "a", encoding="utf-8") as f:
            f.write(s + "\n")


class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def do_GET(self):
        u = urllib.parse.urlsplit(self.path)
        q = dict(urllib.parse.parse_qsl(u.query, keep_blank_values=True))
        # ROOT/_opts (if present) holds extra options in query-string form, so a
        # test can change the server's behaviour between runs for the SAME URL
        ctl = os.path.join(root, "_opts")
        if os.path.exists(ctl):
            q.update(dict(urllib.parse.parse_qsl(open(ctl).read().strip(), keep_blank_values=True)))
        rng = self.headers.get("Range")
        logline("%s %s range=%s" % (time.strftime("%H:%M:%S"), self.path, rng))
        try:
            if u.path.startswith("/f/"):
                return self.serve_file(u.path[3:], q, rng)
            if u.path == "/redir":
                # /redir?to=URL&code=302 : redirect anywhere (Location sent verbatim)
                self.send_response(int(q.get("code", "302")))
                self.send_header("Location", q.get("to", ""))
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            if u.path.startswith("/hdr"):
                # log every request header, then serve 1000 bytes
                logline("HEADERS %s %s" % (self.path, dict(self.headers.items())))
                body = b"h" * 1000
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            if u.path == "/redirloop":
                self.send_response(302)
                self.send_header("Location", "/redirloop")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            if u.path == "/inf":
                self.send_response(200)
                self.send_header("Content-Type", q.get("ct", "application/vnd.apple.mpegurl"))
                self.send_header("Transfer-Encoding", "chunked")
                self.end_headers()
                chunk = b"#EXTM3U\n" + b"#EXTINF:1,\nseg.ts\n" * 4000
                while True:
                    self.wfile.write(b"%x\r\n%s\r\n" % (len(chunk), chunk))
            if u.path == "/hang":
                time.sleep(3600)
                return
            if u.path == "/slow":
                self.send_response(200)
                self.send_header("Content-Type", q.get("ct", "application/octet-stream"))
                self.send_header("Content-Length", "100000")
                self.end_headers()
                while True:
                    self.wfile.write(b"x")
                    self.wfile.flush()
                    time.sleep(4)
            if u.path == "/html":
                body = b"<html><body>hi</body></html>"
                self.send_response(200)
                self.send_header("Content-Type", "text/html")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            self.send_error(404)
        except (ConnectionResetError, ConnectionAbortedError, BrokenPipeError, OSError):
            pass

    def serve_file(self, name, q, rng):
        key = name
        with lock:
            already = sent.get(key, 0)
        if "swap" in q and already >= int(q.get("swapafter", "0")):
            name = q["swap"]
            if "swapetag" in q:
                q["etag"] = q["swapetag"]
        path = os.path.join(root, urllib.parse.unquote(name))
        if not os.path.isfile(path):
            self.send_error(404)
            return
        data = open(path, "rb").read()
        size = len(data)
        use_ranges = q.get("ranges", "1") != "0" and "liecl" not in q and "chunked" not in q
        start, end, status = 0, size - 1, 200
        m = re.match(r"bytes=(\d+)-(\d*)$", rng or "")
        if m and use_ranges:
            if q.get("r206first") == "1" and rng != "bytes=0-":
                pass
            else:
                start = int(m.group(1))
                end = int(m.group(2)) if m.group(2) else size - 1
                end = min(end, size - 1)
                if start >= size:
                    self.send_response(416)
                    self.send_header("Content-Range", "bytes */%d" % size)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                status = 206
        if status == 206 and rng == "bytes=0-" and "probestart" in q:
            start = int(q["probestart"])   # answer a different (honestly labelled) range
        body = data[start:end + 1]
        cr_start = start
        self.send_response(status)
        self.send_header("Accept-Ranges", "bytes" if use_ranges else "none")
        self.send_header("Content-Type", q.get("ct", "application/octet-stream"))
        if "etag" in q:
            self.send_header("ETag", '"%s"' % q["etag"])
        if "lm" in q:
            self.send_header("Last-Modified", q["lm"])
        if "cd" in q:
            self.send_header("Content-Disposition", q["cd"])
        if "chunked" in q:
            self.send_header("Transfer-Encoding", "chunked")
        elif "liecl" in q:
            self.send_header("Content-Length", q["liecl"])
            self.send_header("Connection", "close")
        else:
            self.send_header("Content-Length", str(len(body)))
        if status == 206:
            self.send_header("Content-Range", "bytes %d-%d/%d" % (cr_start, cr_start + len(body) - 1, size))
        self.end_headers()
        if q.get("stall") == "1" or (q.get("stall") == "ranged" and start > 0) or                 (q.get("stall") == "ts" and name.endswith(".ts")):
            time.sleep(3600)
            return
        delay = float(q.get("delay", "0"))
        drop = int(q.get("dropafter", "0"))
        off = 0
        while off < len(body):
            n = min(65536, len(body) - off)
            if drop and off + n > drop:
                n = drop - off
                if n <= 0:
                    break
            piece = body[off:off + n]
            if "chunked" in q:
                self.wfile.write(b"%x\r\n%s\r\n" % (len(piece), piece))
            else:
                self.wfile.write(piece)
            off += n
            with lock:
                sent[key] = sent.get(key, 0) + n
            if delay:
                time.sleep(delay)
            if drop and off >= drop:
                break
        if "chunked" in q and off >= len(body):
            self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()
        if drop or "liecl" in q:
            self.close_connection = True
            try:
                import socket
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


class S(http.server.ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 128


S(("127.0.0.1", port), H).serve_forever()
