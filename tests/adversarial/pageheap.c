/* Poor man's page heap for MinGW (test helper, no admin rights needed).
   Wraps malloc/calloc/realloc/free of U-loader's own objects:
     - every block gets its own VirtualAlloc region, right-aligned against a
       PAGE_NOACCESS guard page (catches heap overruns),
     - free() decommits the block but keeps the address range reserved, so any
       later use of a dangling pointer faults immediately (catches use-after-free).
   Build:
     gcc -O0 -g -std=gnu11 ../../review-v1/src/*.c pageheap.c -o uloader_pageheap.exe -static \
       -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free -lwinhttp -lbcrypt -lshell32 -lole32 */
#include <windows.h>
#include <string.h>
#include <stdint.h>

typedef struct { size_t size; char *base; size_t span; uint64_t magic; } hdr_t;
#define MAGIC 0x50414745484541ULL

void *__wrap_malloc(size_t n)
{
    size_t user = (n + 15) & ~(size_t)15;
    if (!user) user = 16;
    size_t need = user + sizeof(hdr_t);
    size_t pages = (need + 4095) / 4096;
    size_t span = (pages + 1) * 4096;
    char *base = VirtualAlloc(NULL, span, MEM_RESERVE, PAGE_NOACCESS);
    if (!base) return NULL;
    if (!VirtualAlloc(base, pages * 4096, MEM_COMMIT, PAGE_READWRITE)) return NULL;
    char *p = base + pages * 4096 - user;
    hdr_t *h = (hdr_t *)(p - sizeof(hdr_t));
    h->size = n;
    h->base = base;
    h->span = span;
    h->magic = MAGIC;
    return p;
}

void *__wrap_calloc(size_t a, size_t b)
{
    return __wrap_malloc(a * b);          /* VirtualAlloc memory is already zeroed */
}

void __wrap_free(void *p)
{
    if (!p) return;
    hdr_t *h = (hdr_t *)((char *)p - sizeof(hdr_t));
    if (h->magic != MAGIC) { RaiseException(0xE0FF0001, 0, 0, NULL); return; }  /* bad / double free */
    char *base = h->base;
    size_t span = h->span;
    h->magic = 0;
    VirtualFree(base, span - 4096, MEM_DECOMMIT);   /* stays reserved: never reused */
}

/* On a crash, append the faulting PC and a raw stack walk to %PAGEHEAP_LOG%
   (resolve with: addr2line -f -e uloader_pageheap.exe <addr>...) */
static LONG WINAPI ph_veh(PEXCEPTION_POINTERS ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && (code & 0xFFFF0000) != 0xE0FF0000) return EXCEPTION_CONTINUE_SEARCH;
    char path[MAX_PATH];
    if (!GetEnvironmentVariableA("PAGEHEAP_LOG", path, sizeof path)) return EXCEPTION_CONTINUE_SEARCH;
    HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return EXCEPTION_CONTINUE_SEARCH;
    char line[256];
    DWORD w;
    HMODULE mod = GetModuleHandleA(NULL);
    int n = wsprintfA(line, "code=%08lx tid=%lu pc=%p module_base=%p fault_addr=%p\r\n", code, GetCurrentThreadId(),
                      ep->ExceptionRecord->ExceptionAddress, (void *)mod,
                      ep->ExceptionRecord->NumberParameters > 1 ? (void *)ep->ExceptionRecord->ExceptionInformation[1] : NULL);
    WriteFile(f, line, (DWORD)n, &w, NULL);
    /* scan the stack for return addresses inside our image */
    uintptr_t *sp = (uintptr_t *)ep->ContextRecord->Rsp;
    for (int i = 0; i < 4096; i++) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(sp + i, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) break;
        uintptr_t v = sp[i];
        if (v > (uintptr_t)mod && v < (uintptr_t)mod + 0x200000) {
            n = wsprintfA(line, "  stack %p\r\n", (void *)v);
            WriteFile(f, line, (DWORD)n, &w, NULL);
        }
    }
    CloseHandle(f);
    return EXCEPTION_CONTINUE_SEARCH;
}

__attribute__((constructor)) static void ph_init(void) { AddVectoredExceptionHandler(1, ph_veh); }

void *__wrap_realloc(void *p, size_t n)
{
    if (!p) return __wrap_malloc(n);
    hdr_t *h = (hdr_t *)((char *)p - sizeof(hdr_t));
    if (h->magic != MAGIC) { RaiseException(0xE0FF0002, 0, 0, NULL); return NULL; }
    void *q = __wrap_malloc(n);
    if (!q) return NULL;
    memcpy(q, p, h->size < n ? h->size : n);
    __wrap_free(p);
    return q;
}
