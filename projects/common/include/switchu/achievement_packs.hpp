#pragma once
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// HomeNX achievement packs: a game's own achievements, in a text file anyone can
// write (docs/achievement-packs.md). While the game runs, the daemon reads its
// memory (Atmosphère's dmnt:cht) and an achievement unlocks the moment its
// condition holds. Addresses belong to one version of the game, so each
// version's conditions follow a "build" line with its build id (the name of
// that version's cheat file).
//
//   game 0100000000010000 Super Mario Odyssey
//   a moons100 | Moon hunter | Collect 100 Power Moons
//   build 3CA12DFAAB9C82DA
//   c moons100 | u32 [[main+2D21A90]+20]+1C0 >= 100
namespace switchu::packs {

struct Condition {
    std::string address; // main / heap / alias / hex, + and - hex offsets, [ ] reads a pointer there
    int bytes = 4;       // u8 u16 u32 u64 s8 s16 s32 s64 f32 f64
    bool isSigned = false, isFloat = false;
    std::string op;      // == != >= > <= < & (any of these bits)
    double value = 0;
};

struct Achievement {
    std::string id, name, about;
    std::vector<std::pair<std::string, Condition>> builds; // build id (upper hex) -> its condition
};

struct Pack {
    std::uint64_t title = 0;
    std::string game;
    std::vector<Achievement> list;
    bool valid() const { return title != 0 && !list.empty(); }
};

// Where packs are: anyone's own first, then the ones HomeNX ships.
inline constexpr const char* kDirs[] = {"sdmc:/config/HomeNX/achievements/", "sdmc:/switch/SwitchU/achievements/"};

inline std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

inline std::vector<std::string> fields(const std::string& s) { // split at '|'
    std::vector<std::string> out;
    size_t at = 0;
    for (;;) {
        const size_t bar = s.find('|', at);
        out.push_back(trim(s.substr(at, bar == std::string::npos ? std::string::npos : bar - at)));
        if (bar == std::string::npos) return out;
        at = bar + 1;
    }
}

inline std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

inline bool parseCondition(const std::string& text, Condition& c) {
    std::istringstream in(text);
    std::string type, number;
    if (!(in >> type >> c.address >> c.op >> number)) return false;
    type = upper(type);
    if (type.size() < 2 || (type[0] != 'U' && type[0] != 'S' && type[0] != 'F')) return false;
    const int bits = std::atoi(type.c_str() + 1);
    if (bits != 8 && bits != 16 && bits != 32 && bits != 64) return false;
    c.bytes = bits / 8;
    c.isSigned = type[0] == 'S';
    c.isFloat = type[0] == 'F';
    if (c.isFloat && c.bytes < 4) return false;
    static const char* const kOps[] = {"==", "!=", ">=", ">", "<=", "<", "&"};
    bool known = false;
    for (const char* op : kOps) known |= c.op == op;
    if (!known) return false;
    c.value = number.rfind("0x", 0) == 0 || number.rfind("0X", 0) == 0 ? static_cast<double>(std::strtoull(number.c_str(), nullptr, 16))
                                                                        : std::strtod(number.c_str(), nullptr);
    return true;
}

inline bool goodId(const std::string& id) {
    if (id.empty() || id.size() > 40) return false;
    for (const char c : id)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.') return false;
    return true;
}

inline Pack parse(std::istream& in) {
    Pack p;
    std::string line, build;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const size_t space = line.find(' ');
        const std::string word = line.substr(0, space), rest = space == std::string::npos ? std::string() : trim(line.substr(space + 1));
        if (word == "game") {
            const size_t s2 = rest.find(' ');
            p.title = std::strtoull(rest.substr(0, s2).c_str(), nullptr, 16);
            p.game = s2 == std::string::npos ? std::string() : trim(rest.substr(s2 + 1));
        } else if (word == "a") {
            const auto f = fields(rest);
            if (f.size() >= 2 && goodId(f[0])) p.list.push_back({f[0], f[1], f.size() > 2 ? f[2] : std::string(), {}});
        } else if (word == "build") {
            build = upper(trim(rest));
        } else if (word == "c" && !build.empty()) {
            const auto f = fields(rest);
            Condition c;
            if (f.size() < 2 || !parseCondition(f[1], c)) continue;
            for (Achievement& a : p.list)
                if (a.id == f[0]) a.builds.emplace_back(build, c);
        }
    }
    return p;
}

