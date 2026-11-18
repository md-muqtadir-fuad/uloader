/* Send Ctrl+C (default) or Ctrl+Break ("break") to the console of process <pid>.
   Build: gcc -O1 sendctrl.c -o sendctrl.exe ; run: sendctrl.exe <pid> [break] */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    DWORD ev = (argc > 2 && !strcmp(argv[2], "break")) ? CTRL_BREAK_EVENT : CTRL_C_EVENT;
    FreeConsole();
    if (!AttachConsole((DWORD)atoi(argv[1]))) return 3;
    SetConsoleCtrlHandler(NULL, TRUE);
    if (!GenerateConsoleCtrlEvent(ev, 0)) return 4;
    Sleep(200);
    return 0;
}
