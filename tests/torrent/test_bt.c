/* Unit tests for src/torrent.c (bencode, magnets, path safety, piece->file
   mapping, and regression tests for the audit findings).
   Build: gcc -std=gnu11 -I src tests/torrent/test_bt.c src/util.c src/http.c src/ftp.c
          src/engine.c src/hls.c src/extract.c src/task.c -static -lwinhttp -lbcrypt
          -lws2_32 -lshell32 -lole32 -loleaut32 -luuid -ladvapi32
   Run:   test_bt.exe [test names...]   (no names: all of them) */
#include "torrent.c"

static int fails, checks;
static char g_base[MAX_PATH];        /* %TEMP%\ulbt-unit-<pid>, removed at the end */
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int dec_ok(const char *s, int maxn)
{
    bdoc d;
    int r = be_decode(&d, s, strlen(s), maxn, NULL);
    be_free(&d);
    return r == 0;
}

static void test_bencode(void)
{
    bdoc d;
    const char *s = "d3:bar4:spam3:fooi42e4:listli1ei-2e0:ee";
    CHECK(be_decode(&d, s, strlen(s), 100, NULL) == 0);
    int64_t v;
    size_t n;
    CHECK(be_int(&d, be_get(&d, 0, "foo"), &v) && v == 42);
    const uint8_t *str = be_str(&d, be_get(&d, 0, "bar"), &n);
    CHECK(str && n == 4 && !memcmp(str, "spam", 4));
    int l = be_get(&d, 0, "list");
    CHECK(be_type(&d, l) == BE_LIST);
    int c = d.n[l].child;
    CHECK(be_int(&d, c, &v) && v == 1);
    c = d.n[c].next;
    CHECK(be_int(&d, c, &v) && v == -2);
    c = d.n[c].next;
    CHECK(be_str(&d, c, &n) && n == 0);
    CHECK(d.n[c].next == -1);
    CHECK(be_get(&d, 0, "missing") == -1);
    CHECK(be_get(&d, l, "foo") == -1);                 /* not a dict */
    /* raw span of a value */
    CHECK(d.n[l].off == (uint32_t)(strstr(s, "listl") + 4 - s) && d.n[l].end == strlen(s) - 1);
    be_free(&d);

    /* integers */
    CHECK(dec_ok("i0e", 10));
    CHECK(dec_ok("i9223372036854775807e", 10));
    CHECK(!dec_ok("i9223372036854775808e", 10));
    CHECK(dec_ok("i-9223372036854775808e", 10));
    CHECK(!dec_ok("i-9223372036854775809e", 10));
    CHECK(!dec_ok("i99999999999999999999999e", 10));
    CHECK(!dec_ok("i-0e", 10));
    CHECK(!dec_ok("i03e", 10));
    CHECK(!dec_ok("ie", 10));
    CHECK(!dec_ok("i-e", 10));
    CHECK(!dec_ok("i12", 10));
    CHECK(!dec_ok("i1x2e", 10));
    {
        bdoc x;
        be_decode(&x, "i-9223372036854775808e", 22, 10, NULL);
        CHECK(x.n[0].ival == INT64_MIN);
        be_free(&x);
    }
    /* strings */
    CHECK(dec_ok("0:", 10));
    CHECK(dec_ok("4:spam", 10));
    CHECK(!dec_ok("4:spa", 10));
    CHECK(!dec_ok("5", 10));
    CHECK(!dec_ok("03:abc", 10));
    CHECK(!dec_ok("99999999999:x", 10));
    CHECK(!dec_ok("4294967296:x", 10));
    CHECK(!dec_ok("18446744073709551617:x", 10));
    CHECK(!dec_ok("-1:x", 10));
    /* containers */
    CHECK(dec_ok("le", 10));
    CHECK(dec_ok("de", 10));
    CHECK(!dec_ok("l", 10));
    CHECK(!dec_ok("li1e", 10));
    CHECK(!dec_ok("d3:fooe", 10));                    /* key without value */
    CHECK(!dec_ok("di1ei2ee", 10));                   /* non-string key */
    CHECK(!dec_ok("dle1:xe", 10));
    CHECK(!dec_ok("x", 10));
    CHECK(!dec_ok("", 10));
    /* element budget */
    {
        sbuf b = { 0 };
        sb_putc(&b, 'l');
        for (int i = 0; i < 100; i++) sb_puts(&b, "i1e");
        sb_putc(&b, 'e');
        CHECK(!dec_ok(b.s, 50));
        CHECK(dec_ok(b.s, 101));
        sb_free(&b);
    }
    /* depth bombs */
    {
        char *bomb = xmalloc(200002);
        memset(bomb, 'l', 100000);
        memset(bomb + 100000, 'e', 100000);
        bomb[200000] = 0;
        bdoc x;
        CHECK(be_decode(&x, bomb, 200000, 1000000, NULL) < 0 && x.err && strstr(x.err, "deeply"));
        be_free(&x);
        /* exactly at the limit: 33 nested lists = depth 32 */
        memset(bomb, 'l', 33);
        memset(bomb + 33, 'e', 33);
        bomb[66] = 0;
        CHECK(dec_ok(bomb, 1000));
        memset(bomb, 'l', 34);
        memset(bomb + 34, 'e', 34);
        bomb[68] = 0;
        CHECK(!dec_ok(bomb, 1000));
        /* dictionaries nested deeply */
        sbuf b = { 0 };
        for (int i = 0; i < 5000; i++) sb_puts(&b, "d1:a");
        sb_puts(&b, "i1e");
        for (int i = 0; i < 5000; i++) sb_putc(&b, 'e');
        CHECK(!dec_ok(b.s, 1000000));
        sb_free(&b);
        free(bomb);
    }
    /* trailing data: allowed, reported through used */
    {
        size_t used = 0;
        bdoc x;
        CHECK(be_decode(&x, "d1:ai1ee<rest>", 14, 10, &used) == 0 && used == 8);
        be_free(&x);
    }
    /* random garbage and mutations never crash and never read out of bounds
       (each buffer is an exact-size heap block) */
    {
        const char *seed = "d8:announce22:http://x.example/ann4:infod6:lengthi12345e4:name5:a.txt"
                           "12:piece lengthi16384e6:pieces20:aaaaaaaaaaaaaaaaaaaaee";
        size_t sl = strlen(seed);
        uint64_t r = 88172645463325252ULL;
        int ok = 0;
        for (int it = 0; it < 300000; it++) {
            r ^= r << 13; r ^= r >> 7; r ^= r << 17;
            size_t n = (it & 1) ? sl : (size_t)(r % 64);
            uint8_t *buf = xmalloc(n ? n : 1);
            if (it & 1) {
                memcpy(buf, seed, sl);
                int muts = 1 + (int)(r % 4);
                for (int k = 0; k < muts; k++) {
                    r ^= r << 13; r ^= r >> 7; r ^= r << 17;
                    buf[r % sl] = (uint8_t)(r >> 32);
                }
                if ((r >> 40) % 3 == 0) n = (size_t)((r >> 20) % sl);       /* truncate */
            } else {
                static const char alpha[] = "ilde0123456789:-x";
                for (size_t k = 0; k < n; k++) {
                    r ^= r << 13; r ^= r >> 7; r ^= r << 17;
                    buf[k] = (uint8_t)alpha[r % (sizeof alpha - 1)];
                }
            }
            bdoc x;
            size_t used = 0;
            if (be_decode(&x, buf, n, 10000, &used) == 0) {
                ok++;
                if (used > n || x.n[0].end != used) { fails++; printf("FAIL fuzz: bad span\n"); }
                for (int k = 0; k < x.nn; k++)
                    if (x.n[k].end > n || (x.n[k].type == BE_STR && (size_t)x.n[k].soff + x.n[k].slen > n)) {
                        fails++;
                        printf("FAIL fuzz: node out of range\n");
                        break;
                    }
            }
            be_free(&x);
            free(buf);
        }
        checks++;
        printf("  fuzz: 300000 inputs, %d decoded, no crash\n", ok);
    }
}

