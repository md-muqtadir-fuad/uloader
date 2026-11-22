/* Unit tests for the core helpers (util.c, task.c helpers).
   Build:
     gcc -O0 -g -std=gnu11 -Isrc tests/unit_core.c src/util.c src/http.c src/ftp.c src/engine.c src/hls.c
         src/extract.c src/torrent.c src/task.c -o unit_core.exe -lwinhttp -lbcrypt -lws2_32 -lshell32
         -lole32 -loleaut32 -luuid -ladvapi32
   Run: unit_core.exe   (exit code 0 = all passed) */
#include "uloader.h"
#include <process.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* a "download" taking 200 KB from the shared speed limiter in 16 KB reads */
static unsigned __stdcall limiter_user(void *arg)
{
    (void)arg;
    for (int i = 0; i < 200 / 16; i++) speed_limit_take(NULL, 16 * 1024);
    return 0;
}

static void check_resolve(const char *base, const char *ref, const char *want)
{
    char *got = url_resolve(base, ref);
    CHECK(!strcmp(got, want), "url_resolve(%s, %s) = %s, want %s", base, ref, got, want);
    free(got);
}

int main(void)
{
    /* str_trim: in place, same pointer (callers free what they passed) */
    char *s = xstrdup("  \t hello world \r\n");
    char *t = str_trim(s);
    CHECK(t == s && !strcmp(s, "hello world"), "str_trim -> [%s]", s);
    free(s);
    s = xstrdup("   ");
    CHECK(str_trim(s) == s && !*s, "str_trim of blanks");
    free(s);

    /* RFC 3986 5.4 examples */
    const char *B = "http://a/b/c/d;p?q";
    check_resolve(B, "g", "http://a/b/c/g");
    check_resolve(B, "./g", "http://a/b/c/g");
    check_resolve(B, "g/", "http://a/b/c/g/");
    check_resolve(B, "/g", "http://a/g");
    check_resolve(B, "//g", "http://g");
    check_resolve(B, "?y", "http://a/b/c/d;p?y");
    check_resolve(B, "g?y", "http://a/b/c/g?y");
    check_resolve(B, ";x", "http://a/b/c/;x");
    check_resolve(B, ".", "http://a/b/c/");
    check_resolve(B, "./", "http://a/b/c/");
    check_resolve(B, "..", "http://a/b/");
    check_resolve(B, "../", "http://a/b/");
    check_resolve(B, "../g", "http://a/b/g");
    check_resolve(B, "../..", "http://a/");
    check_resolve(B, "../../", "http://a/");
    check_resolve(B, "../../g", "http://a/g");
    check_resolve(B, "../../../g", "http://a/g");
    check_resolve(B, "/./g", "http://a/g");
    check_resolve(B, "/../g", "http://a/g");
    check_resolve(B, "g.", "http://a/b/c/g.");
    check_resolve(B, ".g", "http://a/b/c/.g");
    check_resolve(B, "g..", "http://a/b/c/g..");
    check_resolve(B, "..g", "http://a/b/c/..g");
    check_resolve(B, "./../g", "http://a/b/g");
    check_resolve(B, "./g/.", "http://a/b/c/g/");
    check_resolve(B, "g/./h", "http://a/b/c/g/h");
    check_resolve(B, "g/../h", "http://a/b/c/h");
    check_resolve(B, "g;x=1/./y", "http://a/b/c/g;x=1/y");
    check_resolve(B, "g;x=1/../y", "http://a/b/c/y");
    check_resolve(B, "https://x/y", "https://x/y");
    /* empty path segments survive (signed CDN / S3 links) */
    check_resolve("https://cdn.example/a/b//c/index.m3u8?sig=1", "seg1.ts", "https://cdn.example/a/b//c/seg1.ts");
    check_resolve("https://cdn.example/a/b//c/index.m3u8", "../x//y.ts", "https://cdn.example/a/b//x//y.ts");

    /* strict UTF-8 */
    CHECK(utf8_valid((const unsigned char *)"\xE0\xA6\xAC", 3), "valid Bengali letter");
    CHECK(!utf8_valid((const unsigned char *)"\xC0\xAF", 2), "overlong '/' must be invalid");
    CHECK(!utf8_valid((const unsigned char *)"\xED\xA0\x80", 3), "UTF-16 surrogate must be invalid");
    CHECK(!utf8_valid((const unsigned char *)"\xF4\x90\x80\x80", 4), "above U+10FFFF must be invalid");

    /* sizes */
    CHECK(parse_size("500K") == 512000, "500K");
    CHECK(parse_size("2M") == 2097152, "2M");
    CHECK(parse_size("1.5MB/s") == 1572864, "1.5MB/s");
    CHECK(parse_size("nan") == -1 && parse_size("inf") == -1 && parse_size("-1") == -1, "nan/inf/-1");
    CHECK(parse_size("10Mfoo") == -1, "trailing junk");

    /* cookie domains */
    CHECK(host_matches("www.example.com", ".example.com"), "subdomain match");
    CHECK(host_matches("example.com", "example.com"), "exact match");
    CHECK(!host_matches("evil-example.com", "example.com"), "evil-example.com");
    CHECK(!host_matches("a.com.", "."), "domain '.'");
    CHECK(!host_matches("x.com", ".com."), "domain '.com.'");
    CHECK(!host_matches("x.com", "..com"), "domain '..com'");

    /* origins: scheme + host + port */
    char *o1 = url_origin("https://Example.com/x"), *o2 = url_origin("http://example.com/x"),
         *o3 = url_origin("https://example.com:8443/x"), *o4 = url_origin("https://example.com:443/y");
    CHECK(!strcmp(o1, "https://example.com:443"), "origin %s", o1);
    CHECK(strcmp(o1, o2) != 0, "https vs http must differ");
    CHECK(strcmp(o1, o3) != 0, "another port must differ");
    CHECK(!strcmp(o1, o4), "explicit default port is the same origin");
    free(o1); free(o2); free(o3); free(o4);

    /* -b cookie: only to its own host, never when told not to (foreign) */
    ul_config cfg;
    cfg_defaults(&cfg);
    cfg.cookie = "sid=1";
    cfg.cookie_host = "example.com";
    char *c1 = cookie_header_for(&cfg, "https://example.com/a", 1);
    char *c2 = cookie_header_for(&cfg, "https://example.com/a", 0);
    char *c3 = cookie_header_for(&cfg, "https://other.com/a", 1);
    CHECK(c1 && !strcmp(c1, "sid=1"), "raw cookie to its host");
    CHECK(!c2, "raw cookie withheld for a foreign hop");
    CHECK(!c3, "raw cookie never to another host");
    free(c1); free(c2); free(c3);

    /* content types: exact type, parameters ignored */
    CHECK(!strcmp(ext_for_content_type("video/mp4; codecs=avc1"), ".mp4"), "video/mp4 params");
    CHECK(!strcmp(ext_for_content_type("audio/mpegurl"), ""), "audio/mpegurl is not .mp3");

    /* file names */
    char *n1 = sanitize_filename("..\\..\\x:y?.txt");
    CHECK(!strchr(n1, '\\') && !strchr(n1, ':') && !strchr(n1, '?'), "sanitize %s", n1);
    free(n1);
    char *n2 = sanitize_filename("COM\xC2\xB9.txt");
    CHECK(n2[0] == '_', "COM-superscript-1 is a device name: %s", n2);
    free(n2);

    /* versions (#11) */
    CHECK(version_newer("1.10.0", "1.9.9") && version_newer("v1.4", "1.3.9") && version_newer("2.0.0-beta", "1.9"),
          "newer versions");
    CHECK(!version_newer("1.3.0", "1.3.0") && !version_newer("1.3.0", "1.3.0.1") && !version_newer("1.2.9", "1.3"),
          "not newer");

    /* categories (#1) */
    CHECK(category_of("Movie.MKV") == CAT_VIDEO, "mkv is a video");
    CHECK(category_of("song.mp3") == CAT_MUSIC && category_of("a.pdf") == CAT_DOCUMENTS, "mp3 / pdf");
    CHECK(category_of("x.7z") == CAT_COMPRESSED && category_of("setup.exe") == CAT_PROGRAMS, "7z / exe");
    CHECK(category_of("p.webp") == CAT_PICTURES, "webp is a picture");
    CHECK(category_of("data.xyz") == -1 && category_of("README") == -1 && category_of("x.") == -1, "no category");
    category_set_exts(CAT_VIDEO, "MKV, .foo;bar");
    CHECK(category_of("a.foo") == CAT_VIDEO && category_of("a.mp4") == -1, "a changed type list is used");
    char *ce = category_exts(CAT_VIDEO);
    CHECK(!strcmp(ce, "mkv foo bar"), "normalized list: [%s]", ce);
    free(ce);
    category_set_exts(CAT_VIDEO, NULL);
    CHECK(category_of("a.mp4") == CAT_VIDEO, "back to the defaults");
    ul_config cc;
    cfg_defaults(&cc);
    cc.out_dir = "D:\\Dl";
    char *f1 = category_folder(&cc, "a.mp4");
    cc.by_category = 1;
    char *f2 = category_folder(&cc, "a.mp4"), *f3 = category_folder(&cc, "a.xyz");
    CHECK(!strcmp(f1, "D:\\Dl") && !strcmp(f2, "D:\\Dl\\Video") && !strcmp(f3, "D:\\Dl"), "%s | %s | %s", f1, f2, f3);
    free(f1); free(f2); free(f3);

    /* speed limiter (#2): 400 KB at 200 KB/s ~ 1.75 s (0.25 s of burst) */
    speed_limit_set(200 * 1024);
    double t0 = now_sec();
    for (int i = 0; i < 400 / 16; i++) speed_limit_take(NULL, 16 * 1024);
    double el = now_sec() - t0;
    CHECK(el > 1.4 && el < 2.6, "400 KB at 200 KB/s took %.2f s", el);
    /* two downloads at once share the budget: 2 x 200 KB still ~1.75 s */
    speed_limit_set(200 * 1024);
    t0 = now_sec();
    HANDLE th[2];
    for (int i = 0; i < 2; i++) th[i] = (HANDLE)_beginthreadex(NULL, 0, limiter_user, NULL, 0, NULL);
    WaitForMultipleObjects(2, th, TRUE, 10000);
    for (int i = 0; i < 2; i++) CloseHandle(th[i]);
    el = now_sec() - t0;
    CHECK(el > 1.4 && el < 2.6, "two downloads, 400 KB together, took %.2f s", el);
    /* switched off: no waiting at all */
    speed_limit_set(0);
    t0 = now_sec();
    for (int i = 0; i < 1000; i++) speed_limit_take(NULL, 1 << 20);
    CHECK(now_sec() - t0 < 0.1, "limiter off still waited");

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
