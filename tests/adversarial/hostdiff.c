/* Compare U-loader's url_host() (used to pick cookies) with the host WinHTTP
   actually connects to (WinHttpCrackUrl). Any difference can leak cookies.
   Build: gcc -O1 -std=gnu11 -DSRC2 hostdiff.c -o hostdiff.exe -static -lwinhttp -lbcrypt -lws2_32 -lshell32 -lole32 -loleaut32 -luuid -ladvapi32
   (includes ../../review-v2/src/util.c) */
#include "../../review-v2/src/util.c"
/* stubs for symbols util.c references from other modules */
int task_aborted(const ul_task *t) { (void)t; return 0; }
void task_sleep(const ul_task *t, DWORD ms) { (void)t; Sleep(ms); }
void tlog(ul_task *t, int kind, const char *fmt, ...) { (void)t; (void)kind; (void)fmt; }

static void crack(const char *u, char *out, size_t n, int *port)
{
    wchar_t *w = u8_to_w(u);
    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.dwSchemeLength = uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = (DWORD)-1;
    uc.dwUserNameLength = uc.dwPasswordLength = (DWORD)-1;
    if (!WinHttpCrackUrl(w, 0, 0, &uc) || !uc.dwHostNameLength) { snprintf(out, n, "<crack failed %lu>", GetLastError()); *port = 0; free(w); return; }
    wchar_t h[512];
    memcpy(h, uc.lpszHostName, uc.dwHostNameLength * sizeof(wchar_t));
    h[uc.dwHostNameLength] = 0;
    WideCharToMultiByte(CP_UTF8, 0, h, -1, out, (int)n, NULL, NULL);
    for (char *p = out; *p; p++) *p = (char)tolower((unsigned char)*p);
    *port = uc.nPort;
    free(w);
}

int main(int argc, char **argv)
{
    const char *urls[] = {
        "http://evil.com\\@victim.com/x",
        "http://evil.com\\\\@victim.com/x",
        "http://victim.com@evil.com/x",
        "http://evil.com#@victim.com/x",
        "http://evil.com?@victim.com/x",
        "http://evil.com;@victim.com/x",
        "http://a@b@victim.com/x",
        "http://evil.com%2F@victim.com/x",
        "http://evil.com:80@victim.com/x",
        "http://evil.com:80\\@victim.com/x",
        "http://user:p@ss@evil.com/x",
        "http://victim.com./x",
        "http://VICTIM.COM/x",
        "http://victim.com\t/x",
        "http://evil.com\\victim.com/x",
        "http://[::1]@victim.com/x",
        "http://evil.com\x01@victim.com/x",
        "http://evil.com @victim.com/x",
        "http:\\\\evil.com\\@victim.com/x",
        "HTTP://evil.com\\@victim.com",
        "http://evil.com%40victim.com/x",
        "http://victim.com%2e/x",
        "http://xn--victim-ex.com/x",
        "http://victim.com:/x",
        NULL };
    int diffs = 0;
    for (int i = 0; urls[i]; i++) {
        char *mine = url_host(urls[i]);
        char real[512];
        int port;
        crack(urls[i], real, sizeof real, &port);
        int d = strcmp(mine, real) != 0;
        diffs += d;
        printf("%-4s %-40s cookies-for=%-22s winhttp-connects-to=%s:%d\n", d ? "DIFF" : "ok", urls[i], mine, real, port);
        free(mine);
    }
    printf("%d differences\n", diffs);
    return diffs ? 1 : 0;
}