static void test_magnet(void)
{
    magnet m;
    char err[200];
    static const uint8_t want[20] = { 0xc1, 0x2f, 0xe1, 0xc0, 0x6b, 0xba, 0x25, 0x4a, 0x9d, 0xc9,
                                      0xf5, 0x19, 0xb3, 0x35, 0xaa, 0x7c, 0x13, 0x67, 0xa8, 0x8a };
    CHECK(magnet_parse("magnet:?xt=urn:btih:c12fe1c06bba254a9dc9f519b335aa7c1367a88a&dn=Some+Name%20%C3%A9.iso"
                       "&tr=http%3A%2F%2Ftracker.example%3A6969%2Fannounce&tr=udp://t2.example:80"
                       "&x.pe=1.2.3.4:5678", &m, err, sizeof err) == 0);
    CHECK(!memcmp(m.ih, want, 20));
    CHECK(m.dn && !strcmp(m.dn, "Some Name \xC3\xA9.iso"));
    CHECK(m.ntr == 2 && !strcmp(m.tr[0], "http://tracker.example:6969/announce") && !strcmp(m.tr[1], "udp://t2.example:80"));
    CHECK(m.npe == 1 && !strcmp(m.pe[0], "1.2.3.4:5678"));
    magnet_free(&m);
    /* upper-case hex, base32, HTML-escaped ampersands, tr.N */
    CHECK(magnet_parse("magnet:?xt=urn:btih:C12FE1C06BBA254A9DC9F519B335AA7C1367A88A", &m, err, sizeof err) == 0 &&
          !memcmp(m.ih, want, 20));
    magnet_free(&m);
    CHECK(magnet_parse("magnet:?xt=urn:btih:YEX6DQDLXISUVHOJ6UM3GNNKPQJWPKEK&amp;tr.1=http://a.example/ann", &m, err, sizeof err) == 0 &&
          !memcmp(m.ih, want, 20) && m.ntr == 1);
    magnet_free(&m);
    CHECK(magnet_parse("magnet:?dn=x&xt=urn:btih:yex6dqdlxisuvhoj6um3gnnkpqjwpkek", &m, err, sizeof err) == 0 &&
          !memcmp(m.ih, want, 20));
    magnet_free(&m);
    /* hybrid: btih + btmh */
    CHECK(magnet_parse("magnet:?xt=urn:btmh:1220aaaa&xt=urn:btih:c12fe1c06bba254a9dc9f519b335aa7c1367a88a", &m, err, sizeof err) == 0);
    magnet_free(&m);
    /* errors */
    CHECK(magnet_parse("magnet:?xt=urn:btih:c12fe1c06bba254a9dc9f519b335aa7c1367a88", &m, err, sizeof err) != 0 && strstr(err, "invalid"));
    CHECK(magnet_parse("magnet:?xt=urn:btih:c12fe1c06bba254a9dc9f519b335aa7c1367a88g", &m, err, sizeof err) != 0);
    CHECK(magnet_parse("magnet:?xt=urn:btih:YEX6DQDLXISUVHOJ6UM3GNNKPQJWPKE1", &m, err, sizeof err) != 0);
    CHECK(magnet_parse("magnet:?xt=urn:btmh:1220aaaa", &m, err, sizeof err) != 0 && strstr(err, "v2"));
    CHECK(magnet_parse("magnet:?dn=hello", &m, err, sizeof err) != 0 && strstr(err, "no BitTorrent"));
    CHECK(magnet_parse("http://x", &m, err, sizeof err) != 0);
    CHECK(is_magnet("MAGNET:?xt=1") && !is_magnet("magnet:xt") && !is_magnet(NULL));
    /* hostile: a very long magnet with many trackers */
    {
        sbuf b = { 0 };
        sb_puts(&b, "magnet:?xt=urn:btih:c12fe1c06bba254a9dc9f519b335aa7c1367a88a");
        for (int i = 0; i < 1000; i++) sb_printf(&b, "&tr=udp://t%d.example:80&x.pe=%%", i);
        CHECK(magnet_parse(b.s, &m, err, sizeof err) == 0 && m.ntr == MAG_MAX_TR && m.npe == MAG_MAX_TR);
        magnet_free(&m);
        sb_free(&b);
    }
    /* addresses from x.pe */
    baddr a;
    char as[80];
    CHECK(addr_parse("1.2.3.4:5678", &a) && a.fam == 4 && a.port == 5678 && !strcmp(addr_str(&a, as, sizeof as), "1.2.3.4:5678"));
    CHECK(addr_parse("[2001:db8::1]:51413", &a) && a.fam == 6 && a.port == 51413);
    CHECK(addr_parse("[::ffff:10.0.0.1]:80", &a) && a.fam == 4 && a.ip[0] == 10);
    CHECK(!addr_parse("1.2.3.4", &a) && !addr_parse("host.example:80", &a) && !addr_parse("1.2.3.4:0", &a) &&
          !addr_parse("1.2.3.4:70000", &a) && !addr_parse("1.2.3.4:80x", &a));
}

/* build an info dict */
static void put_bstr(sbuf *b, const char *s) { sb_printf(b, "%zu:%s", strlen(s), s); }

static void test_paths(void)
{
    char *c;
    CHECK(!path_component((const uint8_t *)"", 0));
    CHECK(!path_component((const uint8_t *)".", 1));
    CHECK(!path_component((const uint8_t *)"..", 2));
    CHECK(!path_component((const uint8_t *)". .", 3));
    CHECK(!path_component((const uint8_t *)"..  ", 4));
    c = path_component((const uint8_t *)"...", 3); CHECK(c && !strchr(c, '.') ); free(c);
    c = path_component((const uint8_t *)"a/../b", 6); CHECK(c && !strchr(c, '/') && !strstr(c, "..\\")); free(c);
    c = path_component((const uint8_t *)"..\\..\\x", 7); CHECK(c && !strchr(c, '\\')); free(c);
    c = path_component((const uint8_t *)"C:", 2); CHECK(c && !strchr(c, ':')); free(c);
    c = path_component((const uint8_t *)"con", 3); CHECK(c && _stricmp(c, "con") != 0); free(c);
    c = path_component((const uint8_t *)"aux.txt", 7); CHECK(c && _stricmp(c, "aux.txt") != 0); free(c);
    c = path_component((const uint8_t *)"a\0b", 3); CHECK(c && strlen(c) == 3); free(c);
    c = path_component((const uint8_t *)"file.txt:stream", 15); CHECK(c && !strchr(c, ':')); free(c);
    c = path_component((const uint8_t *)"normal name.mkv", 15); CHECK(c && !strcmp(c, "normal name.mkv")); free(c);

    /* hostile multi-file torrent */
    sbuf b = { 0 };
    sb_puts(&b, "d5:filesl");
    const char *paths[][4] = {
        { "..", "..", "evil1.txt", NULL },
        { "/etc", "passwd", NULL },
        { "C:\\Windows", "evil2.dll", NULL },
        { "a", "..", "..", "evil3.txt" },
        { "\\\\server\\share", "x", NULL },
        { "..", NULL },
        { "dup.txt", NULL },
        { "DUP.TXT", NULL },
        { "dir", NULL },
        { "dir", "inside.txt", NULL },
        { "con", "nul.txt", NULL },
    };
    int nfiles = (int)(sizeof paths / sizeof *paths);
    for (int i = 0; i < nfiles; i++) {
        sb_printf(&b, "d6:lengthi%de4:pathl", 10 + i);
        for (int k = 0; k < 4 && paths[i][k]; k++) put_bstr(&b, paths[i][k]);
        sb_puts(&b, "ee");
    }
    int total = 0;
    for (int i = 0; i < nfiles; i++) total += 10 + i;
    sb_puts(&b, "e4:name2:..12:piece lengthi16384e6:pieces20:01234567890123456789e");
    meta m;
    char err[300];
    int r = meta_parse(&m, (const uint8_t *)b.s, b.len, err, sizeof err);
    CHECK(r == 0);
    if (r == 0) {
        CHECK(m.total == total && m.np == 1 && m.multi && m.nfiles == nfiles);
        CHECK(m.dropped == 5);
        char *dir = path_join(getenv("TEMP") ? getenv("TEMP") : ".", "ulbt-unit-out");
        CHECK(meta_layout(&m, dir, "top", err, sizeof err) == 0);
        for (int i = 0; i < m.nfiles; i++) {
            CHECK(path_inside(dir, m.files[i].path));
            CHECK(!strstr(m.files[i].rel, "..\\") && !strstr(m.files[i].rel, ":"));
            printf("  %-40s -> %s\n", paths[i][0], m.files[i].rel);
        }
        /* the case-insensitive duplicate and the file that collides with a folder were renamed */
        CHECK(_stricmp(m.files[6].rel, m.files[7].rel) != 0);
        CHECK(_stricmp(m.files[8].rel, "top\\dir") != 0);
        CHECK(m.renamed == 2);
        free(dir);
        meta_free(&m);
    }
    sb_free(&b);

    /* path_inside itself */
    CHECK(path_inside("C:\\dl", "C:\\dl\\a.txt"));
    CHECK(path_inside("C:\\dl\\", "C:\\dl\\x\\y"));
    CHECK(!path_inside("C:\\dl", "C:\\dl"));
    CHECK(!path_inside("C:\\dl", "C:\\dl\\..\\x"));
    CHECK(!path_inside("C:\\dl", "C:\\dlx\\a"));
    CHECK(!path_inside("C:\\dl", "D:\\dl\\a"));
    CHECK(!path_inside("C:\\dl", "C:\\dl\\a\\..\\..\\b"));
    CHECK(path_inside(".", ".\\a"));

    /* single-file torrent with a hostile name */
    sb_puts(&b, "d6:lengthi100e4:name14:..\\..\\evil.exe12:piece lengthi64e6:pieces40:0123456789012345678901234567890123456789e");
    r = meta_parse(&m, (const uint8_t *)b.s, b.len, err, sizeof err);
    CHECK(r == 0);
    if (r == 0) {
        char *top = sanitize_filename(m.disp);
        CHECK(!strchr(top, '\\'));
        CHECK(meta_layout(&m, "C:\\dl", top, err, sizeof err) == 0 && path_inside("C:\\dl", m.files[0].path));
        free(top);
        meta_free(&m);
    }
    sb_free(&b);

    /* v2-only and malformed metainfo */
    const char *v2 = "d9:file treed1:ad0:d6:lengthi1e11:pieces root32:01234567890123456789012345678901ee"
                     "e12:meta versioni2e4:name1:a12:piece lengthi16384ee";
    CHECK(meta_parse(&m, (const uint8_t *)v2, strlen(v2), err, sizeof err) != 0 && strstr(err, "v2"));
    const char *badcount = "d6:lengthi100e4:name1:a12:piece lengthi64e6:pieces20:01234567890123456789e";
    CHECK(meta_parse(&m, (const uint8_t *)badcount, strlen(badcount), err, sizeof err) != 0);
    const char *badplen = "d6:lengthi100e4:name1:a12:piece lengthi0e6:pieces20:01234567890123456789e";
    CHECK(meta_parse(&m, (const uint8_t *)badplen, strlen(badplen), err, sizeof err) != 0);
    const char *neglen = "d6:lengthi-5e4:name1:a12:piece lengthi64e6:pieces0:e";
    CHECK(meta_parse(&m, (const uint8_t *)neglen, strlen(neglen), err, sizeof err) != 0);
    const char *overflow = "d5:filesld6:lengthi1125899906842624e4:pathl1:aeed6:lengthi1125899906842624e4:pathl1:beee"
                           "4:name1:x12:piece lengthi16384e6:pieces0:e";
    CHECK(meta_parse(&m, (const uint8_t *)overflow, strlen(overflow), err, sizeof err) != 0);
    const char *badpath = "d5:filesld6:lengthi1e4:pathli5eeee4:name1:x12:piece lengthi16384e6:pieces20:01234567890123456789e";
    CHECK(meta_parse(&m, (const uint8_t *)badpath, strlen(badpath), err, sizeof err) != 0);
    /* hybrid: v1 fields win, pad files are recognised */
    const char *hyb = "d9:file treed1:ad0:d6:lengthi1eeee12:meta versioni2e5:filesld6:lengthi5e4:pathl1:aeed4:attr1:p"
                      "6:lengthi11e4:pathl4:.pad2:11eed6:lengthi3e4:pathl1:beee4:name1:h12:piece lengthi16e6:pieces"
                      "40:0123456789012345678901234567890123456789e";
    r = meta_parse(&m, (const uint8_t *)hyb, strlen(hyb), err, sizeof err);
    CHECK(r == 0);
    if (r == 0) { CHECK(m.nfiles == 3 && m.files[1].pad && !m.files[0].pad && m.total == 19 && m.np == 2); meta_free(&m); }
    else printf("  hybrid error: %s\n", err);
}

