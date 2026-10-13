#include "LeaveFrameCache.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace LeaveFrameCache {
namespace {

constexpr std::uint32_t kMagic = 0x4C555753u; // 'SWUL' LE
constexpr std::uint32_t kVersion = 1;
constexpr int kMaxSide = 1280;
constexpr std::size_t kMaxPixels = static_cast<std::size_t>(kMaxSide) * static_cast<std::size_t>(kMaxSide);

#pragma pack(push, 1)
struct Header {
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t page = 0;
    std::uint32_t openFolderId = 0;
    std::uint64_t focusTitleId = 0;
};
#pragma pack(pop)

static_assert(sizeof(Header) == 32, "LeaveFrameCache header size");

bool readHeader(std::ifstream& in, Header& header) {
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!in || in.gcount() != static_cast<std::streamsize>(sizeof(header)))
        return false;
    if (header.magic != kMagic || header.version != kVersion)
        return false;
    if (header.width == 0 || header.height == 0)
        return false;
    if (header.width > static_cast<std::uint32_t>(kMaxSide)
        || header.height > static_cast<std::uint32_t>(kMaxSide))
        return false;
    const std::size_t pixels = static_cast<std::size_t>(header.width)
                             * static_cast<std::size_t>(header.height);
    if (pixels > kMaxPixels)
        return false;
    return true;
}

} // namespace

bool save(const LeaveFrameSession& session,
          const std::uint8_t* rgba, int width, int height) {
    if (!rgba || width <= 0 || height <= 0 || width > kMaxSide || height > kMaxSide)
        return false;

    std::error_code ec;
    std::filesystem::create_directories("sdmc:/config/SwitchU", ec);

    Header header{};
    header.magic = kMagic;
    header.version = kVersion;
    header.width = static_cast<std::uint32_t>(width);
    header.height = static_cast<std::uint32_t>(height);
    header.page = session.page < 0 ? 0u : static_cast<std::uint32_t>(session.page);
    header.openFolderId = session.openFolderId;
    header.focusTitleId = session.focusTitleId;

    const std::size_t bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    const std::string tmpPath = std::string(kPath) + ".tmp";

    {
        // HomeNX: 64 KB at a time. HOME has one SD card session, so a single
        // 3.6 MB write held the music and the launch sound off the card for
        // ~100 ms right after pressing a game: the "little lag spike".
        std::FILE* out = std::fopen(tmpPath.c_str(), "wb");
        if (!out)
            return false;
        std::setvbuf(out, nullptr, _IONBF, 0);
        bool ok = std::fwrite(&header, sizeof(header), 1, out) == 1;
        constexpr std::size_t kChunk = 64 * 1024;
        for (std::size_t off = 0; ok && off < bytes; off += kChunk) {
            const std::size_t n = std::min(kChunk, bytes - off);
            ok = std::fwrite(rgba + off, 1, n, out) == n;
        }
        ok = std::fclose(out) == 0 && ok;
        if (!ok)
            return false;
    }

    std::filesystem::remove(kPath, ec);
    std::filesystem::rename(tmpPath, kPath, ec);
    if (ec) {
        std::filesystem::remove(tmpPath, ec);
        return false;
    }
    return true;
}

bool load(LeaveFrameSession& session, LeaveFrameImage& image) {
    session = {};
    image = {};

    std::ifstream in(kPath, std::ios::binary);
    if (!in)
        return false;

    Header header{};
    if (!readHeader(in, header))
        return false;

    const std::size_t bytes = static_cast<std::size_t>(header.width)
                            * static_cast<std::size_t>(header.height) * 4u;
    image.rgba.resize(bytes);
    in.read(reinterpret_cast<char*>(image.rgba.data()), static_cast<std::streamsize>(bytes));
    if (!in || in.gcount() != static_cast<std::streamsize>(bytes)) {
        image = {};
        return false;
    }

    image.width = static_cast<int>(header.width);
    image.height = static_cast<int>(header.height);
    session.valid = true;
    session.page = static_cast<int>(header.page);
    session.openFolderId = header.openFolderId;
    session.focusTitleId = header.focusTitleId;
    return image.valid();
}

bool loadSessionOnly(LeaveFrameSession& session) {
    session = {};
    std::ifstream in(kPath, std::ios::binary);
    if (!in)
        return false;

    Header header{};
    if (!readHeader(in, header))
        return false;

    session.valid = true;
    session.page = static_cast<int>(header.page);
    session.openFolderId = header.openFolderId;
    session.focusTitleId = header.focusTitleId;
    return true;
}

} // namespace LeaveFrameCache
