// Atmosphère allows three csrng sessions in the whole system, and libnx's curl
// keeps one open for as long as it's loaded. HOME holding one (and the Discord
// overlay another) left too few for the rest of the console. These stand in for
// libnx's csrng: the kernel's entropy (randomGet) needs no session.
#include <switch.h>

extern "C" {
Result csrngInitialize(void) { return 0; }
void csrngExit(void) {}
Service* csrngGetServiceSession(void) {
    static Service none{};
    return &none;
}
Result csrngGetRandomBytes(void* out, size_t size) {
    randomGet(out, size);
    return 0;
}
}
