/* Ask Windows which names are DOS devices (GetFullPathNameW oracle).
   Build: gcc -O1 devname.c -o devname.exe ; run: devname.exe */
#include <windows.h>
#include <stdio.h>
static void t(const wchar_t *n){
    wchar_t in[512], out[512];
    swprintf(in, 512, L"C:\\tmp\\%ls", n);
    DWORD r = GetFullPathNameW(in, 512, out, NULL);
    char o[1024]; WideCharToMultiByte(CP_UTF8,0,out,-1,o,sizeof o,NULL,NULL);
    char i8[1024]; WideCharToMultiByte(CP_UTF8,0,n,-1,i8,sizeof i8,NULL,NULL);
    printf("%-28s -> %s %s\n", i8, r?o:"(fail)", (r && wcsncmp(out,L"\\\\.\\",4)==0)?"  <== DEVICE":"");
}
int main(void){
    SetConsoleOutputCP(65001);
    const wchar_t *names[] = { L"CON", L"con.txt", L"CON .txt", L"NUL .bin", L"NUL .bin.ulpart",
      L"nul.tar.gz", L"COM1.x", L"COM0", L"LPT0.txt", L"COM\x00B9", L"COM\x00B9.mp4", L"LPT\x00B2.txt",
      L"CONIN$", L"CONOUT$", L"CONOUT$.txt", L"AUX .  .x", L"_CON.txt", L"CON  ", L"NUL:x", L"aux.ulstate.tmp",
      L"NUL\x3000.txt", L"CLOCK$", L"COM1 ", L"NUL\t.x", NULL };
    for (int i = 0; names[i]; i++) t(names[i]);
    return 0;
}
