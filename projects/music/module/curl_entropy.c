// libcurl's libnx backend keeps a csrng session between global init and exit.
// Acquire it only while fetching random bytes, so another app can use the slot.
#include <switch.h>

Result __real_csrngInitialize(void);
void __real_csrngExit(void);
Result __real_csrngGetRandomBytes(void* out, size_t size);

Result __wrap_csrngInitialize(void) { return 0; }
void __wrap_csrngExit(void) {}

Result __wrap_csrngGetRandomBytes(void* out, size_t size) {
    Result rc = __real_csrngInitialize();
    if (R_FAILED(rc)) return rc;
    rc = __real_csrngGetRandomBytes(out, size);
    __real_csrngExit();
    return rc;
}
