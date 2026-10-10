#include "MiiStore.hpp"
#include "core/DebugLog.hpp"
#include "themeshop/ThemeHttp.hpp"
#include <nxui/core/Renderer.hpp>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <cmath>

namespace {

constexpr std::uint32_t kFavourite[12] = {
    0xD21E14FF, 0xFF6E19FF, 0xFFD820FF, 0x78D220FF, 0x007830FF, 0x0A48B4FF,
    0x3CAADEFF, 0xF55A7DFF, 0x7328ADFF, 0x483818FF, 0xE0E0E0FF, 0x181814FF,
};
constexpr const char* kFavouriteNames[12] = {
    "Red", "Orange", "Yellow", "Light green", "Green", "Blue",
    "Light blue", "Pink", "Purple", "Brown", "White", "Black",
};
constexpr std::uint32_t kSkin[10] = {
    0xFFD3ADFF, 0xFFB68AFF, 0xDE7942FF, 0xFFAA8CFF, 0xAD5129FF,
    0x632C18FF, 0xFFD0B6FF, 0xF4AE7EFF, 0xCC7C52FF, 0x8E4A2EFF,
};
constexpr std::uint32_t kHairClassic[8] = {
    0x1E1A18FF, 0x402010FF, 0x5C180AFF, 0x7C3A14FF,
    0x787880FF, 0x4E3E10FF, 0x885818FF, 0xD0A04AFF,
};
constexpr std::uint32_t kEyeClassic[6] = {
    0x000000FF, 0x6C7070FF, 0x663C2CFF, 0x605E30FF, 0x4E7FAFFF, 0x4E8F70FF,
};

// Switch Miis index a 100-colour table past the classic ones; spread those
// over the hue wheel so different picks still look different.
nxui::Color paletteColour(u8 index, const std::uint32_t* classic, int classicCount) {
    if (index < classicCount) return nxui::Color(classic[index]);
    return nxui::Color::fromHSL(std::fmod(index * 0.137f, 1.f), 0.55f, 0.45f);
}

} // namespace

std::vector<MiiCharInfo> loadMiis(Result& rc) {
    std::vector<MiiCharInfo> out;
    rc = miiInitialize(MiiServiceType_System);
    if (R_FAILED(rc)) rc = miiInitialize(MiiServiceType_User);
    if (R_FAILED(rc)) {
        DebugLog::log("[mii] init rc=0x%X", rc);
        return out;
    }
    MiiDatabase db;
    rc = miiOpenDatabase(&db, MiiSpecialKeyCode_Normal);
    if (R_SUCCEEDED(rc)) {
        s32 count = 0;
        rc = miiDatabaseGetCount(&db, &count, MiiSourceFlag_Database);
        if (R_SUCCEEDED(rc) && count > 0) {
            out.resize((size_t)count);
            s32 total = 0;
            rc = miiDatabaseGet1(&db, MiiSourceFlag_Database, out.data(), count, &total);
            out.resize(R_SUCCEEDED(rc) ? (size_t)std::clamp(total, 0, count) : 0);
        }
        miiDatabaseClose(&db);
    }
    miiExit();
    DebugLog::log("[mii] loaded %d rc=0x%X", (int)out.size(), rc);
    return out;
}