static void test_mapping(void)
{
    bfile f[6];
    memset(f, 0, sizeof f);
    int64_t lens[6] = { 5, 0, 10, 3, 0, 7 }, off = 0;
    for (int i = 0; i < 6; i++) { f[i].off = off; f[i].len = lens[i]; off += lens[i]; }
    int n;
    fseg *s;
    /* inside one file */
    s = map_range(f, 6, 1, 3, &n);
    CHECK(n == 1 && s[0].fi == 0 && s[0].foff == 1 && s[0].len == 3 && s[0].boff == 0);
    free(s);
    /* across 0 -> (empty 1) -> 2 */
    s = map_range(f, 6, 3, 6, &n);
    CHECK(n == 2 && s[0].fi == 0 && s[0].foff == 3 && s[0].len == 2 && s[1].fi == 2 && s[1].foff == 0 && s[1].len == 4 && s[1].boff == 2);
    free(s);
    /* the whole torrent */
    s = map_range(f, 6, 0, 25, &n);
    CHECK(n == 4 && s[0].fi == 0 && s[1].fi == 2 && s[2].fi == 3 && s[3].fi == 5 && s[3].len == 7 && s[3].boff == 18);
    free(s);
    /* starts exactly on a boundary after empty files */
    s = map_range(f, 6, 15, 4, &n);
    CHECK(n == 2 && s[0].fi == 3 && s[0].foff == 0 && s[0].len == 3 && s[1].fi == 5 && s[1].foff == 0 && s[1].len == 1);
    free(s);
    s = map_range(f, 6, 18, 7, &n);
    CHECK(n == 1 && s[0].fi == 5 && s[0].foff == 0 && s[0].len == 7);
    free(s);
    /* out of range */
    s = map_range(f, 6, 20, 6, &n);
    CHECK(n == -1 && !s);
    s = map_range(f, 6, -1, 2, &n);
    CHECK(n == -1);
    s = map_range(f, 6, 25, 1, &n);
    CHECK(n == -1);
    s = map_range(f, 6, 4, 0, &n);
    CHECK(n == 0 && !s);

    /* exhaustive: every range of a random layout maps back to the bytes */
    uint64_t r = 12345;
    for (int round = 0; round < 200; round++) {
        bfile g[12];
        memset(g, 0, sizeof g);
        int nf = 1 + (int)(r % 12);
        int64_t o = 0;
        for (int i = 0; i < nf; i++) {
            r ^= r << 13; r ^= r >> 7; r ^= r << 17;
            g[i].off = o;
            g[i].len = (int64_t)(r % 4 == 0 ? 0 : r % 40);
            o += g[i].len;
        }
        for (int64_t a = 0; a < o; a++)
            for (int64_t len = 1; a + len <= o; len++) {
                fseg *x = map_range(g, nf, a, len, &n);
                int64_t pos = a, cover = 0;
                int good = n > 0;
                for (int k = 0; k < n && good; k++) {
                    if (g[x[k].fi].off + x[k].foff != pos || x[k].boff != pos - a || x[k].len <= 0 ||
                        x[k].foff + x[k].len > g[x[k].fi].len) good = 0;
                    pos += x[k].len;
                    cover += x[k].len;
                }
                if (!good || cover != len) { fails++; printf("FAIL mapping a=%lld len=%lld\n", (long long)a, (long long)len); round = 1000; a = o; break; }
                free(x);
                checks++;
            }
    }

    /* piece -> files with a real multi-file torrent and stor_io round trip */
    char dir[MAX_PATH];
    snprintf(dir, sizeof dir, "%s\\io", g_base);
    sbuf b = { 0 };
    sb_puts(&b, "d5:filesl");
    int fl[5] = { 7, 0, 20, 1, 13 };
    for (int i = 0; i < 5; i++) sb_printf(&b, "d6:lengthi%de4:pathl3:sub6:f%d.binee", fl[i], i);
    sb_puts(&b, "e4:name2:io12:piece lengthi16e6:pieces60:012345678901234567890123456789012345678901234567890123456789e");
    meta m;
    char err[300];
    CHECK(meta_parse(&m, (const uint8_t *)b.s, b.len, err, sizeof err) == 0);
    CHECK(m.np == 3 && m.total == 41);
    CHECK(meta_layout(&m, dir, "io", err, sizeof err) == 0);
    CHECK(stor_prepare(&m, NULL) == 0);
    uint8_t data[41], back[41];
    for (int i = 0; i < 41; i++) data[i] = (uint8_t)(i * 7 + 1);
    for (int p = 0; p < 3; p++) {
        int len = p == 2 ? 41 - 32 : 16;
        CHECK(stor_io(&m, p * 16, data + p * 16, len, 1) == 0);
    }
    memset(back, 0, sizeof back);
    CHECK(stor_io(&m, 0, back, 41, 0) == 0 && !memcmp(back, data, 41));
    meta_close_files(&m);
    for (int i = 0; i < 5; i++) {
        CHECK(file_size(m.files[i].path) == fl[i]);
        size_t got;
        char *c = read_file(m.files[i].path, &got);
        CHECK(c && got == (size_t)fl[i] && !memcmp(c, data + m.files[i].off, got));
        free(c);
        file_delete(m.files[i].path);
    }
    meta_free(&m);
    sb_free(&b);
}

