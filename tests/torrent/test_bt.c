/* Unit tests for src/torrent.c (bencode, magnets, path safety, piece->file
   mapping). Build: gcc -I<src> test_bt.c <core objs> ... (see run_unit.sh) */
#include "torrent.c"

static int fails, checks;
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
    snprintf(dir, sizeof dir, "%s\\ulbt-unit-io", getenv("TEMP") ? getenv("TEMP") : ".");
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

int main(void)
{
    printf("bencode\n");  test_bencode();
    printf("magnet\n");   test_magnet();
    printf("paths\n");    test_paths();
    printf("mapping\n");  test_mapping();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
