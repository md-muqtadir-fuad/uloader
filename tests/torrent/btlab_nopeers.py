"""BitTorrent test lab for U-loader (Python 3.8, stdlib only).

  python -I btlab.py mkdata DIR                      sample files (odd sizes, nested, unicode, empty file)
  python -I btlab.py mktorrent SRC OUT.torrent [--plen N] [--tracker URL]... [--name NAME]
  python -I btlab.py hostile OUT.torrent DATA_DIR --tracker URL   torrent with ../ and absolute paths
  python -I btlab.py serve CONFIG.json               HTTP tracker (+ /files/*.torrent) + UDP tracker + seeders
  python -I btlab.py run [SCENARIO...]               the whole local test-suite (see SCENARIOS)
  python -I btlab.py verify TORRENT OUT_DIR          SHA-256/SHA-1 check of a finished download
"""
import hashlib, json, os, random, socket, socketserver, struct, subprocess, sys, threading, time
import http.server, urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(HERE, 'uloader.exe')
SENDCTRLC = os.path.join(os.path.dirname(HERE), 'tools', 'sendctrlc.exe')

# ---------------------------------------------------------------- bencode
def benc(x):
    if isinstance(x, bool):
        x = int(x)
    if isinstance(x, int):
        return b'i%de' % x
    if isinstance(x, str):
        x = x.encode('utf-8')
    if isinstance(x, (bytes, bytearray)):
        return b'%d:' % len(x) + bytes(x)
    if isinstance(x, list):
        return b'l' + b''.join(benc(i) for i in x) + b'e'
    if isinstance(x, dict):
        items = sorted(((k.encode() if isinstance(k, str) else k), v) for k, v in x.items())
        return b'd' + b''.join(benc(k) + benc(v) for k, v in items) + b'e'
    raise TypeError(type(x))

def bdec(b, i=0):
    c = b[i:i + 1]
    if c == b'i':
        e = b.index(b'e', i)
        return int(b[i + 1:e]), e + 1
    if c == b'l':
        i += 1; out = []
        while b[i:i + 1] != b'e':
            v, i = bdec(b, i); out.append(v)
        return out, i + 1
    if c == b'd':
        i += 1; out = {}
        while b[i:i + 1] != b'e':
            k, i = bdec(b, i); v, i = bdec(b, i); out[k] = v
        return out, i + 1
    colon = b.index(b':', i)
    n = int(b[i:colon])
    return b[colon + 1:colon + 1 + n], colon + 1 + n

def info_span(b):
    """raw bytes of the info dict inside a .torrent"""
    assert b[:1] == b'd'
    i = 1
    while b[i:i + 1] != b'e':
        k, i = bdec(b, i)
        start = i
        v, i = bdec(b, i)
        if k == b'info':
            return b[start:i]
    raise ValueError('no info')

# ---------------------------------------------------------------- torrents
def file_list(src):
    """[(relative path components, absolute path)] in a stable order"""
    if os.path.isfile(src):
        return [([os.path.basename(src)], src)]
    out = []
    for root, dirs, files in os.walk(src):
        dirs.sort()
        for f in sorted(files):
            p = os.path.join(root, f)
            rel = os.path.relpath(p, src).split(os.sep)
            out.append((rel, p))
    return out

def make_torrent(src, out, plen=65536, trackers=(), name=None, private=False):
    files = file_list(src)
    h = []
    buf = b''
    for rel, p in files:
        with open(p, 'rb') as f:
            while True:
                chunk = f.read(1 << 20)
                if not chunk:
                    break
                buf += chunk
                while len(buf) >= plen:
                    h.append(hashlib.sha1(buf[:plen]).digest()); buf = buf[plen:]
    if buf:
        h.append(hashlib.sha1(buf).digest())
    info = {'name': name or os.path.basename(os.path.normpath(src)), 'piece length': plen, 'pieces': b''.join(h)}
    if os.path.isfile(src):
        info['length'] = os.path.getsize(src)
    else:
        info['files'] = [{'length': os.path.getsize(p), 'path': rel} for rel, p in files]
    if private:
        info['private'] = 1
    t = {'info': info, 'created by': 'btlab'}
    if trackers:
        t['announce'] = trackers[0]
        t['announce-list'] = [[x] for x in trackers]
    data = benc(t)
    with open(out, 'wb') as f:
        f.write(data)
    return hashlib.sha1(benc(info)).hexdigest()

def write_random(path, size, seed):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    r = random.Random(seed)
    with open(path, 'wb') as f:
        left = size
        while left:
            n = min(left, 1 << 20)
            f.write(r.getrandbits(8 * n).to_bytes(n, 'little') if n else b'')
            left -= n

def mkdata(d):
    write_random(os.path.join(d, 'single', 'big file.bin'), 23456789, 1)
    m = os.path.join(d, 'multi', 'Album \u0995\u09be\u099c')
    sizes = [('a.txt', 1), ('empty.dat', 0), ('b/c.bin', 33333), ('b/d/e.bin', 1000003),
             ('f \u00e9t\u00e9.bin', 5555555), ('g.txt', 77), ('h/i.bin', 2222221), ('z.bin', 65536)]
    for i, (rel, size) in enumerate(sizes):
        write_random(os.path.join(m, *rel.split('/')), size, 100 + i)
    write_random(os.path.join(d, 'slow', 'slow.bin'), 31457283, 7)       # ~30 MB for pause tests
    write_random(os.path.join(d, 'magnet', 'magnet-payload.bin'), 7340033, 9)