/* ================================================================== */
/* regression tests for the audit (bug_report.md: C2 C3 H14 H15,       */
/* medium 25-28, low torrent.c items). Built with -DTEST_ORIG they run */
/* against the code before the fixes, to see them fail there.          */
/* ================================================================== */
#ifdef TEST_ORIG
static void helpers_tick(bt *b) { jobs_poll(b); trackers_tick(b); dht_tick(b); }
static int g_no_solo;
#define MD_SOLO(b) g_no_solo
#else
#define MD_SOLO(b) ((b)->md_solo)
#endif

static sbuf g_log;                  /* what the tests' tasks logged */

static void cap_log(ul_task *t, int kind, const char *msg)
{
    (void)t; (void)kind;
    sb_puts(&g_log, msg);
    sb_putc(&g_log, '\n');
}
static void cap_progress(ul_task *t) { (void)t; }
static int  log_has(const char *s) { return g_log.s && strstr(g_log.s, s) != NULL; }
static void log_clear(void) { sb_free(&g_log); }

static ul_task *quiet_task(void)
{
    ul_task *t = task_new("");
    t->on_log = cap_log;
    t->on_progress = cap_progress;
    return t;
}

static char *tdir(const char *name)              /* a folder of its own under g_base */
{
    char *d = path_join(g_base, name);
    dir_create(d);
    return d;
}

