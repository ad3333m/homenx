// Checks switchu/achievement_packs.hpp on a PC (CI runs it): a pack is read, each
// version's condition is picked by build id, and pointer chains, sizes and the
// comparisons work against a made-up memory.
//   g++ -std=c++20 -I projects/common/include tools/test_achievement_packs.cpp && ./a.out
#undef NDEBUG
#include <switchu/achievement_packs.hpp>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>

int main() {
    namespace packs = switchu::packs;
    std::istringstream text(
        "# a test pack\n"
        "game 0100000000010000 Test Game\n"
        "a moons | Moon hunter | Collect 100 moons\n"
        "a flag | Done | A flag is set\n"
        "a bad id | Nope | an id with a space is skipped\n"
        "a speed | Fast | A float\n"
        "build 3ca12dfaab9c82da\n"
        "c moons | u32 [[main+10]+20]+8 >= 100\n"
        "c flag | u8 heap+40 & 0x4\n"
        "c speed | f32 heap+50 > 2.5\n"
        "c nothing | u32 main+0 == 1\n"
        "build 1111\n"
        "c moons | s16 main-8 < 0\n");
    const packs::Pack p = packs::parse(text);
    assert(p.valid() && p.title == 0x0100000000010000ULL && p.game == "Test Game");
    assert(p.list.size() == 3 && p.list[0].id == "moons" && p.list[0].about == "Collect 100 moons");

    std::vector<std::uint8_t> ram(0x10000, 0);
    auto put = [&](std::uint64_t at, const void* v, std::size_t n) { std::memcpy(&ram[at], v, n); };
    auto put64 = [&](std::uint64_t at, std::uint64_t v) { put(at, &v, 8); };
    const packs::Reader read = [&](std::uint64_t at, void* out, std::size_t n) {
        if (at + n > ram.size()) return false;
        std::memcpy(out, &ram[at], n);
        return true;
    };
    const packs::Bases b{0x1000, 0x8000, 0};
    put64(0x1000 + 0x10, 0x2000); // [main+10] = 0x2000
    put64(0x2000 + 0x20, 0x3000); // [0x2000+20] = 0x3000
    const std::uint32_t ninetyNine = 99, hundred = 100;
    put(0x3000 + 0x8, &ninetyNine, 4);

    const packs::Condition* moons = packs::forBuild(p.list[0], "3CA12DFAAB9C82DA");
    assert(moons && !packs::holds(*moons, b, read));
    put(0x3000 + 0x8, &hundred, 4);
    assert(packs::holds(*moons, b, read));
    put64(0x1000 + 0x10, 0); // a pointer not set up yet: false, not a crash
    assert(!packs::holds(*moons, b, read));

    const packs::Condition* flag = packs::forBuild(p.list[1], "3CA12DFAAB9C82DAFFFFFFFF"); // (a longer build id)
    assert(flag && !packs::holds(*flag, b, read));
    const std::uint8_t bits = 0x6;
    put(0x8000 + 0x40, &bits, 1);
    assert(packs::holds(*flag, b, read));

    const packs::Condition* speed = packs::forBuild(p.list[2], "3CA12DFAAB9C82DA");
    const float slow = 2.f, fast = 3.f;
    put(0x8000 + 0x50, &slow, 4);
    assert(speed && !packs::holds(*speed, b, read));
    put(0x8000 + 0x50, &fast, 4);
    assert(packs::holds(*speed, b, read));

    // Another version: only what it lists; signed values.
    const packs::Condition* old = packs::forBuild(p.list[0], "1111AAAABBBBCCCC");
    assert(old && !packs::forBuild(p.list[1], "1111AAAABBBBCCCC"));
    const std::int16_t minus = -3;
    put(0x1000 - 8, &minus, 2);
    assert(packs::holds(*old, b, read));
    assert(!packs::forBuild(p.list[0], "2222"));

    std::puts("achievement packs: ok");
    return 0;
}
