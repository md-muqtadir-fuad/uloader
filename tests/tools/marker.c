/* Test helper: a program that must never be run. If it is, it leaves
   "<its own path>.ran" behind as evidence. */
#include <windows.h>
#include <stdio.h>
int main(void)
{
    char path[MAX_PATH + 8];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (n) {
        strcpy(path + n, ".ran");
        FILE *f = fopen(path, "w");
        if (f) { fputs("executed\n", f); fclose(f); }
    }
    return 0;
}