static void rm_tree(const wchar_t *dir)
{
    wchar_t pat[1024];
    _snwprintf(pat, 1023, L"%ls\\*", dir);
    pat[1023] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            wchar_t p[1024];
            _snwprintf(p, 1023, L"%ls\\%ls", dir, fd.cFileName);
            p[1023] = 0;
            SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rm_tree(p);
            else DeleteFileW(p);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

/* the test torrents' data: byte k of the stream */
static uint8_t dbyte(int64_t k) { return (uint8_t)(((uint64_t)k * 2654435761u) >> 11); }

static void write_bytes(const char *path, const void *d, size_t n)
{
    char *dir = path_dirname(path);
    dir_create(dir);
    free(dir);
    FILE *f = fopen_u8(path, "wb");
    if (!f) { printf("  cannot write %s\n", path); return; }
    if (n) fwrite(d, 1, n, f);
    fclose(f);
}

/* len bytes of the stream from off, then extra junk bytes */
static void put_data(const char *path, int64_t off, int64_t len, int64_t extra)
{
    uint8_t *d = xmalloc((size_t)(len + extra + 1));
    for (int64_t k = 0; k < len; k++) d[k] = dbyte(off + k);
    memset(d + len, 0xEE, (size_t)extra);
    write_bytes(path, d, (size_t)(len + extra));
    free(d);
}

typedef struct { const char *path; int64_t len; } tfile;

/* an info dictionary with real piece hashes of the dbyte() stream */
static char *mk_info(const char *name, int64_t plen, const tfile *f, int nf, int multi, size_t *len)
{
    int64_t total = 0;
    for (int i = 0; i < nf; i++) total += f[i].len;
    int64_t np = (total + plen - 1) / plen;
    sbuf h = { 0 };
    uint8_t *piece = xmalloc((size_t)plen);
    for (int64_t p = 0; p < np; p++) {
        int64_t pl = total - p * plen < plen ? total - p * plen : plen;
        for (int64_t k = 0; k < pl; k++) piece[k] = dbyte(p * plen + k);
        uint8_t d[20];
        sha1(piece, (size_t)pl, d);
        sb_putn(&h, (const char *)d, 20);
    }
    free(piece);
    sbuf b = { 0 };
    sb_putc(&b, 'd');
    if (multi) {
        sb_puts(&b, "5:filesl");
        for (int i = 0; i < nf; i++) {
            sb_printf(&b, "d6:lengthi%llde4:pathl", (long long)f[i].len);
            for (const char *s = f[i].path; *s;) {
                size_t n = strcspn(s, "/");
                sb_printf(&b, "%zu:", n);
                sb_putn(&b, s, n);
                s += n;
                if (*s == '/') s++;
            }
            sb_puts(&b, "ee");
        }
        sb_putc(&b, 'e');
    } else {
        sb_printf(&b, "6:lengthi%llde", (long long)f[0].len);
    }
    sb_puts(&b, "4:name");
    put_bstr(&b, name);
    sb_printf(&b, "12:piece lengthi%llde6:pieces%zu:", (long long)plen, h.len);
    sb_putn(&b, h.s ? h.s : "", h.len);
    sb_putc(&b, 'e');
    sb_free(&h);
    *len = b.len;
    return sb_steal(&b);
}

/* a torrent as torrent_run() builds it, without network */
static bt *tb_new(const char *out_dir)
{
    bt *b = xcalloc(1, sizeof *b);
    b->t = quiet_task();
    b->cfg = xcalloc(1, sizeof(ul_config));
    cfg_defaults(b->cfg);
    b->out_dir = out_dir;
    b->now = b->t0 = now_sec();
    b->t_tick = b->t_last_peer = b->t_last_data = b->t_fill = b->now;
    b->rng = 88172645463325252ULL;
    b->c = xcalloc(MAX_CANDS, sizeof(cand));
    b->iobuf = xmalloc(131072 + 64);
    b->d.s = INVALID_SOCKET;
    memcpy(b->pid, "-UL1100-unittest0000", 20);
    b->max_act = 8;
    b->ul_rate = 256 * 1024;
    return b;
}

static void tb_free(bt *b)
{
    ul_task *t = b->t;
    ul_config *c = b->cfg;
    bt_free(b);
    task_free(t);
    cfg_free(c);
}

static int tb_meta(bt *b, const char *info, size_t len)     /* as from a .torrent */
{
    char err[300];
    if (meta_parse(&b->m, (const uint8_t *)info, len, err, sizeof err) != 0) { printf("  meta_parse: %s\n", err); return -1; }
    b->have_meta = 1;
    memcpy(b->ih, b->m.ih, 20);
    hex_encode(b->ih, 20, b->ihex);
    return 0;
}

static void tb_magnet(bt *b, const char *info, size_t len)  /* as from a magnet: the hash only */
{
    sha1(info, len, b->ih);
    hex_encode(b->ih, 20, b->ihex);
}

static void tb_tables(bt *b)                 /* piece tables as setup_meta makes them, no files */
{
    int np = b->m.np;
    b->have = xcalloc((size_t)nbytes_of(np), 1);
    b->avail = xcalloc((size_t)np, sizeof(uint16_t));
    b->slot = xmalloc((size_t)np * sizeof(int32_t));
    for (int i = 0; i < np; i++) b->slot[i] = -1;
}

static peer *tp_add(bt *b, int n)            /* a connected peer (no socket) */
{
    uint8_t ip[4] = { 10, 9, (uint8_t)(n >> 8), (uint8_t)n };
    baddr a;
    addr_v4(&a, ip, (uint16_t)(20000 + n));
    cand_add(b, &a, SRC_LINK);
    cand *c = cand_find(b, &a);
    peer *p = xcalloc(1, sizeof *p);
    p->s = INVALID_SOCKET;
    p->a = a;
    p->ci = (int)(c - b->c);
    c->st = CS_CONNECTED;
    p->st = PS_ACTIVE;
    p->am_choking = 1;
    p->md_piece = -1;
    p->reqq = 250;
    p->t_start = p->t_recv = p->t_send = p->t_block = b->now;
    b->p[b->npeers++] = p;
    return p;
}

static void tp_seed(bt *b, peer *p)          /* has everything, unchoked us */
{
    int np = b->m.np, nb = nbytes_of(np);
    free(p->bf);
    p->bf = xmalloc((size_t)nb);
    memset(p->bf, 0xFF, (size_t)nb);
    clear_spare_bits(p->bf, np);
    for (int i = 0; i < np; i++) b->avail[i]++;
    p->nhave = np;
    p->seed = 1;
    p->choked = 0;
    p->am_interested = 1;
}

static void block_data(int piece, int begin, int len, int64_t plen, uint8_t *out)
{
    for (int k = 0; k < len; k++) out[k] = dbyte((int64_t)piece * plen + begin + k);
}

/* p is the start of a heap block (what free() may be given) */
static int heap_block_start(void *p)
{
    HANDLE hs[64];
    DWORD n = GetProcessHeaps(64, hs);
    for (DWORD i = 0; i < n && i < 64; i++) if (HeapValidate(hs[i], 0, p)) return 1;
    return 0;
}

/* C2, L3: magnet links */
static void test_magnet_audit(void)
{
    magnet m;
    char err[200];
    /* trackers with white space around them: trimmed, and magnet_free frees what url_decode returned */
    CHECK(magnet_parse("magnet:?xt=urn:btih:c12fe1c06bba254a9dc9f519b335aa7c1367a88a"
                       "&tr=%20%20http://a.example/ann%20&tr= %09udp://b.example:80&tr=%20", &m, err, sizeof err) == 0);
    CHECK(m.ntr == 3 && !strcmp(m.tr[0], "http://a.example/ann") && !strcmp(m.tr[1], "udp://b.example:80") && !*m.tr[2]);
    for (int i = 0; i < m.ntr; i++) CHECK(heap_block_start(m.tr[i]));
    magnet_free(&m);
    /* a #fragment is not part of the link's parameters */
    CHECK(magnet_parse("magnet:?dn=Name&xt=urn:btih:c12fe1c06bba254a9dc9f519b335aa7c1367a88a#frag", &m, err, sizeof err) == 0);
    CHECK(m.dn && !strcmp(m.dn, "Name"));
    magnet_free(&m);
    CHECK(magnet_parse("magnet:?xt=urn:btih:c12fe1c06bba254a9dc9f519b335aa7c1367a88a&dn=My+File#x&tr=http://x.example/a",
                       &m, err, sizeof err) == 0 && m.dn && !strcmp(m.dn, "My File") && m.ntr == 0);
    magnet_free(&m);
    CHECK(magnet_parse("magnet:?xt=urn:btih:c12fe1c06bba254a9dc9f519b335aa7c1367a88a&dn=a%23b", &m, err, sizeof err) == 0 &&
          m.dn && !strcmp(m.dn, "a#b"));                 /* an encoded # is part of the value */
    magnet_free(&m);
    CHECK(magnet_parse("magnet:?#xt=urn:btih:c12fe1c06bba254a9dc9f519b335aa7c1367a88a", &m, err, sizeof err) != 0);
}

/* L2: an empty path.utf-8 / name.utf-8 falls back to path / name */
static void test_utf8_fallback(void)
{
    meta m;
    char err[300];
    const char *s1 = "d5:filesld6:lengthi5e4:pathl8:real.txte10:path.utf-8leed6:lengthi5e4:pathl5:b.txteee"
                     "4:name3:top12:piece lengthi16e6:pieces20:01234567890123456789e";
    int r = meta_parse(&m, (const uint8_t *)s1, strlen(s1), err, sizeof err);
    CHECK(r == 0);
    if (r == 0) { CHECK(!strcmp(m.files[0].sub, "real.txt") && !strcmp(m.files[1].sub, "b.txt")); meta_free(&m); }
    /* nothing usable left of path.utf-8, but path is fine */
    const char *s2 = "d5:filesld6:lengthi5e4:pathl4:goode10:path.utf-8l2:..eee"
                     "4:name3:top12:piece lengthi16e6:pieces20:01234567890123456789e";
    r = meta_parse(&m, (const uint8_t *)s2, strlen(s2), err, sizeof err);
    CHECK(r == 0);
    if (r == 0) { CHECK(!strcmp(m.files[0].sub, "good")); meta_free(&m); }
    /* a usable path.utf-8 still wins */
    const char *s3 = "d5:filesld6:lengthi5e4:pathl1:ae10:path.utf-8l2:\xC3\xA9" "eee"
                     "4:name3:top12:piece lengthi16e6:pieces20:01234567890123456789e";
    r = meta_parse(&m, (const uint8_t *)s3, strlen(s3), err, sizeof err);
    CHECK(r == 0);
    if (r == 0) { CHECK(!strcmp(m.files[0].sub, "\xC3\xA9")); meta_free(&m); }
    /* neither has anything: "file 1" as before */
    const char *s4 = "d5:filesld6:lengthi5e4:pathl2:..e10:path.utf-8leee"
                     "4:name3:top12:piece lengthi16e6:pieces20:01234567890123456789e";
    r = meta_parse(&m, (const uint8_t *)s4, strlen(s4), err, sizeof err);
    CHECK(r == 0);
    if (r == 0) { CHECK(!strcmp(m.files[0].sub, "file 1")); meta_free(&m); }
    const char *s5 = "d6:lengthi5e4:name5:plain10:name.utf-80:12:piece lengthi16e6:pieces20:01234567890123456789e";
    r = meta_parse(&m, (const uint8_t *)s5, strlen(s5), err, sizeof err);
    CHECK(r == 0);
    if (r == 0) { CHECK(!strcmp(m.disp, "plain")); meta_free(&m); }
}

/* H15 (2nd part): many files of one name - linear time, and no two left on one path */
static void test_layout_many_equal_names(void)
{
    int n = 150000;
    sbuf b = { 0 };
    sb_puts(&b, "d5:filesl");
    sb_puts(&b, "d6:lengthi1e4:pathl9:a (7).txtee");          /* a real name the renaming must step over */
    for (int i = 0; i < n; i++) sb_puts(&b, i % 3 ? "d6:lengthi1e4:pathl5:a.txtee" : "d6:lengthi1e4:pathl5:A.TXTee");
    int np = (n + 1 + 16383) / 16384;
    sb_printf(&b, "e4:name4:same12:piece lengthi16384e6:pieces%d:", np * 20);
    for (int i = 0; i < np * 20; i++) sb_putc(&b, 'x');
    sb_putc(&b, 'e');
    meta m;
    char err[300];
    int r = meta_parse(&m, (const uint8_t *)b.s, b.len, err, sizeof err);
    CHECK(r == 0);
    if (r == 0) {
        double t0 = now_sec();
        CHECK(meta_layout(&m, g_base, "same", err, sizeof err) == 0);
        double dt = now_sec() - t0;
        nameset seen = { 0 };
        int dups = 0;
        for (int i = 0; i < m.nfiles; i++) {
            char *k = lower_key(m.files[i].rel);
            if (nameset_has(&seen, k)) dups++;
            nameset_add(&seen, k);
            free(k);
        }
        nameset_free(&seen);
        printf("  %d files of one name laid out in %.2fs, %d renamed, %d duplicate paths\n", n, dt, m.renamed, dups);
        CHECK(dups == 0 && m.renamed == n - 1);
        CHECK(!_stricmp(m.files[0].rel, "same\\a (7).txt") && !_stricmp(m.files[7].rel, "same\\a (8).txt"));
        CHECK(dt < 30);
        meta_free(&m);
    }
    sb_free(&b);
}

/* M26: checking and uploading need only read access */
static void test_readonly_files(void)
{
    char *dir = tdir("m26");
    tfile f[1] = { { "", 40000 } };
    size_t il;
    char *info = mk_info("ro.bin", 16384, f, 1, 0, &il);
    char *path = path_join(dir, "ro.bin");
    put_data(path, 0, 40000, 0);
    wchar_t *w = wpath(path);
    SetFileAttributesW(w, FILE_ATTRIBUTE_READONLY);
    bt *b = tb_new(dir);                        /* a complete, read-only file is checked, not re-downloaded */
    CHECK(tb_meta(b, info, il) == 0);
    log_clear();
    CHECK(setup_meta(b, NULL, NULL, 0) == 0);
    CHECK(b->nhave == b->m.np && b->top && !strcmp(b->top, "ro.bin"));
    tb_free(b);
    SetFileAttributesW(w, FILE_ATTRIBUTE_NORMAL);
    /* another program has the file open for writing */
    HANDLE other = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(other != INVALID_HANDLE_VALUE);
    meta m;
    char err[300];
    CHECK(meta_parse(&m, (const uint8_t *)info, il, err, sizeof err) == 0);
    CHECK(meta_layout(&m, dir, "ro.bin", err, sizeof err) == 0);
    uint8_t buf[100], want[100];
    for (int k = 0; k < 100; k++) want[k] = dbyte(20000 + k);
    CHECK(stor_io(&m, 20000, buf, 100, 0) == 0 && !memcmp(buf, want, 100));
    CloseHandle(other);
    /* a write after a read gets a handle that can write */
    memset(buf, 0x5A, sizeof buf);
    CHECK(stor_io(&m, 100, buf, 100, 1) == 0);
    meta_close_files(&m);
    size_t got;
    char *c = read_file(path, &got);
    CHECK(c && got == 40000 && (uint8_t)c[100] == 0x5A && (uint8_t)c[199] == 0x5A && (uint8_t)c[200] == dbyte(200));
    free(c);
    meta_free(&m);
    free(w);
    free(path);
    free(info);
    free(dir);
}

/* M28 (4th part): a complete download still gets exact file sizes */
static void test_complete_exact_sizes(void)
{
    char *dir = tdir("m28d");
    tfile f[3] = { { "a.bin", 5000 }, { "empty.dat", 0 }, { "sub/c.bin", 30000 } };
    size_t il;
    char *info = mk_info("fix", 16384, f, 3, 1, &il);
    char *top = path_join(dir, "fix");
    char *pa = path_join(top, "a.bin"), *pe = path_join(top, "empty.dat"), *pc = path_join(top, "sub\\c.bin");
    put_data(pa, 0, 5000, 100);                 /* junk after the end */
    put_data(pc, 5000, 30000, 0);               /* and empty.dat is missing */
    bt *b = tb_new(dir);
    CHECK(tb_meta(b, info, il) == 0);
    CHECK(setup_meta(b, NULL, NULL, 0) == 0);
    CHECK(b->nhave == b->m.np && !strcmp(b->top, "fix"));
    meta_close_files(&b->m);
    CHECK(file_size(pa) == 5000);
    CHECK(file_size(pe) == 0);
    CHECK(file_size(pc) == 30000);
    tb_free(b);
    free(pa); free(pe); free(pc); free(top); free(info); free(dir);
}

/* M28 (2nd part): another torrent's state file under our name */
static void test_foreign_state(void)
{
    char *dir = tdir("m28b");
    tfile f[1] = { { "", 1000 } };
    size_t il;
    char *info = mk_info("name.bin", 16384, f, 1, 0, &il);
    char *fst = path_join(dir, "name.bin.ulbt");
    const char *junk = "ULBT1 0123456789012345678901234567890123456789\nd4:infoi0ee";
    write_bytes(fst, junk, strlen(junk));
    bt *b = tb_new(dir);
    CHECK(tb_meta(b, info, il) == 0);
    CHECK(setup_meta(b, NULL, NULL, 0) == 0);
    CHECK(!strcmp(b->top, "name (2).bin"));
    CHECK(b->state_path && strstr(b->state_path, "name (2).bin.ulbt"));
    size_t n;
    char *now = read_file(fst, &n);
    CHECK(now && n == strlen(junk) && !memcmp(now, junk, n));      /* the other download's progress is untouched */
    free(now);
    tb_free(b);
    /* our own (damaged) state file under that name is not foreign */
    char *dir2 = tdir("m28b-own");
    char *own = path_join(dir2, "name.bin.ulbt");
    bt *c = tb_new(dir2);
    CHECK(tb_meta(c, info, il) == 0);
    char *t = xasprintf("ULBT1 %s\ngarbage", c->ihex);
    write_bytes(own, t, strlen(t));
    CHECK(setup_meta(c, NULL, NULL, 0) == 0 && !strcmp(c->top, "name.bin"));
    tb_free(c);
    free(t); free(own); free(dir2); free(fst); free(info); free(dir);
}

/* C3: a state file named just ".ulbt" */
typedef struct { ul_task *t; HANDLE ev; } watchdog_arg;
static unsigned __stdcall watchdog_main(void *arg)
{
    watchdog_arg *w = arg;
    if (WaitForSingleObject(w->ev, 10000) == WAIT_TIMEOUT) InterlockedExchange(&w->t->abort, 1);
    return 0;
}

static void test_state_named_ulbt(void)
{
    char *dir = tdir("c3");
    tfile f[1] = { { "", 3000 } };
    size_t il;
    char *info = mk_info("from-torrent.bin", 1024, f, 1, 0, &il);
    char *data = path_join(dir, "inner-name.bin");          /* complete, under the name saved in the state */
    put_data(data, 0, 3000, 0);
    uint8_t ih[20];
    char hex[41];
    sha1(info, il, ih);
    hex_encode(ih, 20, hex);
    sbuf s = { 0 };
    sb_printf(&s, "ULBT1 %s\nd4:info", hex);
    sb_putn(&s, info, il);
    sb_puts(&s, "4:name14:inner-name.bin6:pieces1:\xE0");
    sb_putc(&s, 'e');
    char *stp = path_join(dir, ".ulbt");
    write_bytes(stp, s.s, s.len);
    sbuf tor = { 0 };
    sb_puts(&tor, "d4:info");
    sb_putn(&tor, info, il);
    sb_putc(&tor, 'e');
    ul_config *cfg = xcalloc(1, sizeof *cfg);
    cfg_defaults(cfg);
    cfg->out_dir = xstrdup(dir);
    ul_task *t = quiet_task();
    http_session *hs = http_session_new(cfg, t);
    watchdog_arg wa = { t, CreateEventW(NULL, TRUE, FALSE, NULL) };
    HANDLE wd = (HANDLE)_beginthreadex(NULL, 0, watchdog_main, &wa, 0, NULL);
    log_clear();
    int rc = hs ? torrent_run(hs, cfg, "c3.torrent", (const unsigned char *)tor.s, tor.len) : -1;
    SetEvent(wa.ev);
    WaitForSingleObject(wd, INFINITE);
    CloseHandle(wd);
    CloseHandle(wa.ev);
    CHECK(rc == 0 && log_has("Already downloaded"));
    CHECK(t->name && !strcmp(t->name, "inner-name.bin"));
    if (hs) http_session_free(hs);
    task_free(t);
    cfg_free(cfg);
    sb_free(&tor); sb_free(&s);
    free(stp); free(data); free(info); free(dir);
}

/* H14: a block of the wrong size for one of our requests */
static void test_wrong_length_block(void)
{
    char *dir = tdir("h14");
    tfile f[1] = { { "", 32768 * 10 } };
    size_t il;
    char *info = mk_info("h14.bin", 32768, f, 1, 0, &il);
    bt *b = tb_new(dir);
    CHECK(tb_meta(b, info, il) == 0);
    tb_tables(b);
    char err[300];
    CHECK(meta_layout(&b->m, dir, "h14.bin", err, sizeof err) == 0);
    peer *p = tp_add(b, 1), *q = tp_add(b, 2);
    tp_seed(b, p);
    tp_seed(b, q);
    fill_requests(b, p);
    CHECK(p->nreq == MIN_PIPE && !b->endgame);
    int piece = p->req[0].piece, begin = p->req[0].begin;
    uint8_t buf[BLOCK];
    block_data(piece, begin, BLOCK, 32768, buf);
    on_block(b, p, (uint32_t)piece, (uint32_t)begin, buf, BLOCK - 1);     /* one byte short */
    apiece *a = b->act[b->slot[piece]];
    int j = begin / BLOCK;
    CHECK(a->st[j] == 0 && a->dup[j] == 0);              /* free to be asked for again */
    CHECK(p->dead && strstr(p->why, "wrong size"));
    fill_requests(b, q);
    CHECK(has_req(q, piece, begin));                     /* and the next peer does */
    tb_free(b);
    free(info);
    free(dir);
}

/* M28 (3rd part): blocks we cancelled are not "data nobody asked for" */
static void test_cancelled_blocks(void)
{
    char *dir = tdir("m28c");
    tfile f[1] = { { "", 32768 * 10 } };
    size_t il;
    char *info = mk_info("m28c.bin", 32768, f, 1, 0, &il);
    bt *b = tb_new(dir);
    CHECK(tb_meta(b, info, il) == 0);
    tb_tables(b);
    char err[300];
    CHECK(meta_layout(&b->m, dir, "m28c.bin", err, sizeof err) == 0);
    peer *p = tp_add(b, 1);
    tp_seed(b, p);
    uint8_t buf[BLOCK];
    int late = 0;
    for (int round = 0; round < 2; round++) {           /* snubbed: cancelled, but already on the way */
        fill_requests(b, p);
        int n = p->nreq;
        preq r[MAX_PIPE];
        memcpy(r, p->req, sizeof r);
        release_requests(b, p, 1);
        for (int k = 0; k < n; k++) {
            block_data(r[k].piece, r[k].begin, r[k].len, 32768, buf);
            on_block(b, p, (uint32_t)r[k].piece, (uint32_t)r[k].begin, buf, (uint32_t)r[k].len);
            late++;
        }
    }
    CHECK(late == 20 && p->unrequested == 0 && !p->dead);
    CHECK(b->nhave == 10);                               /* and the late blocks were used */
    on_block(b, p, 0, 0, buf, BLOCK);                    /* this one nobody asked for */
    CHECK(p->unrequested == BLOCK);
    tb_free(b);
    /* endgame: the copy another peer was fetching too is cancelled there */
    tfile g[1] = { { "", 65536 } };
    char *info2 = mk_info("eg.bin", 32768, g, 1, 0, &il);
    bt *e = tb_new(dir);
    CHECK(tb_meta(e, info2, il) == 0);
    tb_tables(e);
    peer *p2 = tp_add(e, 1), *q2 = tp_add(e, 2);
    tp_seed(e, p2);
    tp_seed(e, q2);
    fill_requests(e, p2);
    fill_requests(e, q2);
    CHECK(p2->nreq == 4 && q2->nreq == 4 && e->endgame);
    preq x = p2->req[0];
    block_data(x.piece, x.begin, x.len, 32768, buf);
    on_block(e, p2, (uint32_t)x.piece, (uint32_t)x.begin, buf, (uint32_t)x.len);
    CHECK(!has_req(q2, x.piece, x.begin));
    on_block(e, q2, (uint32_t)x.piece, (uint32_t)x.begin, buf, (uint32_t)x.len);
    CHECK(q2->unrequested == 0);
    tb_free(e);
    free(info2);
    free(info);
    free(dir);
}

/* a ~22 KB info dictionary: two metadata pieces (piece hashes are fake) */
static char *big_info(size_t *len)
{
    sbuf ib = { 0 };
    sb_puts(&ib, "d6:lengthi1100e4:name6:md.bin12:piece lengthi1e6:pieces22000:");
    for (int i = 0; i < 22000; i++) sb_putc(&ib, (char)('a' + i % 26));
    sb_putc(&ib, 'e');
    *len = ib.len;
    return sb_steal(&ib);
}

/* a ut_metadata data message from p; total: NULL = the right one, "" = none, else bencoded */
static void md_send(bt *b, peer *p, int piece, const char *info, size_t il, int corrupt, const char *total)
{
    if (piece < 0) return;                       /* (nothing was asked: a check failed already) */
    size_t off = (size_t)piece * MD_PIECE, k = il - off < MD_PIECE ? il - off : MD_PIECE;
    sbuf m = { 0 };
    if (!total) sb_printf(&m, "d8:msg_typei1e5:piecei%de10:total_sizei%zuee", piece, il);
    else if (!*total) sb_printf(&m, "d8:msg_typei1e5:piecei%dee", piece);
    else sb_printf(&m, "d8:msg_typei1e5:piecei%de10:total_size%se", piece, total);
    size_t at = m.len;
    sb_putn(&m, info + off, k);
    if (corrupt) m.s[at + 7] ^= 0x55;
    on_metadata_msg(b, p, (const uint8_t *)m.s, m.len);
    sb_free(&m);
}

static peer *md_peer(bt *b, int n, size_t il)
{
    peer *p = tp_add(b, n);
    p->ut_md = 3;
    p->md_size = (int64_t)il;
    return p;
}

/* H15 (1st part), L5: metadata from peers */
static void test_metadata_from_peers(void)
{
    char *dir = tdir("h15");
    size_t il;
    char *info = big_info(&il);
    /* total_size: a negative one is rejected, a missing one tolerated (the size we went with decides) */
    bt *b = tb_new(dir);
    tb_magnet(b, info, il);
    peer *p = md_peer(b, 1, il);
    md_request(b, p);
    CHECK(b->md_n == 2 && p->md_piece >= 0);
    int i = p->md_piece;
    md_send(b, p, i, info, il, 0, "i-5e");
    CHECK(b->md_st && b->md_st[i] == 0 && p->md_piece == -1 && p->md_bad == 1);
    md_request(b, p);
    i = p->md_piece;
    md_send(b, p, i, info, il, 0, "");
    CHECK(b->md_st && b->md_st[i] == 2);
    tb_free(b);
    /* a piece nobody asked x for is ignored */
    b = tb_new(dir);
    tb_magnet(b, info, il);
    p = md_peer(b, 1, il);
    peer *x = md_peer(b, 2, il);
    md_request(b, p);
    int other = 1 - p->md_piece;
    md_send(b, x, other, info, il, 1, NULL);
    CHECK(b->md_st && b->md_st[other] == 0 && b->banned == 0);
    tb_free(b);
    /* a bad copy made of an honest peer's piece and a liar's: nobody banned, then one peer at a time */
    b = tb_new(dir);
    tb_magnet(b, info, il);
    p = md_peer(b, 1, il);
    x = md_peer(b, 2, il);
    peer *q = md_peer(b, 3, il);
    md_request(b, p);
    md_request(b, x);
    CHECK(p->md_piece >= 0 && x->md_piece >= 0 && p->md_piece != x->md_piece);
    log_clear();
    md_send(b, p, p->md_piece, info, il, 0, NULL);
    md_send(b, x, x->md_piece, info, il, 1, NULL);
    CHECK(log_has("does not match the info-hash") && b->banned == 0 && !p->dead && !x->dead && MD_SOLO(b));
    md_request(b, q);                                     /* q goes first, the others wait */
    md_request(b, p);
    md_request(b, x);
    CHECK(q->md_piece >= 0 && p->md_piece == -1 && x->md_piece == -1);
    md_send(b, q, q->md_piece, info, il, 0, NULL);        /* (asks q for the other piece itself) */
    CHECK(q->md_piece >= 0);
    md_send(b, q, q->md_piece, info, il, 0, NULL);
    CHECK(b->have_meta && log_has("metadata received") && b->banned == 0);
    tb_free(b);
    /* the liar alone: it is banned, the honest peer is not, and then takes over */
    bt *c = tb_new(dir);
    tb_magnet(c, info, il);
    MD_SOLO(c) = 1;
    peer *h = md_peer(c, 1, il), *l = md_peer(c, 2, il);
    md_request(c, l);
    md_request(c, h);
    CHECK(l->md_piece >= 0 && h->md_piece == -1);
    md_send(c, l, l->md_piece, info, il, 1, NULL);
    md_send(c, l, l->md_piece, info, il, 1, NULL);
    CHECK(c->banned == 1 && l->dead && !h->dead);
    md_request(c, h);
    CHECK(h->md_piece >= 0);
    tb_free(c);
    free(info);
    free(dir);
}

/* L5 (2nd part): metadata size vote - a peer with another size is acted on at once */
static void test_md_size_switch(void)
{
    size_t il;
    char *info = big_info(&il);
    bt *b = tb_new(".");
    tb_magnet(b, info, il);
    peer *p = md_peer(b, 1, il);
    double t = b->now;
    md_request(b, p);
    CHECK(b->md_size == (int64_t)il);
    b->now = t + 31;
    b->t_tick = t + 29;
    tick(b);
    CHECK(b->md_size == (int64_t)il);                     /* nobody claims another size: keep it */
    peer *q = md_peer(b, 2, il + 100);
    (void)q;
    b->now = t + 32.5;
    b->t_tick = t + 31;
    tick(b);
    CHECK(b->md_size == 0);                               /* one does now: start over with it */
    tb_free(b);
    free(info);
}

/* M27: the setup is paused right after the metadata arrived */
static void test_meta_pause_keeps_metadata(void)
{
    char *dir = tdir("m27");
    tfile f[70];
    char names[70][16];
    for (int i = 0; i < 70; i++) { snprintf(names[i], sizeof names[i], "f%02d.bin", i); f[i].path = names[i]; f[i].len = 100; }
    size_t il;
    char *info = mk_info("m27", 1024, f, 70, 1, &il);
    bt *b = tb_new(dir);
    tb_magnet(b, info, il);
    peer *p = md_peer(b, 1, il);
    md_request(b, p);
    CHECK(p->md_piece == 0 && b->md_n == 1);
    InterlockedExchange(&b->t->abort, 1);                /* paused while the files are being created */
    md_send(b, p, 0, info, il, 0, NULL);
    CHECK(b->have_meta == 1 && p->bf != NULL);
    size_t wl = p->wlen;
    md_request(b, p);
    CHECK(p->wlen == wl);                                 /* nothing asks for metadata again */
    on_have(b, p, 3);
    CHECK(p->nhave == 1);
    CHECK(b->state_path != NULL);
    if (b->state_path) {
        state_save(b);                                    /* what stopping does */
        bt_state st;
        CHECK(state_load(b->state_path, b->ih, &st) == 0 && st.info_len == il);
        state_clear(&st);
    }
    tb_free(b);
    free(info);
    free(dir);
}

/* M28 (1st part): many trackers do not hold up the DHT bootstrap */
static void test_dht_gets_helpers(void)
{
    static const char *local[4] = { "localhost:6881", "localhost:6882", "localhost:6883", "localhost:6884" };
    const char *saved[4];
    for (int i = 0; i < 4; i++) { saved[i] = dht_router_names[i]; dht_router_names[i] = local[i]; }
    bt *b = tb_new(".");
    b->dht_on = 1;
    for (int i = 0; i < 20; i++) {
        char u[64];
        snprintf(u, sizeof u, "udp://127.0.0.1:9/announce%d", i);
        tracker_add(b, u);
    }
    CHECK(b->ntr == 20);
    helpers_tick(b);
    CHECK(b->d.resolving == 4 && b->njobs == MAX_JOBS);
    tb_free(b);
    /* every helper busy: the lookup waits for a free one, not a minute */
    bt *c = tb_new(".");
    c->dht_on = 1;
    for (int i = 0; i < MAX_JOBS; i++) {
        job *j = xcalloc(1, sizeof *j);
        j->refs = 1;
        j->ti = -1;
        j->t = task_new("");
        c->jobs[c->njobs++] = j;
    }
    dht_tick(c);
    CHECK(c->d.resolving == 0 && c->d.resolve_next <= c->now);
    job_release(c->jobs[--c->njobs]);
    dht_tick(c);
    CHECK(c->d.resolving == 1);
    tb_free(c);
    for (int i = 0; i < 4; i++) dht_router_names[i] = saved[i];
}

/* L6: a DHT query that could not be sent leaves the node to ask again */
static void test_dht_unsent_query(void)
{
    bt *b = tb_new(".");
    b->dht_on = 1;                                        /* no socket: every sendto fails */
    b->d.resolved = 1;
    for (int i = 0; i < 3; i++) {
        uint8_t id[20], ip[4] = { 1, 2, 3, (uint8_t)(i + 1) };
        memset(id, i + 1, 20);
        baddr a;
        addr_v4(&a, ip, 6881);
        dht_add(b, id, &a);
    }
    b->d.searching = 1;
    b->d.t_start = b->now;
    dht_tick(b);
    CHECK(b->d.nn == 3 && b->d.npend == 0);
    for (int i = 0; i < b->d.nn; i++) CHECK(b->d.n[i].st == 0);
    tb_free(b);
}

/* M25, L1: UDP tracker replies */
typedef struct { SOCKET s; int fam, mode, npeers, connects; volatile LONG stop; } fake_tracker;

static unsigned __stdcall fake_tracker_main(void *arg)
{
    fake_tracker *u = arg;
    uint8_t buf[2048];
    while (!u->stop) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(u->s, &rd);
        struct timeval tv = { 0, 100000 };
        if (select(0, &rd, NULL, NULL, &tv) <= 0) continue;
        struct sockaddr_storage ss;
        int sl = sizeof ss;
        int n = recvfrom(u->s, (char *)buf, sizeof buf, 0, (struct sockaddr *)&ss, &sl);
        if (n < 16) continue;
        uint32_t action = rd32(buf + 8), tid = rd32(buf + 12);
        if (action == 0) {
            if (++u->connects > 1 && u->mode == 1) continue;  /* slow: answers the first connect only, late */
            if (u->mode == 1) Sleep(3500);
            uint8_t r[16];
            wr32(r, 0);
            wr32(r + 4, tid);
            memset(r + 8, 0x42, 8);
            sendto(u->s, (const char *)r, 16, 0, (struct sockaddr *)&ss, sl);
        } else if (action == 1 && n >= 98) {
            int step = u->fam == 6 ? 18 : 6;
            size_t len = 20 + (size_t)step * (size_t)u->npeers;
            uint8_t *r = xcalloc(len, 1);
            wr32(r, 1);
            wr32(r + 4, tid);
            wr32(r + 8, 1800);
            wr32(r + 12, 0xFFFFFFFFu);
            wr32(r + 16, 0x80000005u);
            for (int k = 0; k < u->npeers; k++) {
                uint8_t *e = r + 20 + (size_t)k * step;
                if (step == 6) { e[0] = 10; e[1] = 7; e[2] = (uint8_t)(k >> 8); e[3] = (uint8_t)k; wr16(e + 4, (uint32_t)(1000 + k)); }
                else { e[0] = 0x20; e[1] = 0x01; e[2] = 0x0d; e[3] = 0xb8; e[14] = (uint8_t)(k >> 8); e[15] = (uint8_t)(k + 1); wr16(e + 16, (uint32_t)(1000 + k)); }
            }
            sendto(u->s, (const char *)r, (int)len, 0, (struct sockaddr *)&ss, sl);
            free(r);
        }
    }
    return 0;
}

