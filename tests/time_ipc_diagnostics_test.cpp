// g++ -std=gnu++20 -I/opt/devkitpro/libnx/include -Iprojects/menu/src \
//     tests/time_ipc_diagnostics_test.cpp -o /tmp/time-ipc-test && /tmp/time-ipc-test
#include "services/TimeIpcDiagnostics.hpp"
#include <cassert>
#include <cstring>
#include <initializer_list>

int main() {
    alignas(16) unsigned char buffer[256]{};
    unsigned char before[sizeof(buffer)];
    for (u32 id : {0u, 1u, 2u, 3u, 4u, 20u}) {
        cmifMakeRequest(buffer, {.request_id = id});
        std::memcpy(before, buffer, sizeof(buffer));
        u32 type = 0, command = 999;
        assert(switchu::time_debug::readCommand(buffer, type, command));
        assert(type == CmifCommandType_Request && command == id);
        assert(std::memcmp(buffer, before, sizeof(buffer)) == 0);
    }

    // TIPC uses a distinct command type, even if its payload resembles SFCI.
    auto tipc = hipcMakeRequest(buffer, {.type = 16, .num_data_words = 8});
    auto* fakeHeader = static_cast<CmifInHeader*>(cmifGetAlignedDataStart(tipc.data_words, buffer));
    *fakeHeader = {CMIF_IN_HEADER_MAGIC, 0, 20, 0};
    u32 type = 0, command = 999;
    assert(!switchu::time_debug::readCommand(buffer, type, command));
    assert(type == 16 && command == 999);

    // Exercise alignment with and without handle descriptors, and preserve raw
    // service failures even when the IPC transport returned success.
    for (u32 handles : {0u, 1u, 2u}) {
        auto response = hipcMakeRequest(buffer, {.num_data_words = 8, .num_copy_handles = handles});
        auto* header = static_cast<CmifOutHeader*>(cmifGetAlignedDataStart(response.data_words, buffer));
        for (Result raw : {0u, static_cast<Result>(MAKERESULT(116, 102)), 0xDEADBEEFu}) {
            *header = {CMIF_OUT_HEADER_MAGIC, 0, raw, 0};
            std::memcpy(before, buffer, sizeof(buffer));
            Result result = 1;
            assert(switchu::time_debug::readResult(buffer, result));
            assert(result == raw);
            assert(std::memcmp(buffer, before, sizeof(buffer)) == 0);
        }
        header->magic = 0;
        Result result = 123;
        assert(!switchu::time_debug::readResult(buffer, result));
        assert(result == 123);
    }

    // TIPC/non-CMIF, empty and truncated responses have no CMIF Result.
    for (u32 words : {0u, 1u, 3u}) {
        std::memset(buffer, 0, sizeof(buffer));
        hipcMakeRequest(buffer, {.num_data_words = words});
        Result result = 123;
        u32 type = 0, command = 999;
        assert(!switchu::time_debug::readResult(buffer, result));
        assert(!switchu::time_debug::readCommand(buffer, type, command));
        assert(result == 123 && command == 999);
    }
}
