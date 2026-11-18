"""Misbehaving FTP test server for U-loader (Python 3.8, stdlib only).

    python -I ftpserver.py --root DIR [--port 0] [switches]

Prints "PORT <n>" on stdout once it listens. Passive mode only (EPSV/PASV).
Commands: USER PASS SYST FEAT OPTS TYPE MODE STRU PWD CWD CDUP SIZE MDTM REST
EPSV PASV RETR ABOR NOOP QUIT.
"""
import argparse
import datetime
import json
import os
import posixpath
import random
import socket
import socketserver
import sys
import threading
import time

A = None
ROOT = None
LOCK = threading.Lock()
STATE = {"conns": 0, "active": 0, "peak": 0, "logged_in_peak": 0, "logged_in": 0,
         "busy": 0, "retr": 0, "rest": [], "drops": 0, "stalls": 0, "aborts": 0,
         "pasv": 0, "epsv": 0, "bad_login": 0}
LOGF = None

MULTI_GREETING = [
    "220-Welcome to the U-loader test FTP server.",
    "220-",
    " 220 this indented line must not end the reply",
    "230 a line with another code must not end it either",
    "220-220-still going",
    "220 Ready.",
]


def log(msg):
    if LOGF:
        with LOCK:
            LOGF.write("%.3f %s\n" % (time.time(), msg))
            LOGF.flush()


SAVE_LOCK = threading.Lock()


def save_stats():
    """Writes are serialized, so the file always holds the newest counters."""
    if not A.stats:
        return
    with SAVE_LOCK:
        with LOCK:
            data = json.dumps(STATE)
        tmp = A.stats + ".tmp"
        for _ in range(5):
            try:
                with open(tmp, "w") as f:
                    f.write(data)
                os.replace(tmp, A.stats)
                return
            except OSError:
                time.sleep(0.02)


