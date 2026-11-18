/* Send a real Ctrl+C to the console of process <pid> (test helper). */
#include <windows.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    FreeConsole();
    if (!AttachConsole((DWORD)atoi(argv[1]))) return 3;
    SetConsoleCtrlHandler(NULL, TRUE);          /* do not kill ourselves */
    if (!GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0)) return 4;
    Sleep(200);
    return 0;
}
