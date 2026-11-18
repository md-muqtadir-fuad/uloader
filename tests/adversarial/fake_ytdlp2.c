/* Configurable yt-dlp stand-in (test helper).
 *   - always creates "<its own exe path>.ran" (proves it was executed)
 *   - FAKE_YTDLP_LOG: write the raw command line, then each argv[] as parsed by the CRT, one per line
 *   - FAKE_YTDLP_OUT: file whose bytes are printed verbatim to stdout
 *   - FAKE_YTDLP_RC : exit code (default 0)
 * Build: gcc -O1 -municode fake_ytdlp2.c -o yt-dlp.exe
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <io.h>

static void put_u8(FILE *f, const wchar_t *w)
{
    char buf[65536];
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof buf, NULL, NULL);
    if (n > 0) fwrite(buf, 1, (size_t)n - 1, f);
}

int wmain(int argc, wchar_t **argv)
{
    wchar_t me[MAX_PATH + 8];
    DWORD n = GetModuleFileNameW(NULL, me, MAX_PATH);
    wcscpy(me + n, L".ran");
    FILE *m = _wfopen(me, L"ab");
    if (m) { fputs("ran\n", m); fclose(m); }

    wchar_t logp[1024];
    if (GetEnvironmentVariableW(L"FAKE_YTDLP_LOG", logp, 1024)) {
        FILE *f = _wfopen(logp, L"wb");
        if (f) {
            put_u8(f, GetCommandLineW());
            fputc('\n', f);
            for (int i = 0; i < argc; i++) { fprintf(f, "ARGV[%d]=", i); put_u8(f, argv[i]); fputc('\n', f); }
            fclose(f);
        }
    }
    wchar_t outp[1024];
    if (GetEnvironmentVariableW(L"FAKE_YTDLP_OUT", outp, 1024)) {
        FILE *f = _wfopen(outp, L"rb");
        if (f) {
            char b[8192];
            size_t r;
            _setmode(_fileno(stdout), 0x8000 /* _O_BINARY */);
            while ((r = fread(b, 1, sizeof b, f)) > 0) fwrite(b, 1, r, stdout);
            fclose(f);
        }
    }
    char rc[16];
    if (GetEnvironmentVariableA("FAKE_YTDLP_RC", rc, sizeof rc)) return atoi(rc);
    return 0;
}