std::string miiName(const MiiCharInfo& mii) {
    // libnx documents the name as UTF-16; detect byte order from the data.
    int le = 0, be = 0;
    for (u16 c : mii.mii_name) {
        if (!c) break;
        if (c < 0x100) ++le; else if ((c & 0xFF) == 0) ++be;
    }
    std::string out;
    for (u16 c : mii.mii_name) {
        if (!c) break;
        if (be > le) c = (u16)((c >> 8) | (c << 8));
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
        else { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
    }
    return out.empty() ? "Mii" : out;
}

// Mii Studio data: the Switch CharInfo fields in the order Mii Studio uses
// (mii2studio), then obfuscated: prefix 00, eo = (7 + (v ^ n)) & 0xFF, n = eo.
std::string miiStudioHex(const MiiCharInfo& m) {
    const u8 fields[46] = {
        m.mii_facial_hair_color, m.mii_beard_style, m.mii_width,
        m.mii_eye_thickness, m.mii_eye_color, m.mii_eye_angle, m.mii_eye_size, m.mii_eye_style,
        m.mii_eye_pos_x, m.mii_eye_pos_y,
        m.mii_eyebrow_thickness, m.mii_eyebrow_color, m.mii_eyebrow_angle, m.mii_eyebrow_size,
        m.mii_eyebrow_style, m.mii_eyebrow_pos_x, m.mii_eyebrow_pos_y,
        m.mii_face_color, m.mii_makeup_style, m.mii_face_shape, m.mii_wrinkles_style,
        m.mii_color, m.mii_sex,
        m.mii_glasses_color, m.mii_glasses_size, m.mii_glasses_style, m.mii_glasses_pos,
        m.mii_hair_color, m.mii_has_hair_flipped, m.mii_hair_style, m.mii_height,
        m.mii_mole_size, m.mii_has_mole, m.mii_mole_pos_x, m.mii_mole_pos_y,
        m.mii_mouth_thickness, m.mii_mouth_color, m.mii_mouth_size, m.mii_mouth_style, m.mii_mouth_pos,
        m.mii_mustache_size, m.mii_mustache_style, m.mii_mustache_pos,
        m.mii_nose_size, m.mii_nose_style, m.mii_nose_pos,
    };
    std::string hex = "00";
    unsigned n = 256;
    char buf[3];
    for (u8 v : fields) {
        const unsigned eo = (7 + (v ^ n)) & 0xFF;
        n = eo;
        std::snprintf(buf, sizeof(buf), "%02x", eo);
        hex += buf;
    }
    return hex;
}

std::vector<std::uint8_t> miiFaceImage(const MiiCharInfo& mii) {
    static bool offline = false; // after one failed fetch, stop trying this session
    const std::string hex = miiStudioHex(mii);
    const std::string dir = "sdmc:/config/SwitchU/mii_faces";
    const std::string cached = dir + "/" + hex + ".png";

    std::vector<std::uint8_t> png;
    if (std::ifstream in{cached, std::ios::binary}) {
        png.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (!png.empty()) return png;
    }
    if (offline || !themeshop::http::initialize()) return {};

    static const char* const hosts[] = {
        "https://studio.mii.nintendo.com/miis/image.png",
        "https://mii-unsecure.ariankordi.net/miis/image.png",
    };
    for (const char* host : hosts) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        try {
            png = themeshop::http::getBytes(
                std::string(host) + "?data=" + hex + "&type=face&expression=normal&width=256&bgColor=FFFFFF00",
                {}, {}, [deadline] { return std::chrono::steady_clock::now() > deadline; });
        } catch (const std::exception& e) {
            DebugLog::log("[mii] face fetch failed (%s): %s", host, e.what());
            png.clear();
        }
        // PNG signature: some servers answer errors with HTML.
        if (png.size() > 8 && png[0] == 0x89 && png[1] == 'P') break;
        png.clear();
    }
    if (png.empty()) {
        offline = true;
        return {};
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream(cached, std::ios::binary).write(reinterpret_cast<const char*>(png.data()), png.size());
    return png;
}

std::string miiFavouriteColourName(const MiiCharInfo& mii) {
    return kFavouriteNames[mii.mii_color % 12];
}

void drawMiiPortrait(nxui::Renderer& ren, const nxui::Rect& r, const MiiCharInfo& mii, float opacity) {
    const float w = std::min(r.width, r.height);
    const float cx = r.x + r.width * 0.5f, cy = r.y + r.height * 0.5f;
    const nxui::Color fav(kFavourite[mii.mii_color % 12]);
    const nxui::Color skin(kSkin[mii.mii_face_color % 10]);
    const nxui::Color hair = paletteColour(mii.mii_hair_color, kHairClassic, 8);
    const nxui::Color eye = paletteColour(mii.mii_eye_color, kEyeClassic, 6);

    ren.drawCircle({cx, cy}, w * 0.5f, fav.withAlpha(0.35f * opacity), 48);
    ren.drawCircle({cx, cy}, w * 0.5f - 6.f, fav.withAlpha(0.55f * opacity), 48);
    // Hair behind the head, then the face, then the fringe on top.
    ren.drawCircle({cx, cy - w * 0.04f}, w * 0.33f, hair.withAlpha(opacity), 40);
    ren.drawCircle({cx, cy + w * 0.04f}, w * 0.27f, skin.withAlpha(opacity), 40);
    ren.drawRoundedRect({cx - w * 0.28f, cy - w * 0.27f, w * 0.56f, w * 0.13f},
                        hair.withAlpha(opacity), w * 0.065f);
    // Eyes and mouth.
    const float eyeY = cy + w * 0.03f;
    ren.drawCircle({cx - w * 0.095f, eyeY}, w * 0.032f, eye.withAlpha(opacity), 16);
    ren.drawCircle({cx + w * 0.095f, eyeY}, w * 0.032f, eye.withAlpha(opacity), 16);
    ren.drawRoundedRect({cx - w * 0.06f, cy + w * 0.15f, w * 0.12f, w * 0.028f},
                        nxui::Color(0xB83A48FF).withAlpha(opacity), w * 0.014f);
}