class Handler(socketserver.BaseRequestHandler):
    def setup(self):
        self.sock = self.request
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""
        self.pasv = None
        self.rest = 0
        self.user = None
        self.logged = False
        self.counted = False
        home = "/" + A.home.strip("/") if A.home else "/"
        self.cwd = posixpath.normpath(home)
        with LOCK:
            STATE["conns"] += 1
            self.cid = STATE["conns"]

    # -- output --------------------------------------------------------
    def raw(self, text):
        data = text.encode("utf-8", "surrogateescape")
        if A.chunked:
            i = 0
            while i < len(data):
                n = random.randint(1, 7)
                self.sock.sendall(data[i:i + n])
                i += n
                time.sleep(0.002)
        else:
            self.sock.sendall(data)

    def reply(self, code, text, multi=False):
        if A.slow_reply:
            time.sleep(A.slow_reply)
        log("c%d < %d %s" % (self.cid, code, text))
        if multi and A.multiline:
            self.raw("%d-%s\r\n%d-second line\r\n continuation without code\r\n%d %s\r\n"
                     % (code, text, code, code, "End."))
        else:
            self.raw("%d %s\r\n" % (code, text))

    def readline(self):
        while b"\n" not in self.buf:
            if len(self.buf) > 8192:
                return None
            d = self.sock.recv(4096)
            if not d:
                return None
            self.buf += d
        line, _, self.buf = self.buf.partition(b"\n")
        return line.rstrip(b"\r").decode("utf-8", "surrogateescape")

    # -- session -------------------------------------------------------
    def handle(self):
        with LOCK:
            STATE["active"] += 1
            STATE["peak"] = max(STATE["peak"], STATE["active"])
            over = A.max_conns and STATE["active"] > A.max_conns and A.limit_style == "421"
            if over:
                STATE["busy"] += 1
        save_stats()
        try:
            if over:
                log("c%d 421 busy" % self.cid)
                self.raw("421 Too many connections (%d) from this IP, try again later.\r\n" % A.max_conns)
                return
            if A.no_greeting:
                time.sleep(600)
                return
            if A.multiline:
                if A.slow_reply:
                    time.sleep(A.slow_reply)
                self.raw("\r\n".join(MULTI_GREETING) + "\r\n")
            else:
                self.reply(220, "U-loader test FTP server ready.")
            while True:
                line = self.readline()
                if line is None:
                    break
                cmd, _, arg = line.partition(" ")
                cmd = cmd.upper()
                log("c%d > %s" % (self.cid, "PASS ****" if cmd == "PASS" else line))
                fn = getattr(self, "cmd_" + cmd, None)
                if fn is None:
                    self.reply(502, "Command not implemented.")
                    continue
                if not self.logged and cmd not in ("USER", "PASS", "QUIT", "FEAT", "SYST", "NOOP", "OPTS"):
                    self.reply(530, "Please login with USER and PASS.")
                    continue
                if fn(arg) == "quit":
                    break
        except OSError as e:
            log("c%d error %r" % (self.cid, e))
        finally:
            self.close_pasv()
            with LOCK:
                STATE["active"] -= 1
                if self.counted:
                    STATE["logged_in"] -= 1
            save_stats()

    def resolve(self, arg):
        if not arg:
            return None
        if arg.startswith("/"):
            if A.no_abs:
                return None
            vp = arg
        else:
            vp = posixpath.join(self.cwd, arg)
        vp = posixpath.normpath(vp)
        while vp.startswith("//"):
            vp = vp[1:]
        parts = [p for p in vp.split("/") if p and p != "."]
        if ".." in parts:
            return None
        try:
            real = os.path.join(ROOT, *parts)
        except (ValueError, UnicodeError):
            return None
        return vp, real

    # -- commands ------------------------------------------------------
    def cmd_USER(self, arg):
        self.user = arg
        self.logged = False
        if A.tls_required:
            self.reply(530, "Non-anonymous sessions must use encryption.")
            return
        self.reply(331, "Please specify the password.", multi=True)

    def cmd_PASS(self, arg):
        if self.user is None:
            self.reply(503, "Login with USER first.")
            return
        if A.user is not None and (self.user != A.user or arg != A.password):
            with LOCK:
                STATE["bad_login"] += 1
            time.sleep(0.2)
            self.reply(530, "Login incorrect.")
            return
        with LOCK:
            over = A.max_conns and A.limit_style == "530" and STATE["logged_in"] >= A.max_conns
            if over:
                STATE["busy"] += 1
            else:
                STATE["logged_in"] += 1
                STATE["logged_in_peak"] = max(STATE["logged_in_peak"], STATE["logged_in"])
                self.counted = True
        if over:
            self.reply(530, "Sorry, the maximum number of clients (%d) from your host are already connected."
                       % A.max_conns)
            return
        self.logged = True
        self.reply(230, "Login successful.", multi=True)

    def cmd_SYST(self, arg):
        self.reply(215, "UNIX Type: L8")

    def cmd_FEAT(self, arg):
        feats = ["MDTM", "PASV", "UTF8"]
        if not A.no_epsv:
            feats.append("EPSV")
        if not A.no_rest:
            feats.append("REST STREAM")
        if not A.no_size:
            feats.append("SIZE")
        self.raw("211-Features:\r\n" + "".join(" %s\r\n" % f for f in feats) + "211 End\r\n")

    def cmd_OPTS(self, arg):
        self.reply(200, "Always in UTF8 mode.")

    def cmd_TYPE(self, arg):
        if arg.upper() in ("I", "A", "L 8"):
            self.reply(200, "Switching to %s mode." % ("Binary" if arg.upper() != "A" else "ASCII"), multi=True)
        else:
            self.reply(504, "Bad TYPE.")

    def cmd_MODE(self, arg):
        self.reply(200 if arg.upper() == "S" else 504, "Mode.")

    def cmd_STRU(self, arg):
        self.reply(200 if arg.upper() == "F" else 504, "Structure.")

    def cmd_NOOP(self, arg):
        self.reply(200, "NOOP ok.")

    def cmd_PWD(self, arg):
        self.reply(257, '"%s" is the current directory' % self.cwd.replace('"', '""'))

    def cmd_CWD(self, arg):
        r = self.resolve(arg)
        if not r or not os.path.isdir(r[1]):
            self.reply(550, "Failed to change directory.")
            return
        self.cwd = r[0]
        self.reply(250, "Directory successfully changed.", multi=True)

    def cmd_CDUP(self, arg):
        return self.cmd_CWD("..")

    def cmd_SIZE(self, arg):
        if A.no_size:
            self.reply(502, "SIZE not implemented.")
            return
        r = self.resolve(arg)
        if not r or not os.path.isfile(r[1]):
            self.reply(550, "Could not get file size.")
            return
        self.reply(213, str(os.path.getsize(r[1]) + A.size_delta))

    def cmd_MDTM(self, arg):
        r = self.resolve(arg)
        if not r or not os.path.isfile(r[1]):
            self.reply(550, "Could not get file modification time.")
            return
        t = datetime.datetime.utcfromtimestamp(os.path.getmtime(r[1]))
        self.reply(213, t.strftime("%Y%m%d%H%M%S"))

    def cmd_REST(self, arg):
        if A.no_rest:
            self.reply(502, "REST not implemented.")
            return
        try:
            n = int(arg)
            if n < 0:
                raise ValueError
        except ValueError:
            self.reply(501, "Bad REST argument.")
            return
        self.rest = n
        with LOCK:
            STATE["rest"].append(n)
            if len(STATE["rest"]) > 2000:
                del STATE["rest"][:1000]
        self.reply(350, "Restart position accepted (%d)." % n)

    def open_pasv(self):
        self.close_pasv()
        ls = socket.socket(self.sock.family, socket.SOCK_STREAM)
        ls.bind((self.sock.getsockname()[0], 0))
        ls.listen(1)
        self.pasv = ls
        return ls.getsockname()[1]

    def close_pasv(self):
        if self.pasv:
            try:
                self.pasv.close()
            except OSError:
                pass
            self.pasv = None

    def cmd_EPSV(self, arg):
        if A.no_epsv:
            self.reply(500, "Unknown command.")
            return
        port = self.open_pasv()
        with LOCK:
            STATE["epsv"] += 1
        if A.bad_epsv:
            port = A.bad_epsv           # a port nobody listens on
        self.reply(229, "Entering Extended Passive Mode (|||%d|)" % port)

    def cmd_PASV(self, arg):
        if self.sock.family != socket.AF_INET:
            self.reply(500, "PASV is IPv4 only, use EPSV.")
            return
        port = self.open_pasv()
        with LOCK:
            STATE["pasv"] += 1
        ip = A.pasv_ip or self.sock.getsockname()[0]
        self.reply(227, "Entering Passive Mode (%s,%d,%d)." % (ip.replace(".", ","), port >> 8, port & 255))

    def cmd_ABOR(self, arg):
        with LOCK:
            STATE["aborts"] += 1
        self.reply(225, "No transfer to ABOR.")

    def cmd_QUIT(self, arg):
        self.reply(221, "Goodbye.")
        return "quit"

    def cmd_RETR(self, arg):
        r = self.resolve(arg)
        off, self.rest = self.rest, 0
        if not r or not os.path.isfile(r[1]):
            self.close_pasv()
            self.reply(550, "Failed to open file.")
            return
        if not self.pasv:
            self.reply(425, "Use PORT or PASV first.")
            return
        self.pasv.settimeout(10)
        try:
            ds, _ = self.pasv.accept()
        except OSError:
            self.close_pasv()
            self.reply(425, "Failed to establish connection.")
            return
        self.close_pasv()
        size = os.path.getsize(r[1])
        with LOCK:
            STATE["retr"] += 1
            drop = A.drop_after is not None and (A.drop_times < 0 or STATE["drops"] < A.drop_times)
            if drop:
                STATE["drops"] += 1
            stall = A.stall_after is not None and (A.stall_times < 0 or STATE["stalls"] < A.stall_times)
            if stall:
                STATE["stalls"] += 1
        self.reply(150, "Opening BINARY mode data connection for %s (%d bytes)." % (arg, size))
        sent, result = 0, "ok"
        try:
            with open(r[1], "rb") as f:
                f.seek(off)
                start = time.time()
                while True:
                    chunk = 65536
                    if A.throttle:
                        chunk = max(1024, min(chunk, A.throttle // 20))
                    if drop:
                        if sent >= A.drop_after:
                            result = "drop"
                            break
                        chunk = min(chunk, A.drop_after - sent)
                    if stall:
                        if sent >= A.stall_after:
                            result = "stall"
                            break
                        chunk = min(chunk, A.stall_after - sent)
                    data = f.read(chunk)
                    if not data:
                        break
                    ds.sendall(data)
                    sent += len(data)
                    if A.throttle:
                        d = start + sent / A.throttle - time.time()
                        if d > 0:
                            time.sleep(d)
        except OSError:
            result = "reset"
        if result == "stall":
            ds.settimeout(1)
            t0 = time.time()
            while time.time() - t0 < 300:
                try:
                    if not ds.recv(1):
                        break
                except socket.timeout:
                    continue
                except OSError:
                    break
            result = "reset"
        try:
            if result == "drop" and A.drop_mode == "rst":
                ds.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\x01\x00\x00\x00\x00\x00\x00\x00")
            ds.close()
        except OSError:
            pass
        log("c%d RETR %s off=%d sent=%d result=%s" % (self.cid, arg, off, sent, result))
        save_stats()
        if result == "ok" or (result == "drop" and A.drop_lie):
            self.reply(226, "Transfer complete.", multi=True)
        else:
            self.reply(426, "Connection closed; transfer aborted.")


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = False
    request_queue_size = 128


def main():
    global A, ROOT, LOGF
    p = argparse.ArgumentParser()
    p.add_argument("--root", required=True)
    p.add_argument("--port", type=int, default=0)
    p.add_argument("--ipv6", action="store_true", help="listen on ::1")
    p.add_argument("--stats", help="write counters (JSON) to this file")
    p.add_argument("--log", help="log commands to this file")
    p.add_argument("--home", help="login folder (relative to root)")
    p.add_argument("--user", help="require this user name (otherwise anyone may log in)")
    p.add_argument("--password", default="")
    p.add_argument("--tls-required", action="store_true", help="530 'must use encryption' on USER")
    p.add_argument("--no-rest", action="store_true")
    p.add_argument("--no-size", action="store_true")
    p.add_argument("--no-epsv", action="store_true")
    p.add_argument("--bad-epsv", type=int, default=0, help="EPSV announces this (dead) port")
    p.add_argument("--no-abs", action="store_true", help="absolute paths give 550 (forces CWD)")
    p.add_argument("--pasv-ip", help="announce this IP in PASV replies")
    p.add_argument("--throttle", type=int, default=0, help="bytes/s per data connection")
    p.add_argument("--max-conns", type=int, default=0)
    p.add_argument("--limit-style", choices=["421", "530"], default="421")
    p.add_argument("--drop-after", type=int, help="close the data connection after N bytes")
    p.add_argument("--drop-times", type=int, default=-1, help="only the first K transfers (-1 = all)")
    p.add_argument("--drop-mode", choices=["fin", "rst"], default="fin")
    p.add_argument("--drop-lie", action="store_true", help="reply 226 after a drop")
    p.add_argument("--stall-after", type=int, help="stop sending (keep open) after N bytes")
    p.add_argument("--stall-times", type=int, default=-1)
    p.add_argument("--size-delta", type=int, default=0, help="SIZE lies by this many bytes")
    p.add_argument("--multiline", action="store_true")
    p.add_argument("--chunked", action="store_true", help="send replies in tiny pieces")
    p.add_argument("--slow-reply", type=float, default=0.0, help="seconds before each reply")
    p.add_argument("--no-greeting", action="store_true", help="accept but never greet")
    A = p.parse_args()
    ROOT = os.path.abspath(A.root)
    if A.log:
        LOGF = open(A.log, "a", encoding="utf-8", errors="replace")
    if A.ipv6:
        Server.address_family = socket.AF_INET6
        addr = ("::1", A.port)
    else:
        addr = ("127.0.0.1", A.port)
    srv = Server(addr, Handler)
    save_stats()
    print("PORT %d" % srv.server_address[1], flush=True)
    try:
        srv.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
