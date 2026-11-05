#pragma once
#include <switch/sf/cmif.h>

namespace switchu::time_debug {
// Read headers without touching the IPC buffer libnx still needs to parse.
template <typename Header>
inline const Header* readHeader(void* buffer, u32* words, u32 count) {
    if (!count) return nullptr;
    const auto* start = static_cast<const unsigned char*>(cmifGetAlignedDataStart(words, buffer));
    const auto* end = reinterpret_cast<const unsigned char*>(words + count);
    if (start > end || static_cast<size_t>(end - start) < sizeof(Header)) return nullptr;
    return reinterpret_cast<const Header*>(start);
}

inline bool readCommand(void* buffer, u32& type, u32& command) {
    const auto request = hipcParseRequest(buffer);
    type = request.meta.type;
    if (type != CmifCommandType_Request && type != CmifCommandType_Control &&
        type != CmifCommandType_RequestWithContext && type != CmifCommandType_ControlWithContext)
        return false;
    const auto* header = readHeader<CmifInHeader>(buffer, request.data.data_words, request.meta.num_data_words);
    if (!header || header->magic != CMIF_IN_HEADER_MAGIC) return false;
    command = header->command_id;
    return true;
}

inline bool readResult(void* buffer, Result& result) {
    const auto response = hipcParseResponse(buffer);
    const auto* header = readHeader<CmifOutHeader>(buffer, response.data_words, response.num_data_words);
    if (!header || header->magic != CMIF_OUT_HEADER_MAGIC) return false;
    result = header->result;
    return true;
}
}