def hostile(out, data_dir, trackers):
    """multi-file torrent whose paths try to escape out_dir"""
    paths = [['..', '..', 'evil1.txt'], ['/abs', 'evil2.txt'], ['C:\\Windows', 'evil3.txt'],
             ['a', '..', '..', '..', 'evil4.txt'], ['..'], ['ok', 'fine.txt'], ['con'], ['dup.txt'], ['DUP.txt']]
    r = random.Random(5)
    blobs = [bytes(r.getrandbits(8) for _ in range(1000 + 337 * i)) for i in range(len(paths))]
    os.makedirs(data_dir, exist_ok=True)
    allb = b''.join(blobs)
    with open(os.path.join(data_dir, 'stream.bin'), 'wb') as f:
        f.write(allb)
    plen = 4096
    pieces = b''.join(hashlib.sha1(allb[i:i + plen]).digest() for i in range(0, len(allb), plen))
    info = {'name': '../../escape', 'piece length': plen, 'pieces': pieces,
            'files': [{'length': len(b), 'path': p} for b, p in zip(blobs, paths)]}
    t = {'info': info, 'announce': trackers[0]}
    with open(out, 'wb') as f:
        f.write(benc(t))
    json.dump({'blobs': [hashlib.sha256(b).hexdigest() for b in blobs], 'paths': paths},
              open(out + '.json', 'w'))
    return hashlib.sha1(benc(info)).hexdigest()

# ---------------------------------------------------------------- swarm data access
class Swarm:
    def __init__(self, torrent, data):
        raw = open(torrent, 'rb').read()
        self.info_raw = info_span(raw)
        self.ih = hashlib.sha1(self.info_raw).digest()
        info, _ = bdec(self.info_raw)
        self.plen = info[b'piece length']
        self.np = len(info[b'pieces']) // 20
        self.files = []
        off = 0
        if b'files' in info:
            if os.path.isfile(data):          # hostile: one stream file
                self.stream = data
            else:
                self.stream = None
            for f in info[b'files']:
                p = os.path.join(data, *[x.decode() for x in f[b'path']]) if not self.stream else None
                self.files.append((off, f[b'length'], p))
                off += f[b'length']
        else:
            self.stream = None
            self.files.append((0, info[b'length'], data if os.path.isfile(data) else os.path.join(data, info[b'name'].decode())))
            off = info[b'length']
        self.total = off
        self.lock = threading.Lock()

    def read(self, off, n):
        if self.stream:
            with open(self.stream, 'rb') as f:
                f.seek(off); return f.read(n)
        out = b''
        for fo, fl, p in self.files:
            if n <= 0:
                break
            if fo + fl <= off or fl == 0:
                continue
            take = min(fo + fl - off, n)
            with open(p, 'rb') as f:
                f.seek(off - fo); out += f.read(take)
            off += take; n -= take
        return out

# ---------------------------------------------------------------- seeder
def recvn(s, n):
    b = b''
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise EOFError
        b += c
    return b

