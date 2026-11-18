/* Link-time free() poisoning for use-after-free detection (test helper).
   Every free()d block is overwritten with 0xDD and never reused, so a later
   read through a dangling pointer sees garbage instead of stale-but-valid data.
   Build:
     gcc -O0 -g -std=gnu11 ../../review-v1/src/*.c poison_free.c -o uloader_poison.exe -static \
         -Wl,--wrap=free -lwinhttp -lbcrypt -lshell32 -lole32 */
#include <malloc.h>
#include <string.h>
#include <windows.h>
void __real_free(void *p);
void __wrap_free(void *p)
{
    if (!p) return;
    size_t n = _msize(p);
    memset(p, 0xDD, n);
    /* leak on purpose: keep the poisoned block out of circulation */
}
