#pragma once
#include <switch.h>
#include <cstring>

namespace switchu::daemon {

// Call between threadCreate and threadStart. A stack libnx takes from malloc
// holds whatever the heap had there, so the memory report (daemon_mem.txt),
// which counts the untouched zero words at the bottom, would see it as full.
// [stack_mirror, stack_mirror + stack_sz) is the stack alone: libnx puts the
// entry args, TLS and reent above it, so clearing it before the thread runs
// is safe.
inline void clearThreadStack(Thread* t) {
    if (t->stack_mirror && t->stack_sz)
        std::memset(t->stack_mirror, 0, t->stack_sz);
}

}