static job *udp_try(int fam, int mode, int npeers)
{
    fake_tracker u;
    memset(&u, 0, sizeof u);
    u.fam = fam;
    u.mode = mode;
    u.npeers = npeers;
    u.s = socket(fam == 6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (u.s == INVALID_SOCKET) return NULL;
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof ss);
    int sl;
    if (fam == 6) {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)&ss;
        a->sin6_family = AF_INET6;
        a->sin6_addr.s6_addr[15] = 1;
        sl = sizeof *a;
    } else {
        struct sockaddr_in *a = (struct sockaddr_in *)&ss;
        a->sin_family = AF_INET;
        a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sl = sizeof *a;
    }
    if (bind(u.s, (struct sockaddr *)&ss, sl) != 0) { closesocket(u.s); return NULL; }
    getsockname(u.s, (struct sockaddr *)&ss, &sl);
    int port = ntohs(fam == 6 ? ((struct sockaddr_in6 *)&ss)->sin6_port : ((struct sockaddr_in *)&ss)->sin_port);
    HANDLE th = (HANDLE)_beginthreadex(NULL, 0, fake_tracker_main, &u, 0, NULL);
    job *j = xcalloc(1, sizeof *j);
    j->refs = 1;
    j->ti = -1;
    j->t = task_new("");
    j->event = 2;
    j->url = xasprintf(fam == 6 ? "udp://[::1]:%d/announce" : "udp://127.0.0.1:%d/announce", port);
    double t0 = now_sec();
    udp_announce(j);
    printf("  udp tracker (IPv%d, %s, %d peers): ok=%d peers=%d in %.1fs %s\n", fam, mode ? "slow" : "fast", npeers,
           j->ok, j->naddr, now_sec() - t0, j->err);
    InterlockedExchange(&u.stop, 1);
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    closesocket(u.s);
    return j;
}

