#pragma once
#include <stdint.h>
// Type compatibility for libnx's inline wait/event declarations in host-only
// protocol tests. No newlib mutex implementation is linked or exercised.
typedef uint32_t _LOCK_T;
typedef struct { _LOCK_T lock; uint32_t counter; } _LOCK_RECURSIVE_T;
