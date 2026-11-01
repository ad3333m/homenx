#pragma once
#include <switch.h>

#include <cstdint>

// Atmosphère's cheat service (dmnt:cht), read-only: what the achievement packs
// need from the running game, its memory. Commands and the metadata layout are
// Atmosphère's (stratosphere/dmnt: dmnt_cheat_service.hpp, CheatProcessMetadata).
namespace switchu::dmnt {

struct Extents {
    std::uint64_t base, size;
};
struct Metadata {
    std::uint64_t process_id, program_id;
    Extents main_nso, heap, alias, aslr;
    std::uint8_t main_nso_build_id[0x20];
};
static_assert(sizeof(Metadata) == 0x70);

inline Service& service() {
    static Service s{};
    return s;
}

inline bool open() {
    return serviceIsActive(&service()) || R_SUCCEEDED(smGetService(&service(), "dmnt:cht"));
}

inline void close() {
    serviceClose(&service());
}

inline bool hasCheatProcess() {
    std::uint8_t has = 0;
    return R_SUCCEEDED(serviceDispatchOut(&service(), 65000, has)) && has != 0;
}

// Attaches to the running game (cheats aren't changed by this).
inline bool forceOpenCheatProcess() {
    return R_SUCCEEDED(serviceDispatch(&service(), 65003));
}

inline bool metadata(Metadata& out) {
    return R_SUCCEEDED(serviceDispatchOut(&service(), 65002, out));
}

inline bool read(std::uint64_t address, void* buffer, std::size_t size) {
    const struct {
        std::uint64_t address, size;
    } in = {address, size};
    return R_SUCCEEDED(serviceDispatchIn(&service(), 65102, in,
                                         .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
                                         .buffers = {{buffer, size}}));
}

} // namespace switchu::dmnt