// The game's pack (anyone's own first), or an empty one.
inline Pack load(std::uint64_t title) {
    char name[24];
    std::snprintf(name, sizeof(name), "%016llX.txt", static_cast<unsigned long long>(title));
    for (const char* dir : kDirs) {
        std::ifstream in(std::string(dir) + name);
        if (!in) continue;
        Pack p = parse(in);
        if (p.valid() && p.title == title) return p;
    }
    return {};
}

// The condition for this version of the game (its build id, upper hex), if the pack has one.
inline const Condition* forBuild(const Achievement& a, const std::string& build) {
    for (const auto& [b, c] : a.builds)
        if (!b.empty() && build.compare(0, b.size(), b) == 0) return &c;
    return nullptr;
}

// Reads `size` bytes of the game's memory at `address`.
using Reader = std::function<bool(std::uint64_t address, void* out, std::size_t size)>;
struct Bases {
    std::uint64_t main = 0, heap = 0, alias = 0;
};

namespace detail {
// address: term (('+' | '-') hex)*; term: main | heap | alias | hex | '[' address ']' (the pointer there)
inline bool address(const std::string& s, size_t& at, const Bases& b, const Reader& read, std::uint64_t& out, int depth = 0);

inline bool hexNumber(const std::string& s, size_t& at, std::uint64_t& out) {
    if (s.compare(at, 2, "0x") == 0 || s.compare(at, 2, "0X") == 0) at += 2;
    const size_t start = at;
    out = 0;
    while (at < s.size() && std::isxdigit(static_cast<unsigned char>(s[at]))) {
        const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(s[at++])));
        out = out * 16 + static_cast<std::uint64_t>(c <= '9' ? c - '0' : c - 'A' + 10);
    }
    return at > start;
}

inline bool term(const std::string& s, size_t& at, const Bases& b, const Reader& read, std::uint64_t& out, int depth) {
    if (at < s.size() && s[at] == '[') {
        ++at;
        std::uint64_t inner = 0;
        if (!address(s, at, b, read, inner, depth + 1) || at >= s.size() || s[at] != ']') return false;
        ++at;
        return inner != 0 && read(inner, &out, sizeof(out)) && out != 0;
    }
    for (const auto& [word, base] : {std::pair<const char*, std::uint64_t>{"main", b.main}, {"heap", b.heap}, {"alias", b.alias}})
        if (s.compare(at, std::strlen(word), word) == 0) {
            at += std::strlen(word);
            out = base;
            return base != 0;
        }
    return hexNumber(s, at, out);
}

inline bool address(const std::string& s, size_t& at, const Bases& b, const Reader& read, std::uint64_t& out, int depth) {
    if (depth > 8 || !term(s, at, b, read, out, depth)) return false;
    while (at < s.size() && (s[at] == '+' || s[at] == '-')) {
        const bool minus = s[at++] == '-';
        std::uint64_t n = 0;
        if (!hexNumber(s, at, n)) return false;
        out = minus ? out - n : out + n;
    }
    return true;
}
} // namespace detail

// Whether it holds now (false when it can't be read: a pointer not set up yet).
inline bool holds(const Condition& c, const Bases& b, const Reader& read) {
    size_t at = 0;
    std::uint64_t where = 0;
    if (!detail::address(c.address, at, b, read, where) || at != c.address.size() || where == 0) return false;
    std::uint8_t raw[8] = {};
    if (!read(where, raw, static_cast<std::size_t>(c.bytes))) return false;
    std::uint64_t u = 0;
    std::memcpy(&u, raw, sizeof(u)); // little-endian, as the Switch is
    double v = 0;
    if (c.isFloat) {
        if (c.bytes == 4) {
            float f = 0;
            std::memcpy(&f, raw, 4);
            v = f;
        } else {
            std::memcpy(&v, raw, 8);
        }
    } else if (c.isSigned) {
        const int shift = 64 - c.bytes * 8;
        v = static_cast<double>(static_cast<std::int64_t>(u << shift) >> shift);
    } else {
        v = static_cast<double>(u);
    }
    if (c.op == "==") return v == c.value;
    if (c.op == "!=") return v != c.value;
    if (c.op == ">=") return v >= c.value;
    if (c.op == ">") return v > c.value;
    if (c.op == "<=") return v <= c.value;
    if (c.op == "<") return v < c.value;
    return (u & static_cast<std::uint64_t>(c.value)) != 0; // "&"
}

} // namespace switchu::packs