static void test_udp_tracker(void)
{
    net_init();
    job *j = udp_try(6, 0, 100);              /* 20 + 100 * 18 = 1820 bytes */
    if (j) {
        CHECK(j->ok && j->naddr == 100);
        CHECK(j->leechers == 0 && j->seeders == 0);      /* 0xFFFFFFFF / 0x80000005: not counts */
        job_release(j);
    } else {
        printf("  (no IPv6 loopback here: IPv6 part skipped)\n");
    }
    j = udp_try(4, 0, 300);                   /* 20 + 300 * 6 = 1820 bytes */
    CHECK(j && j->ok && j->naddr == 300);
    if (j) job_release(j);
    j = udp_try(4, 1, 5);                     /* the answer to the first try comes during the second */
    CHECK(j && j->ok && j->naddr == 5);
    if (j) job_release(j);
}

typedef struct { const char *name; void (*fn)(void); } utest;
static const utest g_tests[] = {
    { "bencode", test_bencode }, { "magnet", test_magnet }, { "paths", test_paths }, { "mapping", test_mapping },
    { "magnet_audit", test_magnet_audit },              /* C2, L3 */
    { "utf8_fallback", test_utf8_fallback },            /* L2 */
    { "layout_names", test_layout_many_equal_names },   /* H15 */
    { "readonly", test_readonly_files },                /* M26 */
    { "exact_sizes", test_complete_exact_sizes },       /* M28 */
    { "foreign_state", test_foreign_state },            /* M28 */
    { "state_dot_ulbt", test_state_named_ulbt },        /* C3 */
    { "wrong_length", test_wrong_length_block },        /* H14 */
    { "cancelled", test_cancelled_blocks },             /* M28 */
    { "metadata", test_metadata_from_peers },           /* H15, L5 */
    { "md_size", test_md_size_switch },                 /* L5 */
    { "meta_pause", test_meta_pause_keeps_metadata },   /* M27 */
    { "dht_helpers", test_dht_gets_helpers },           /* M28 */
    { "dht_unsent", test_dht_unsent_query },            /* L6 */
    { "udp_tracker", test_udp_tracker },                /* M25, L1 */
};

int main(int argc, char **argv)
{
    snprintf(g_base, sizeof g_base, "%s\\ulbt-unit-%lu", getenv("TEMP") ? getenv("TEMP") : ".", GetCurrentProcessId());
    dir_create(g_base);
    for (size_t i = 0; i < sizeof g_tests / sizeof *g_tests; i++) {
        int want = argc < 2;
        for (int k = 1; k < argc; k++) if (!strcmp(argv[k], g_tests[i].name)) want = 1;
        if (!want) continue;
        int f0 = fails;
        printf("%s\n", g_tests[i].name);
        fflush(stdout);
        g_tests[i].fn();
        if (fails != f0) printf("  -> %d failure(s) in %s\n", fails - f0, g_tests[i].name);
        fflush(stdout);
    }
    wchar_t *w = u8_to_w(g_base);
    rm_tree(w);
    free(w);
    log_clear();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