class Seeder(threading.Thread):
    """modes: good | corrupt | nometa | badmeta | partial"""
    def __init__(self, swarm, port, mode='good', rate=0, log=None):
        super().__init__(daemon=True)
        self.sw, self.port, self.mode, self.rate, self.log = swarm, port, mode, rate, log
        self.srv = socket.socket(); self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(('127.0.0.1', port)); self.srv.listen(50)
        self.tlock = threading.Lock(); self.tokens = 0.0; self.tlast = time.time()
        self.sent = 0; self.corrupted = 0; self.conns = 0; self.md_served = 0
        self.rnd = random.Random(port)
        self.leech_ok = 0; self.leech_bad = 0; self.unchoked_us = 0; self.done_pieces = set()

    def leeched_pieces_done(self, asked):
        return sum(1 for (_, i) in self.done_pieces if i in asked)

    def throttle(self, n):
        if not self.rate:
            return
        while True:
            with self.tlock:
                now = time.time()
                self.tokens = min(self.rate, self.tokens + (now - self.tlast) * self.rate)
                self.tlast = now
                if self.tokens >= n:
                    self.tokens -= n; return
            time.sleep(0.01)

    def run(self):
        while True:
            c, _ = self.srv.accept()
            threading.Thread(target=self.serve, args=(c,), daemon=True).start()

    def serve(self, c):
        self.conns += 1
        sw = self.sw
        try:
            hs = recvn(c, 68)
            if hs[:20] != b'\x13BitTorrent protocol' or hs[28:48] != sw.ih:
                c.close(); return
            res = bytearray(8); res[5] |= 0x10
            c.sendall(b'\x13BitTorrent protocol' + bytes(res) + sw.ih + b'-PY0001-' + os.urandom(6).hex()[:12].encode())
            have = [True] * sw.np
            if self.mode in ('partial', 'leech'):
                have = [i % 2 == 0 for i in range(sw.np)]
            bf = bytearray((sw.np + 7) // 8)
            for i, h in enumerate(have):
                if h:
                    bf[i // 8] |= 0x80 >> (i % 8)
            c.sendall(struct.pack('>IB', 1 + len(bf), 5) + bytes(bf))
            m = {} if self.mode == 'nometa' else {'ut_metadata': 3}
            eh = {'m': m, 'v': 'btlab'}
            if self.mode != 'nometa':
                eh['metadata_size'] = len(sw.info_raw)
            p = benc(eh)
            if self.mode == 'lateext':
                time.sleep(1.5)
            c.sendall(struct.pack('>IBB', 2 + len(p), 20, 0) + p)
            their_md = 0
            theirs = set(); asked = set(); choked = True
            if self.mode == 'leech':
                c.sendall(struct.pack('>IB', 1, 2))          # interested in what uloader has

            def ask_more():
                if choked:
                    return
                for i in sorted(theirs):
                    if len(asked) - self.leeched_pieces_done(asked) >= 4:
                        break
                    if have[i] or i in asked:
                        continue
                    asked.add(i)
                    plen = min(sw.plen, sw.total - i * sw.plen)
                    for b in range(0, plen, 16384):
                        c.sendall(struct.pack('>IBIII', 13, 6, i, b, min(16384, plen - b)))
            while True:
                ln = struct.unpack('>I', recvn(c, 4))[0]
                if ln == 0:
                    continue
                msg = recvn(c, ln)
                mid = msg[0]
                if mid == 2:
                    c.sendall(struct.pack('>IB', 1, 1))
                elif mid in (0, 1) and self.mode == 'leech':
                    choked = mid == 0
                    if not choked:
                        self.unchoked_us += 1
                    ask_more()
                elif mid == 4 and self.mode == 'leech':
                    theirs.add(struct.unpack('>I', msg[1:5])[0]); ask_more()
                elif mid == 5 and self.mode == 'leech':
                    for i in range(sw.np):
                        if msg[1 + i // 8] & (0x80 >> (i % 8)):
                            theirs.add(i)
                    ask_more()
                elif mid == 7 and self.mode == 'leech':
                    idx, begin = struct.unpack('>II', msg[1:9])
                    block = msg[9:]
                    if block == sw.read(idx * sw.plen + begin, len(block)):
                        self.leech_ok += len(block)
                    else:
                        self.leech_bad += len(block)
                    if begin + len(block) >= min(sw.plen, sw.total - idx * sw.plen):
                        self.done_pieces.add((id(c), idx))
                    ask_more()
                elif mid == 6:
                    idx, begin, length = struct.unpack('>III', msg[1:13])
                    if not have[idx] or length > 131072:
                        continue
                    self.throttle(length)
                    data = bytearray(sw.read(idx * sw.plen + begin, length))
                    if self.mode == 'corrupt' and self.rnd.random() < 0.3:
                        data[self.rnd.randrange(len(data))] ^= 0xFF
                        self.corrupted += 1
                    c.sendall(struct.pack('>IBII', 9 + len(data), 7, idx, begin) + bytes(data))
                    self.sent += len(data)
                elif mid == 20:
                    eid = msg[1]
                    if eid == 0:
                        d, _ = bdec(msg[2:])
                        their_md = d.get(b'm', {}).get(b'ut_metadata', 0)
                    elif eid == 3 and self.mode != 'nometa':
                        d, _ = bdec(msg[2:])
                        if d.get(b'msg_type') == 0:
                            pi = d[b'piece']
                            chunk = sw.info_raw[pi * 16384:(pi + 1) * 16384]
                            if self.mode == 'badmeta':
                                chunk = bytes(len(chunk))
                            hdr = benc({'msg_type': 1, 'piece': pi, 'total_size': len(sw.info_raw)})
                            c.sendall(struct.pack('>IBB', 2 + len(hdr) + len(chunk), 20, their_md) + hdr + chunk)
                            self.md_served += 1
        except (EOFError, OSError, ValueError, KeyError, IndexError):
            pass
        finally:
            c.close()

# ---------------------------------------------------------------- trackers
class State:
    swarms = {}          # info_hash -> [(ip, port)]
    log = None
    torrent_dir = None
    announces = []
    lock = threading.Lock()

def compact(peers):
    return b''.join(socket.inet_aton(ip) + struct.pack('>H', p) for ip, p in peers)

class TrackerHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        u = self.path
        path, _, q = u.partition('?')
        if path.startswith('/files/'):
            name = urllib.parse.unquote(path[7:])
            p = os.path.join(State.torrent_dir, os.path.basename(name))
            if not os.path.isfile(p):
                self.send_error(404); return
            data = open(p, 'rb').read()
            self.send_response(200)
            self.send_header('Content-Type', 'application/x-bittorrent')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers(); self.wfile.write(data); return
        params = {}
        for kv in q.split('&'):
            k, _, v = kv.partition('=')
            params[k] = urllib.parse.unquote_to_bytes(v)
        ih = params.get('info_hash', b'')
        with State.lock:
            State.announces.append({'path': path, 'ih': ih.hex(), 'event': params.get('event', b'').decode(),
                                    'peer_id': params.get('peer_id', b'').decode('latin-1'),
                                    'left': params.get('left', b'').decode(), 'port': params.get('port', b'').decode()})
            json.dump(State.announces, open(State.log, 'w'), indent=0)
        if path == '/garbage':
            body = b'd8:intervali1800e5:peers12:\x7f\x00'           # truncated
        elif path == '/deep':
            body = b'l' * 100000 + b'e' * 100000
        elif path == '/huge':
            body = b'd5:peers99999999999:'
        elif path == '/fail':
            body = benc({'failure reason': 'torrent not registered\x01\x02'})
        elif len(ih) != 20:
            body = benc({'failure reason': 'bad info_hash length %d' % len(ih)})
        else:
            peers = State.swarms.get(ih, [])
            body = benc({'interval': 1800, 'complete': len(peers), 'incomplete': 0, 'peers': compact(peers)})
        self.send_response(200)
        self.send_header('Content-Type', 'text/plain')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers(); self.wfile.write(body)

class ThreadingHTTP(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

def udp_tracker(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(('127.0.0.1', port))
    conns = set()
    while True:
        data, addr = s.recvfrom(2048)
        if len(data) >= 16 and struct.unpack('>QII', data[:16])[0] == 0x41727101980 and struct.unpack('>I', data[8:12])[0] == 0:
            cid = random.getrandbits(64); conns.add(cid)
            s.sendto(struct.pack('>IIQ', 0, struct.unpack('>I', data[12:16])[0], cid), addr)
        elif len(data) >= 98 and struct.unpack('>I', data[8:12])[0] == 1:
            cid, _, tid = struct.unpack('>QII', data[:16])
            if cid not in conns:
                s.sendto(struct.pack('>II', 3, tid) + b'bad connection id', addr); continue
            ih = data[16:36]
            event = struct.unpack('>I', data[80:84])[0]
            with State.lock:
                State.announces.append({'path': 'udp', 'ih': ih.hex(), 'event': str(event)})
                json.dump(State.announces, open(State.log, 'w'), indent=0)
            peers = State.swarms.get(ih, [])
            s.sendto(struct.pack('>IIIII', 1, tid, 1800, 0, len(peers)) + compact(peers), addr)

def serve(cfg):
    State.log = cfg['log']; State.torrent_dir = cfg['torrent_dir']
    seeders = []
    for sw in cfg['swarms']:
        swarm = Swarm(sw['torrent'], sw['data'])
        peers = []
        for sd in sw['seeders']:
            sd_obj = Seeder(swarm, sd['port'], sd.get('mode', 'good'), sd.get('rate', 0))
            sd_obj.start(); seeders.append(sd_obj)
            if not sd.get('hidden'):
                peers.append(('127.0.0.1', sd['port']))
        State.swarms[swarm.ih] = State.swarms.get(swarm.ih, []) + peers
    threading.Thread(target=udp_tracker, args=(cfg['udp_port'],), daemon=True).start()
    httpd = ThreadingHTTP(('127.0.0.1', cfg['http_port']), TrackerHandler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    print('ready', flush=True)
    stats = cfg.get('stats')
    while True:
        time.sleep(1)
        if stats:
            tmp = stats + '.tmp'
            json.dump([{'port': s.port, 'mode': s.mode, 'sent': s.sent, 'corrupted': s.corrupted,
                        'conns': s.conns, 'md_served': s.md_served, 'leech_ok': s.leech_ok, 'leech_bad': s.leech_bad,
                        'unchoked_us': s.unchoked_us} for s in seeders], open(tmp, 'w'))
            os.replace(tmp, stats)

# ---------------------------------------------------------------- verification
def verify(torrent, out_dir, data_root):
    """compare every downloaded file with its original by SHA-256"""
    raw = open(torrent, 'rb').read()
    info, _ = bdec(info_span(raw))
    name = info[b'name'].decode()
    res = []
    if b'files' in info:
        for f in info[b'files']:
            rel = [x.decode() for x in f[b'path']]
            got = os.path.join(out_dir, name, *rel)
            orig = os.path.join(data_root, *rel)
            res.append((os.path.join(name, *rel), sha256(got), sha256(orig)))
    else:
        res.append((name, sha256(os.path.join(out_dir, name)), sha256(data_root)))
    return res

def sha256(p):
    if not os.path.isfile(p):
        return None
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()

# ---------------------------------------------------------------- test runner
LAB = os.path.join(HERE, 'lab-nopeers')
DATA = os.path.join(LAB, 'data')
TOR = os.path.join(LAB, 'torrents')
HTTP_PORT, UDP_PORT = 18680, 18681
RESULTS = []

def run_uloader(args, out_dir, log, wait=True, timeout=300):
    import ctypes
    # the tool that started us may run us with Ctrl+C ignored, which children inherit
    ctypes.windll.kernel32.SetConsoleCtrlHandler(None, False)
    os.makedirs(out_dir, exist_ok=True)
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    si.wShowWindow = 0
    f = open(log, 'ab')
    p = subprocess.Popen([EXE, '-d', out_dir] + args, stdout=f, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                         creationflags=subprocess.CREATE_NEW_CONSOLE, startupinfo=si)
    p.logf = f
    if wait:
        try:
            p.wait(timeout)
        except subprocess.TimeoutExpired:
            p.kill(); p.wait()
            p.returncode = 'TIMEOUT'
        f.close()
    return p

def ctrl_c(p):
    rc = subprocess.call([SENDCTRLC, str(p.pid)])
    if rc:
        print('  sendctrlc failed: %d' % rc)
    return rc

def read_json(p):
    for _ in range(50):
        try:
            return json.load(open(p))
        except ValueError:
            time.sleep(0.1)
    return []

def state_progress(out_dir):
    """fraction of verified pieces according to the .ulbt state file"""
    for f in os.listdir(out_dir):
        if f.endswith('.ulbt'):
            raw = open(os.path.join(out_dir, f), 'rb').read()
            d, _ = bdec(raw[47:])
            bits = d[b'pieces']
            info, _ = bdec(d[b'info'] if isinstance(d[b'info'], bytes) else benc(d[b'info']))
            n = len(info[b'pieces']) // 20 if isinstance(info, dict) else 0
            have = sum(bin(x).count('1') for x in bits)
            return have / max(1, n), f
    return None, None

def record(name, ok, detail):
    RESULTS.append((name, ok, detail))
    print(('PASS ' if ok else 'FAIL ') + name + ': ' + detail, flush=True)

def check_files(name, torrent, out_dir, data_root):
    res = verify(torrent, out_dir, data_root)
    bad = [r for r in res if r[1] is None or r[1] != r[2]]
    total = sum(os.path.getsize(os.path.join(out_dir, r[0])) for r in res if r[1])
    detail = '%d file(s), %d bytes, sha256 %s' % (len(res), total, ' '.join(r[1][:12] for r in res[:3] if r[1]))
    if bad:
        detail += ' MISMATCH: ' + ', '.join(r[0] for r in bad)
    record(name, not bad, detail)
    return not bad

def fresh(d):
    import shutil
    if os.path.exists(d):
        shutil.rmtree(d)
    os.makedirs(d)
    return d

def logtext(p):
    return open(p, 'rb').read().decode('utf-8', 'replace')

def scenario_setup():
    os.makedirs(LAB, exist_ok=True)
    if not os.path.isdir(DATA):
        mkdata(DATA)
    os.makedirs(TOR, exist_ok=True)
    ht = 'http://127.0.0.1:%d/announce' % HTTP_PORT
    ut = 'udp://127.0.0.1:%d/announce' % UDP_PORT
    ih = {}
    ih['single'] = make_torrent(os.path.join(DATA, 'single', 'big file.bin'), os.path.join(TOR, 'single.torrent'), 32768 + 1024, [ht])
    ih['multi'] = make_torrent(os.path.join(DATA, 'multi', 'Album \u0995\u09be\u099c'), os.path.join(TOR, 'multi.torrent'), 65536, [ut])
    ih['slow'] = make_torrent(os.path.join(DATA, 'slow', 'slow.bin'), os.path.join(TOR, 'slow.torrent'), 262144, [ht])
    ih['magnet'] = make_torrent(os.path.join(DATA, 'magnet', 'magnet-payload.bin'), os.path.join(TOR, 'magnet.torrent'), 131072, [ht])
    ih['garbage'] = make_torrent(os.path.join(DATA, 'multi', 'Album \u0995\u09be\u099c'), os.path.join(TOR, 'garbage.torrent'), 16384,
                                 ['http://127.0.0.1:%d/garbage' % HTTP_PORT, 'http://127.0.0.1:%d/deep' % HTTP_PORT,
                                  'http://127.0.0.1:%d/huge' % HTTP_PORT, 'http://127.0.0.1:%d/fail' % HTTP_PORT,
                                  'udp://127.0.0.1:1/dead', ht], name='garbage-test')
    ih['corrupt'] = make_torrent(os.path.join(DATA, 'magnet', 'magnet-payload.bin'), os.path.join(TOR, 'corrupt.torrent'), 65536,
                                 ['http://127.0.0.1:%d/announce' % HTTP_PORT], name='corrupt-test.bin')
    ih['badmeta'] = make_torrent(os.path.join(DATA, 'magnet', 'magnet-payload.bin'), os.path.join(TOR, 'badmeta.torrent'), 65536,
                                 [ht], name='badmeta-test.bin')
    ih['hostile'] = hostile(os.path.join(TOR, 'hostile.torrent'), os.path.join(DATA, 'hostile'), [ht])
    sw = [
        {'torrent': os.path.join(TOR, 'single.torrent'), 'data': os.path.join(DATA, 'single', 'big file.bin'),
         'seeders': [{'port': 18600}, {'port': 18601}, {'port': 18602, 'mode': 'partial'}]},
        {'torrent': os.path.join(TOR, 'multi.torrent'), 'data': os.path.join(DATA, 'multi', 'Album \u0995\u09be\u099c'),
         'seeders': [{'port': 18610}, {'port': 18611}]},
        {'torrent': os.path.join(TOR, 'slow.torrent'), 'data': os.path.join(DATA, 'slow', 'slow.bin'),
         'seeders': [{'port': 18620, 'rate': 1200000}, {'port': 18621, 'rate': 1200000}, {'port': 18622, 'mode': 'leech', 'rate': 300000}]},
        {'torrent': os.path.join(TOR, 'magnet.torrent'), 'data': os.path.join(DATA, 'magnet', 'magnet-payload.bin'),
         'seeders': [{'port': 18631, 'rate': 300000}, {'port': 18632, 'mode': 'nometa', 'rate': 300000}]},
        {'torrent': os.path.join(TOR, 'badmeta.torrent'), 'data': os.path.join(DATA, 'magnet', 'magnet-payload.bin'),
         'seeders': [{'port': 18670, 'mode': 'badmeta'}, {'port': 18671, 'mode': 'lateext'}]},
        {'torrent': os.path.join(TOR, 'garbage.torrent'), 'data': os.path.join(DATA, 'multi', 'Album \u0995\u09be\u099c'),
         'seeders': [{'port': 18640}]},
        {'torrent': os.path.join(TOR, 'corrupt.torrent'), 'data': os.path.join(DATA, 'magnet', 'magnet-payload.bin'),
         'seeders': [{'port': 18650, 'mode': 'corrupt'}, {'port': 18651, 'rate': 1500000}]},
        {'torrent': os.path.join(TOR, 'hostile.torrent'), 'data': os.path.join(DATA, 'hostile', 'stream.bin'),
         'seeders': [{'port': 18660}]},
    ]
    cfg = {'http_port': HTTP_PORT, 'udp_port': UDP_PORT, 'torrent_dir': TOR, 'log': os.path.join(LAB, 'announces.json'),
           'stats': os.path.join(LAB, 'seeders.json'), 'swarms': sw}
    json.dump(cfg, open(os.path.join(LAB, 'serve.json'), 'w'), indent=1)
    srv = subprocess.Popen([sys.executable, '-I', os.path.abspath(__file__), 'serve', os.path.join(LAB, 'serve.json')],
                           stdout=subprocess.PIPE)
    assert srv.stdout.readline().strip() == b'ready'
    return srv, ih

def sc_single(ih):
    out = fresh(os.path.join(LAB, 'out-single')); log = os.path.join(LAB, 'single.log'); open(log, 'w').close()
    t0 = time.time()
    p = run_uloader([os.path.join(TOR, 'single.torrent')], out, log)
    record('single-file .torrent exit code', p.returncode == 0, 'rc=%s in %.1fs' % (p.returncode, time.time() - t0))
    check_files('single-file .torrent content', os.path.join(TOR, 'single.torrent'), out, os.path.join(DATA, 'single', 'big file.bin'))
    left = [f for f in os.listdir(out) if f.endswith(('.ulbt', '.tmp'))]
    record('single-file: no state file left', not left, str(left))
    ann = read_json(os.path.join(LAB, 'announces.json'))
    ok = any(a['ih'] == ih['single'] and a['event'] == 'started' and a['port'] == '6881' for a in ann)
    record('HTTP tracker got the binary info_hash right', ok, 'ih=%s' % ih['single'][:16])
    p = run_uloader([os.path.join(TOR, 'single.torrent')], out, log)
    record('rerun of a finished torrent', p.returncode == 0 and 'Already downloaded' in logtext(log), 'rc=%s' % p.returncode)

def sc_multi(ih):
    out = fresh(os.path.join(LAB, 'out-multi')); log = os.path.join(LAB, 'multi.log'); open(log, 'w').close()
    p = run_uloader([os.path.join(TOR, 'multi.torrent')], out, log)
    record('multi-file .torrent via UDP tracker exit code', p.returncode == 0, 'rc=%s' % p.returncode)
    check_files('multi-file .torrent content', os.path.join(TOR, 'multi.torrent'), out, os.path.join(DATA, 'multi', 'Album \u0995\u09be\u099c'))

def sc_http_torrent(ih):
    out = fresh(os.path.join(LAB, 'out-http')); log = os.path.join(LAB, 'http.log'); open(log, 'w').close()
    p = run_uloader(['http://127.0.0.1:%d/files/multi.torrent' % HTTP_PORT], out, log)
    record('.torrent fetched over HTTP exit code', p.returncode == 0, 'rc=%s' % p.returncode)
    check_files('.torrent fetched over HTTP content', os.path.join(TOR, 'multi.torrent'), out, os.path.join(DATA, 'multi', 'Album \u0995\u09be\u099c'))

def sc_magnet(ih):
    out = fresh(os.path.join(LAB, 'out-magnet')); log = os.path.join(LAB, 'magnet.log'); open(log, 'w').close()
    mag = 'magnet:?xt=urn:btih:%s&dn=magnet-payload.bin&tr=%s' % (ih['magnet'], urllib.parse.quote('http://127.0.0.1:%d/announce' % HTTP_PORT, safe=''))
    t0 = time.time()
    p = run_uloader([mag], out, log)
    txt = logtext(log)
    record('magnet (metadata via ut_metadata) exit code', p.returncode == 0 and 'metadata received' in txt,
           'rc=%s in %.1fs' % (p.returncode, time.time() - t0))
    check_files('magnet content', os.path.join(TOR, 'magnet.torrent'), out, os.path.join(DATA, 'magnet', 'magnet-payload.bin'))

def sc_badmeta(ih):
    out = fresh(os.path.join(LAB, 'out-badmeta')); log = os.path.join(LAB, 'badmeta.log'); open(log, 'w').close()
    mag = 'magnet:?xt=urn:btih:%s&tr=%s' % (ih['badmeta'], urllib.parse.quote('http://127.0.0.1:%d/announce' % HTTP_PORT, safe=''))
    p = run_uloader([mag], out, log)
    txt = logtext(log)
    record('magnet: bad metadata rejected, good metadata used', p.returncode == 0 and 'does not match the info-hash' in txt
           and 'banned peer 127.0.0.1:18570' in txt, 'rc=%s' % p.returncode)
    check_files('content after bad metadata', os.path.join(TOR, 'badmeta.torrent'), out, os.path.join(DATA, 'magnet', 'magnet-payload.bin'))

def wait_progress(out, lo, timeout=60):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            fr, _ = state_progress(out)
        except Exception:
            fr = None
        if fr is not None and fr >= lo:
            return fr
        time.sleep(0.2)
    return None

def sc_pause(ih):
    out = fresh(os.path.join(LAB, 'out-pause')); log = os.path.join(LAB, 'pause.log'); open(log, 'w').close()
    tor = os.path.join(TOR, 'slow.torrent')
    p = run_uloader([tor], out, log, wait=False)
    fr = wait_progress(out, 0.25)
    t0 = time.time()
    rc = ctrl_c(p)
    try:
        p.wait(10)
    except subprocess.TimeoutExpired:
        p.kill(); p.wait()
    dt = time.time() - t0
    p.logf.close()
    fr2, st = state_progress(out)
    record('pause with real Ctrl+C', p.returncode == 2 and dt < 2.0 and st is not None,
           'progress before=%.0f%% rc=%s exit after %.2fs, state %s at %.0f%%' % ((fr or 0) * 100, p.returncode, dt, st, (fr2 or 0) * 100))
    p = run_uloader([tor], out, log)
    txt = logtext(log)
    record('resume after Ctrl+C', p.returncode == 0 and 'resuming at' in txt, 'rc=%s' % p.returncode)
    check_files('content after pause/resume', tor, out, os.path.join(DATA, 'slow', 'slow.bin'))
    st = [x for x in read_json(os.path.join(LAB, 'seeders.json')) if x['mode'] == 'leech'][0]
    record('uploading to an interested peer', st['leech_ok'] > 0 and st['leech_bad'] == 0,
           'leech peer was unchoked %d time(s), received %d verified bytes, %d bad bytes' % (st['unchoked_us'], st['leech_ok'], st['leech_bad']))

def sc_kill(ih):
    out = fresh(os.path.join(LAB, 'out-kill')); log = os.path.join(LAB, 'kill.log'); open(log, 'w').close()
    tor = os.path.join(TOR, 'slow.torrent')
    p = run_uloader([tor], out, log, wait=False)
    fr = wait_progress(out, 0.3)
    time.sleep(1.3)                       # mid-flight, between state saves
    p.kill(); p.wait(); p.logf.close()
    fr2, st = state_progress(out)
    record('hard kill mid-download', st is not None, 'state %s at %.0f%% (seen %.0f%%)' % (st, (fr2 or 0) * 100, (fr or 0) * 100))
    p = run_uloader([tor], out, log)
    txt = logtext(log)
    record('resume after hard kill', p.returncode == 0 and 'resuming at' in txt, 'rc=%s' % p.returncode)
    check_files('content after kill/resume', tor, out, os.path.join(DATA, 'slow', 'slow.bin'))

def sc_magnet_pause(ih):
    """pause a magnet after the metadata arrived; the rerun must not need metadata again"""
    out = fresh(os.path.join(LAB, 'out-magnet-pause')); log = os.path.join(LAB, 'magnet-pause.log'); open(log, 'w').close()
    mag = 'magnet:?xt=urn:btih:%s&tr=%s' % (ih['magnet'], urllib.parse.quote('http://127.0.0.1:%d/announce' % HTTP_PORT, safe=''))
    p = run_uloader([mag], out, log, wait=False)
    fr = wait_progress(out, 0.1)
    ctrl_c(p)
    try:
        p.wait(10)
    except subprocess.TimeoutExpired:
        p.kill(); p.wait()
    p.logf.close()
    n1 = logtext(log).count('metadata received')
    p = run_uloader([mag], out, log)
    txt = logtext(log)
    record('magnet pause/resume keeps the metadata', p.returncode == 0 and txt.count('metadata received') == n1 == 1 and 'resuming at' in txt,
           'rc=%s, metadata fetched %d time(s)' % (p.returncode, txt.count('metadata received')))
    check_files('magnet content after pause/resume', os.path.join(TOR, 'magnet.torrent'), out, os.path.join(DATA, 'magnet', 'magnet-payload.bin'))

def sc_corrupt(ih):
    out = fresh(os.path.join(LAB, 'out-corrupt')); log = os.path.join(LAB, 'corrupt.log'); open(log, 'w').close()
    tor = os.path.join(TOR, 'corrupt.torrent')
    p = run_uloader([tor], out, log)
    txt = logtext(log)
    stats = read_json(os.path.join(LAB, 'seeders.json'))
    ev = [s for s in stats if s['port'] == 18550][0]
    record('corrupt seeder: download completes', p.returncode == 0, 'rc=%s, evil seeder corrupted %d blocks' % (p.returncode, ev['corrupted']))
    record('corrupt pieces detected and peer banned', 'failed the SHA-1 check' in txt and 'banned peer 127.0.0.1:18550' in txt,
           '%d hash-fail lines, banned=%s' % (txt.count('failed the SHA-1 check'), 'banned peer 127.0.0.1:18550' in txt))
    check_files('content despite corrupt seeder', tor, out, os.path.join(DATA, 'magnet', 'magnet-payload.bin'))

def sc_garbage(ih):
    out = fresh(os.path.join(LAB, 'out-garbage')); log = os.path.join(LAB, 'garbage.log'); open(log, 'w').close()
    tor = os.path.join(TOR, 'garbage.torrent')
    p = run_uloader([tor], out, log)
    txt = logtext(log)
    record('malformed tracker replies are survived', p.returncode == 0 and txt.count('invalid reply from the tracker') >= 2 and 'tracker says' in txt,
           'rc=%s; %s' % (p.returncode, ' | '.join(l.strip() for l in txt.splitlines() if 'tracker' in l)))
    check_files('content with hostile trackers', tor, out, os.path.join(DATA, 'multi', 'Album \u0995\u09be\u099c'))

def sc_hostile(ih):
    base = fresh(os.path.join(LAB, 'out-hostile-base'))
    out = os.path.join(base, 'a', 'b', 'out'); os.makedirs(out)
    log = os.path.join(LAB, 'hostile.log'); open(log, 'w').close()
    tor = os.path.join(TOR, 'hostile.torrent')
    p = run_uloader([tor], out, log)
    escaped = []
    for root, dirs, files in os.walk(base):
        for f in files:
            full = os.path.join(root, f)
            if not os.path.abspath(full).startswith(os.path.abspath(out) + os.sep):
                escaped.append(full)
    inside = []
    for root, dirs, files in os.walk(out):
        for f in files:
            inside.append(os.path.relpath(os.path.join(root, f), out))
    meta = json.load(open(tor + '.json'))
    hashes = sorted(sha256(os.path.join(out, f)) for f in inside)
    record('hostile paths stay inside out_dir', p.returncode == 0 and not escaped and sorted(meta['blobs']) == hashes,
           'rc=%s escaped=%s files=%s' % (p.returncode, escaped, sorted(inside)))

def sc_nopeers(ih):
    out = fresh(os.path.join(LAB, 'out-nopeers')); log = os.path.join(LAB, 'nopeers.log'); open(log, 'w').close()
    fake = hashlib.sha1(b'nobody has this %f' % time.time()).hexdigest()
    mag = 'magnet:?xt=urn:btih:%s&tr=%s' % (fake, urllib.parse.quote('http://127.0.0.1:%d/announce' % HTTP_PORT, safe=''))
    t0 = time.time()
    p = run_uloader([mag], out, log, timeout=400)
    txt = logtext(log)
    record('no peers: clean failure', p.returncode == 1 and ('no peers found' in txt or 'could send this torrent' in txt),
           'rc=%s after %.0fs: %s' % (p.returncode, time.time() - t0, [l for l in txt.splitlines() if 'Error' in l]))

SCENARIOS = ['single', 'multi', 'http_torrent', 'magnet', 'badmeta', 'pause', 'kill', 'magnet_pause', 'corrupt', 'garbage', 'hostile']

def run(names):
    srv, ih = scenario_setup()
    try:
        for n in names or SCENARIOS:
            globals()['sc_' + n](ih)
    finally:
        srv.kill()
    print('\n%d passed, %d failed' % (sum(1 for r in RESULTS if r[1]), sum(1 for r in RESULTS if not r[1])))
    return 0 if all(r[1] for r in RESULTS) else 1

def read_state(out_dir):
    for f in os.listdir(out_dir):
        if f.endswith('.ulbt'):
            raw = open(os.path.join(out_dir, f), 'rb').read()
            d, _ = bdec(raw[47:])
            info = d[b'info']
            n = len(info[b'pieces']) // 20
            have = [i for i in range(n) if d[b'pieces'][i // 8] & (0x80 >> (i % 8))]
            return {'file': f, 'np': n, 'plen': info[b'piece length'], 'have': have, 'info': info}
    return None

def real(src, out, target_mb, limit_s=900):
    """run uloader on a real torrent, send Ctrl+C after target_mb verified"""
    os.makedirs(out, exist_ok=True)
    log = os.path.join(out, '..', os.path.basename(out) + '.log')
    p = run_uloader([src], out, log, wait=False)
    t0 = time.time(); got = 0
    while time.time() - t0 < limit_s and p.poll() is None:
        time.sleep(1)
        try:
            st = read_state(out)
        except Exception:
            st = None
        if st:
            got = len(st['have']) * st['plen']
            if got >= target_mb * 1e6:
                break
    t1 = time.time()
    if p.poll() is None:
        ctrl_c(p)
        try:
            p.wait(15)
        except subprocess.TimeoutExpired:
            p.kill(); p.wait()
    t2 = time.time()
    p.logf.close()
    print('rc=%s verified=%.1f MB after %.0fs, exit %.2fs after Ctrl+C' % (p.returncode, got / 1e6, t1 - t0, t2 - t1))

def checkpieces(torrent, out):
    """independently re-hash every piece the state file claims"""
    raw = open(torrent, 'rb').read()
    info, _ = bdec(info_span(raw))
    st = read_state(out)
    assert st and st['info'] == info, 'state does not match the torrent'
    plen, hashes = info[b'piece length'], info[b'pieces']
    path = os.path.join(out, info[b'name'].decode())
    ok = bad = 0
    with open(path, 'rb') as f:
        for i in st['have']:
            f.seek(i * plen)
            if hashlib.sha1(f.read(plen)).digest() == hashes[i * 20:i * 20 + 20]:
                ok += 1
            else:
                bad += 1
    print('%s: %d of %d pieces claimed verified; re-hashed OK %d, BAD %d; file size %d' % (
        st['file'], len(st['have']), st['np'], ok, bad, os.path.getsize(path)))

if __name__ == '__main__':
    a = sys.argv[1:]
    if not a:
        print(__doc__); sys.exit(2)
    cmd = a[0]
    if cmd == 'mkdata':
        mkdata(a[1])
    elif cmd == 'mktorrent':
        trs = [a[i + 1] for i, x in enumerate(a) if x == '--tracker']
        plen = int(a[a.index('--plen') + 1]) if '--plen' in a else 65536
        name = a[a.index('--name') + 1] if '--name' in a else None
        print(make_torrent(a[1], a[2], plen, trs, name))
    elif cmd == 'hostile':
        trs = [a[i + 1] for i, x in enumerate(a) if x == '--tracker']
        print(hostile(a[1], a[2], trs))
    elif cmd == 'serve':
        serve(json.load(open(a[1])))
    elif cmd == 'run':
        sys.exit(run(a[1:]))
    elif cmd == 'real':
        real(a[1], a[2], float(a[3]))
    elif cmd == 'checkpieces':
        checkpieces(a[1], a[2])
    elif cmd == 'verify':
        for r in verify(a[1], a[2], a[3]):
            print(('OK  ' if r[1] and r[1] == r[2] else 'BAD ') + r[0], r[1])
    else:
        print(__doc__); sys.exit(2)
