/* Test helper: stands in for U-loader-Setup-<version>.exe in the update
   test. It installs nothing; it writes the command line it was started
   with to the file named by FAKE_SETUP_LOG and exits.
   Build: gcc -O1 fakesetup.c -o fakesetup.exe */
#include <windows.h>
#include <stdio.h>

int main(void)
{
    char log[MAX_PATH];
    if (GetEnvironmentVariableA("FAKE_SETUP_LOG", log, sizeof log)) {
        FILE *f = fopen(log, "w");
        if (f) { fputs(GetCommandLineA(), f); fputc('\n', f); fclose(f); }
    }
    return 0;
}
