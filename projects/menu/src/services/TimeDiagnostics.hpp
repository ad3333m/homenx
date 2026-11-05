#pragma once
#include <switch.h>

namespace switchu::time_debug {
// Read-only probes; each service/session is acquired and released independently.
void probe(Result initializeResult);
#ifndef SWITCHU_HOMEBREW
void initializeLibcTime(Result initializeResult);
#endif
// Replay early, timestamp-free diagnostics once the normal file logger is ready.
void flush();
}
