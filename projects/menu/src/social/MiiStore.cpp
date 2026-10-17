#include "social/MiiStore.hpp"
#include "core/DebugLog.hpp"

#include <cstdio>
#include <cstring>

// IDatabaseService (mii:e) commands libnx doesn't wrap, from Ryujinx's
// implementation: 12 Move(CreateId, s32), 13 AddOrReplace(StoreData),
// 14 Delete(CreateId), 25 ConvertCharInfoToCoreData(CharInfo) -> CoreData,
// 26 Append(CharInfo) [10.2.0+].
namespace {

static_assert(sizeof(MiiCharInfo) == 0x58, "CharInfo layout");

struct MiiSession {
    Result rc;
    MiiDatabase db{};
    MiiSession() {
        rc = miiInitialize(MiiServiceType_System);
        if (R_FAILED(rc)) return;
        rc = miiOpenDatabase(&db, MiiSpecialKeyCode_Normal);
        if (R_FAILED(rc)) miiExit();
    }
    ~MiiSession() {
        if (R_SUCCEEDED(rc)) {
            miiDatabaseClose(&db);
            miiExit();
        }
    }
    Service* s() { return &db.s; }
};

// Names are UTF-16LE (libnx's header says BE; the console and Ryujinx use LE).
std::string nameToUtf8(const u16* s, size_t n) {
    std::string out;
    for (size_t i = 0; i < n && s[i]; ++i) {
        u32 c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000)
            c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

// Up to 10 characters (the Mii limit); only the Basic Multilingual Plane.
void setName(MiiCharInfo& info, const std::string& utf8) {
    std::memset(info.mii_name, 0, sizeof(info.mii_name));
    size_t n = 0;
    for (size_t i = 0; i < utf8.size() && n < 10;) {
        const unsigned char b = static_cast<unsigned char>(utf8[i]);
        u32 c;
        int len;
        if (b < 0x80) { c = b; len = 1; }
        else if ((b & 0xE0) == 0xC0) { c = b & 0x1F; len = 2; }
        else if ((b & 0xF0) == 0xE0) { c = b & 0x0F; len = 3; }
        else { c = b & 0x07; len = 4; }
        if (i + len > utf8.size()) break;
        for (int k = 1; k < len; ++k) c = (c << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3F);
        i += len;
        if (c >= 0x10000 || c < 0x20) continue;
        info.mii_name[n++] = static_cast<u16>(c);
    }
}

u16 crc16(const u8* data, size_t size, u32 crc) {
    for (size_t i = 0; i < size; ++i) {
        crc ^= static_cast<u32>(data[i]) << 8;
        for (int j = 0; j < 8; ++j) {
            crc <<= 1;
            if (crc & 0x10000) crc = (crc ^ 0x1021) & 0xFFFF;
        }
    }
    return static_cast<u16>(crc);
}

u16 swap16(u16 v) { return static_cast<u16>((v >> 8) | (v << 8)); }

struct StoreData {
    u8 core[0x30];
    MiiCreateId id;
    u16 dataCrc;
    u16 deviceCrc;
};
static_assert(sizeof(StoreData) == 0x44, "StoreData layout");

// In-place replace: the CoreData the console makes from the CharInfo, the same
// id, and the two CRCs (the device one is keyed by this console's Mii author id).
Result replaceInPlace(MiiSession& m, const MiiCharInfo& info) {
    StoreData sd{};
    Result rc = serviceDispatchInOut(m.s(), 25, info, sd.core);
    if (R_FAILED(rc)) return rc;
    sd.id = info.create_id;
    sd.dataCrc = swap16(crc16(reinterpret_cast<const u8*>(&sd), 0x40, 0));
    Uuid author{};
    rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        rc = setsysGetMiiAuthorId(&author);
        setsysExit();
    }
    if (R_FAILED(rc)) return rc;
    const u16 seed = crc16(reinterpret_cast<const u8*>(&author), sizeof(author), 0);
    sd.deviceCrc = swap16(crc16(reinterpret_cast<const u8*>(&sd), 0x42, seed));
    return serviceDispatchIn(m.s(), 13, sd);
}

} // namespace

Result loadMiis(std::vector<MiiEntry>& out) {
    out.clear();
    MiiSession m;
    if (R_FAILED(m.rc)) {
        DebugLog::log("[mii] mii:e unavailable rc=0x%X", m.rc);
        return m.rc;
    }
    s32 count = 0;
    Result rc = miiDatabaseGetCount(&m.db, &count, MiiSourceFlag_Database);
    if (R_FAILED(rc) || count <= 0) return rc;
    std::vector<MiiCharInfo> infos(static_cast<size_t>(count));
    s32 total = 0;
    rc = miiDatabaseGet1(&m.db, MiiSourceFlag_Database, infos.data(), count, &total);
    if (R_FAILED(rc)) return rc;
    for (s32 i = 0; i < total && i < count; ++i) {
        MiiEntry e;
        e.info = infos[static_cast<size_t>(i)];
        e.name = nameToUtf8(e.info.mii_name, 10);
        e.faceUrl = miiFaceUrl(e.info);
        out.push_back(std::move(e));
    }
    DebugLog::log("[mii] %d Miis", static_cast<int>(out.size()));
    return 0;
}

