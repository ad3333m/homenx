#pragma once
#include <switch.h>
#include <cstdint>

namespace switchu::daemon {

void initializeExternalContentAllocator();
// malloc heap (the AMS region): size, free bytes, largest allocatable block
void getMallocHeapStats(size_t* total, size_t* free, size_t* allocatable);
Result registerExternalContent(uint64_t program_id, const char* exefs_path);
Result unregisterExternalContent(uint64_t program_id);

}