Result buildRandomMii(int gender, MiiEntry& out) {
    MiiSession m;
    if (R_FAILED(m.rc)) return m.rc;
    const MiiGender g = gender == 0 ? MiiGender_Male : gender == 1 ? MiiGender_Female : MiiGender_All;
    const Result rc = miiDatabaseBuildRandom(&m.db, MiiAge_All, g, MiiFaceColor_All, &out.info);
    if (R_SUCCEEDED(rc)) {
        out.name = nameToUtf8(out.info.mii_name, 10);
        out.faceUrl = miiFaceUrl(out.info);
    }
    return rc;
}

Result addMii(MiiCharInfo info, const std::string& name) {
    MiiSession m;
    if (R_FAILED(m.rc)) return m.rc;
    setName(info, name);
    const Result rc = serviceDispatchIn(m.s(), 26, info);
    DebugLog::log("[mii] add rc=0x%X", rc);
    return rc;
}

Result renameMii(const MiiEntry& mii, int index, const std::string& name) {
    MiiCharInfo info = mii.info;
    setName(info, name);
    return updateMii(mii, index, info);
}

Result updateMii(const MiiEntry& mii, int index, const MiiCharInfo& changed) {
    MiiSession m;
    if (R_FAILED(m.rc)) return m.rc;
    MiiCharInfo info = changed;
    Result rc = replaceInPlace(m, info);
    DebugLog::log("[mii] update in place rc=0x%X", rc);
    if (R_SUCCEEDED(rc)) return rc;
    // Fallback: add the renamed copy, remove the old one, and move it back.
    rc = serviceDispatchIn(m.s(), 26, info);
    if (R_FAILED(rc)) return rc;
    serviceDispatchIn(m.s(), 14, mii.info.create_id);
    // Append gives it a new id: find it (last) and put it where the old one was.
    s32 count = 0;
    if (R_SUCCEEDED(miiDatabaseGetCount(&m.db, &count, MiiSourceFlag_Database)) && count > 0) {
        std::vector<MiiCharInfo> infos(static_cast<size_t>(count));
        s32 total = 0;
        if (R_SUCCEEDED(miiDatabaseGet1(&m.db, MiiSourceFlag_Database, infos.data(), count, &total)) && total > 0) {
            struct { MiiCreateId id; s32 index; } move{infos[static_cast<size_t>(total - 1)].create_id, index};
            const Result mv = serviceDispatchIn(m.s(), 12, move);
            DebugLog::log("[mii] rename by re-adding, move rc=0x%X", mv);
        }
    }
    return 0;
}

Result deleteMii(const MiiCharInfo& info) {
    MiiSession m;
    if (R_FAILED(m.rc)) return m.rc;
    const Result rc = serviceDispatchIn(m.s(), 14, info.create_id);
    DebugLog::log("[mii] delete rc=0x%X", rc);
    return rc;
}

// Mii Studio's face renderer takes 46 bytes of the Mii in its own order,
// scrambled byte by byte (mii2studio's "switchgame" mapping = CharInfo).
std::string miiFaceUrl(const MiiCharInfo& c) {
    const u8 v[] = {
        c.mii_facial_hair_color, c.mii_beard_style, c.mii_width, c.mii_eye_thickness,
        c.mii_eye_color, c.mii_eye_angle, c.mii_eye_size, c.mii_eye_style,
        c.mii_eye_pos_x, c.mii_eye_pos_y, c.mii_eyebrow_thickness, c.mii_eyebrow_color,
        c.mii_eyebrow_angle, c.mii_eyebrow_size, c.mii_eyebrow_style, c.mii_eyebrow_pos_x,
        c.mii_eyebrow_pos_y, c.mii_face_color, c.mii_makeup_style, c.mii_face_shape,
        c.mii_wrinkles_style, c.mii_color, c.mii_sex, c.mii_glasses_color,
        c.mii_glasses_size, c.mii_glasses_style, c.mii_glasses_pos, c.mii_hair_color,
        c.mii_has_hair_flipped, c.mii_hair_style, c.mii_height, c.mii_mole_size,
        c.mii_has_mole, c.mii_mole_pos_x, c.mii_mole_pos_y, c.mii_mouth_thickness,
        c.mii_mouth_color, c.mii_mouth_size, c.mii_mouth_style, c.mii_mouth_pos,
        c.mii_mustache_size, c.mii_mustache_style, c.mii_mustache_pos, c.mii_nose_size,
        c.mii_nose_style, c.mii_nose_pos,
    };
    static_assert(sizeof(v) == 46, "Mii Studio data is 46 bytes");
    std::string data = "00";
    int n = 256;
    char hex[3];
    for (u8 b : v) {
        const int e = (7 + (b ^ n)) % 256;
        n = e;
        std::snprintf(hex, sizeof(hex), "%02x", e);
        data += hex;
    }
    return "https://studio.mii.nintendo.com/miis/image.png?data=" + data
         + "&type=face&expression=normal&width=270&bgColor=FFFFFF00"; // Mii Studio's sizes: 96, 128, 270, 512
}
