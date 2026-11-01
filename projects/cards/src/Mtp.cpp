#include "Mtp.hpp"

#include <nxui/core/ThreadPool.hpp>
#include <switch.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <malloc.h>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

namespace cards {
namespace {

// ---------------------------------------------------------------- the protocol's numbers

constexpr std::uint32_t kStorage = 0x00010001;   // the SD card
constexpr std::uint32_t kRootParent = 0xFFFFFFFF; // "the root", as a parent in a request
constexpr std::size_t kBufSize = 0x100000;        // one USB transfer at most (0x1000-aligned)
constexpr const char* kRootPath = "sdmc:/";

enum : std::uint16_t { kCommand = 1, kData = 2, kResponse = 3 };
enum : std::uint16_t {
    GetDeviceInfo = 0x1001, OpenSession = 0x1002, CloseSession = 0x1003, GetStorageIDs = 0x1004,
    GetStorageInfo = 0x1005, GetNumObjects = 0x1006, GetObjectHandles = 0x1007, GetObjectInfo = 0x1008,
    GetObject = 0x1009, DeleteObject = 0x100B, SendObjectInfo = 0x100C, SendObject = 0x100D,
    GetDevicePropDesc = 0x1014, GetDevicePropValue = 0x1015, GetPartialObject = 0x101B,
    GetObjectPropsSupported = 0x9801, GetObjectPropDesc = 0x9802, GetObjectPropValue = 0x9803,
    SetObjectPropValue = 0x9804,
};
enum : std::uint16_t {
    Ok = 0x2001, GeneralError = 0x2002, NotSupported = 0x2005, InvalidStorage = 0x2008, InvalidHandle = 0x2009,
    DevicePropNotSupported = 0x200A, AccessDenied = 0x200F, NoValidObjectInfo = 0x2015, InvalidParent = 0x201A,
    InvalidPropCode = 0xA801,
};
// What a computer asks of a file (MTP object properties) and their types.
enum : std::uint16_t {
    PropStorage = 0xDC01, PropFormat = 0xDC02, PropProtection = 0xDC03, PropSize = 0xDC04, PropFileName = 0xDC07,
    PropModified = 0xDC09, PropParent = 0xDC0B, PropUniqueId = 0xDC41, PropName = 0xDC44,
};
enum : std::uint16_t { TypeU16 = 0x0004, TypeU32 = 0x0006, TypeU64 = 0x0008, TypeU128 = 0x000A, TypeStr = 0xFFFF };
constexpr std::uint16_t kFriendlyName = 0xD402; // a device property: its name on the computer
constexpr std::uint16_t kFileFormat = 0x3000, kFolderFormat = 0x3001;

// ---------------------------------------------------------------- text

std::u16string utf16(const std::string& s) {
    std::u16string out;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::uint32_t cp = c;
        int n = 1;
        if (c >= 0xF0) cp = c & 0x07, n = 4;
        else if (c >= 0xE0) cp = c & 0x0F, n = 3;
        else if (c >= 0xC0) cp = c & 0x1F, n = 2;
        for (int k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        i += n;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += static_cast<char16_t>(0xD800 + (cp >> 10));
            out += static_cast<char16_t>(0xDC00 + (cp & 0x3FF));
        } else {
            out += static_cast<char16_t>(cp);
        }
    }
    return out;
}

std::string utf8(const std::u16string& w) {
    std::string out;
    for (std::size_t i = 0; i < w.size(); ++i) {
        std::uint32_t cp = w[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < w.size()) cp = 0x10000 + ((cp - 0xD800) << 10) + (w[++i] - 0xDC00);
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

// A dataset going to the computer (little-endian, as the Switch is).
struct Out {
    std::vector<std::uint8_t> b;
    void add(const void* p, std::size_t n) {
        const auto* c = static_cast<const std::uint8_t*>(p);
        b.insert(b.end(), c, c + n);
    }
    void u8(std::uint8_t v) { b.push_back(v); }
    void u16(std::uint16_t v) { add(&v, 2); }
    void u32(std::uint32_t v) { add(&v, 4); }
    void u64(std::uint64_t v) { add(&v, 8); }
    void str(const std::string& s) { // a count (with the 0 at the end), UTF-16, the 0
        std::u16string w = utf16(s);
        if (w.empty()) {
            u8(0);
            return;
        }
        if (w.size() > 254) w.resize(254);
        u8(static_cast<std::uint8_t>(w.size() + 1));
        for (char16_t c : w) u16(static_cast<std::uint16_t>(c));
        u16(0);
    }
    void codes(std::initializer_list<std::uint16_t> list) {
        u32(static_cast<std::uint32_t>(list.size()));
        for (std::uint16_t c : list) u16(c);
    }
};

// A string in a dataset from the computer, at `at` (moved past it).
std::string readStr(const std::uint8_t* p, std::size_t size, std::size_t& at) {
    if (at >= size) return {};
    const std::size_t n = p[at++];
    std::u16string w;
    for (std::size_t i = 0; i < n && at + 1 < size; ++i, at += 2) {
        const char16_t c = static_cast<char16_t>(p[at] | (p[at + 1] << 8));
        if (c) w += c;
    }
    return utf8(w);
}

std::string dateText(std::time_t t) {
    std::tm tm{};
    if (const std::tm* got = std::gmtime(&t)) tm = *got;
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02dT%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min, tm.tm_sec);
    return buf;
}

std::string leaf(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// ---------------------------------------------------------------- USB

class Usb {
public:
    std::uint8_t* buf = nullptr; // every transfer goes through it (0x1000-aligned)
    std::size_t packet = 512;

    bool open() {
        buf = static_cast<std::uint8_t*>(memalign(0x1000, kBufSize));
        if (!buf || R_FAILED(usbDsInitialize())) return false;
        m_open = true;
        static const u16 langs[] = {0x0409};
        u8 iManufacturer = 0, iProduct = 0, iSerial = 0;
        Result rc = usbDsAddUsbLanguageStringDescriptor(nullptr, langs, 1);
        if (R_SUCCEEDED(rc)) rc = usbDsAddUsbStringDescriptor(&iManufacturer, "Nintendo");
        if (R_SUCCEEDED(rc)) rc = usbDsAddUsbStringDescriptor(&iProduct, "Nintendo Switch");
        if (R_SUCCEEDED(rc)) rc = usbDsAddUsbStringDescriptor(&iSerial, "HomeNX");
        // (Its own product id: 0x3000 is the one USB install tools bind a driver to.)
        usb_device_descriptor device = {USB_DT_DEVICE_SIZE, USB_DT_DEVICE, 0x0110, 0x00, 0x00, 0x00, 0x40,
                                        0x057E, 0x201D, 0x0100, iManufacturer, iProduct, iSerial, 0x01};
        if (R_SUCCEEDED(rc)) rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Full, &device);
        device.bcdUSB = 0x0200;
        if (R_SUCCEEDED(rc)) rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_High, &device);
        device.bcdUSB = 0x0300;
        device.bMaxPacketSize0 = 0x09;
        if (R_SUCCEEDED(rc)) rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Super, &device);
        static const u8 bos[0x16] = {0x05, USB_DT_BOS, 0x16, 0x00, 0x02,
                                     0x07, USB_DT_DEVICE_CAPABILITY, 0x02, 0x02, 0x00, 0x00, 0x00,
                                     0x0A, USB_DT_DEVICE_CAPABILITY, 0x03, 0x00, 0x0E, 0x00, 0x03, 0x00, 0x00, 0x00};
        if (R_SUCCEEDED(rc)) rc = usbDsSetBinaryObjectStore(bos, sizeof(bos));
        if (R_SUCCEEDED(rc)) rc = usbDsRegisterInterface(&m_intf);
        if (R_FAILED(rc)) return false;

        // A still-image (PTP) interface, which MTP is spoken over: bulk in and out, events.
        const u8 n = m_intf->interface_index;
        usb_interface_descriptor intf = {USB_DT_INTERFACE_SIZE, USB_DT_INTERFACE, n, 0, 3, 0x06, 0x01, 0x01, 0};
        usb_endpoint_descriptor in = {USB_DT_ENDPOINT_SIZE, USB_DT_ENDPOINT, static_cast<u8>(USB_ENDPOINT_IN + n + 1),
                                      USB_TRANSFER_TYPE_BULK, 0x40, 0};
        usb_endpoint_descriptor out = {USB_DT_ENDPOINT_SIZE, USB_DT_ENDPOINT, static_cast<u8>(USB_ENDPOINT_OUT + n + 1),
                                       USB_TRANSFER_TYPE_BULK, 0x40, 0};
        usb_endpoint_descriptor event = {USB_DT_ENDPOINT_SIZE, USB_DT_ENDPOINT, static_cast<u8>(USB_ENDPOINT_IN + n + 2),
                                         USB_TRANSFER_TYPE_INTERRUPT, 0x1C, 6};
        usb_ss_endpoint_companion_descriptor bulkMore = {USB_DT_SS_ENDPOINT_COMPANION_SIZE, USB_DT_SS_ENDPOINT_COMPANION,
                                                         0x0F, 0x00, 0x0000};
        usb_ss_endpoint_companion_descriptor eventMore = {USB_DT_SS_ENDPOINT_COMPANION_SIZE, USB_DT_SS_ENDPOINT_COMPANION,
                                                          0x00, 0x00, 0x001C};
        auto add = [&](UsbDeviceSpeed speed, const void* d, std::size_t size) {
            if (R_SUCCEEDED(rc)) rc = usbDsInterface_AppendConfigurationData(m_intf, speed, d, size);
        };
        for (UsbDeviceSpeed speed : {UsbDeviceSpeed_Full, UsbDeviceSpeed_High, UsbDeviceSpeed_Super}) {
            const bool super = speed == UsbDeviceSpeed_Super;
            const u16 most = speed == UsbDeviceSpeed_Full ? 0x40 : super ? 0x400 : 0x200;
            in.wMaxPacketSize = most;
            out.wMaxPacketSize = most;
            add(speed, &intf, USB_DT_INTERFACE_SIZE);
            add(speed, &in, USB_DT_ENDPOINT_SIZE);
            if (super) add(speed, &bulkMore, USB_DT_SS_ENDPOINT_COMPANION_SIZE);
            add(speed, &out, USB_DT_ENDPOINT_SIZE);
            if (super) add(speed, &bulkMore, USB_DT_SS_ENDPOINT_COMPANION_SIZE);
            add(speed, &event, USB_DT_ENDPOINT_SIZE);
            if (super) add(speed, &eventMore, USB_DT_SS_ENDPOINT_COMPANION_SIZE);
        }
        if (R_SUCCEEDED(rc)) rc = usbDsInterface_RegisterEndpoint(m_intf, &m_in, in.bEndpointAddress);
        if (R_SUCCEEDED(rc)) rc = usbDsInterface_RegisterEndpoint(m_intf, &m_out, out.bEndpointAddress);
        if (R_SUCCEEDED(rc)) rc = usbDsInterface_RegisterEndpoint(m_intf, &m_event, event.bEndpointAddress);
        if (R_SUCCEEDED(rc)) rc = usbDsInterface_EnableInterface(m_intf);
        if (R_SUCCEEDED(rc)) usbDsEndpoint_SetZlt(m_in, false); // (data ends are marked by hand: see finish())
        if (R_SUCCEEDED(rc)) rc = usbDsEnable();
        return R_SUCCEEDED(rc);
    }

    void close() {
        if (m_open) usbDsExit();
        m_open = false;
        std::free(buf);
        buf = nullptr;
    }

    // Until a computer has set the Switch up (false: asked to stop).
    bool waitConfigured(const std::atomic<bool>& stop) {
        while (!stop) {
            UsbState state = UsbState_Detached;
            if (R_SUCCEEDED(usbDsGetState(&state)) && state == UsbState_Configured) {
                UsbDeviceSpeed speed = UsbDeviceSpeed_High;
                usbDsGetSpeed(&speed);
                packet = speed == UsbDeviceSpeed_Super ? 0x400 : speed == UsbDeviceSpeed_Full ? 0x40 : 0x200;
                return true;
            }
            eventWait(usbDsGetStateChangeEvent(), 100'000'000ULL);
            eventClear(usbDsGetStateChangeEvent());
        }
        return false;
    }

    // From the computer into buf (at most kBufSize): false when the cable went or stop was asked.
    bool read(std::size_t& got, const std::atomic<bool>& stop) {
        u32 done = 0;
        if (!transfer(m_out, kBufSize, done, stop)) return false;
        got = done;
        return true;
    }

    bool write(std::size_t size, const std::atomic<bool>& stop) {
        u32 done = 0;
        return transfer(m_in, size, done, stop) && done == size;
    }

    // The end of what went to the computer: a data phase that ends exactly on a
    // packet needs an empty one after it, or the computer waits for more.
    bool finish(std::uint64_t total, const std::atomic<bool>& stop) {
        return total % packet != 0 || write(0, stop);
    }

private:
    bool transfer(UsbDsEndpoint* ep, std::size_t size, u32& done, const std::atomic<bool>& stop) {
        u32 urb = 0;
        if (R_FAILED(usbDsEndpoint_PostBufferAsync(ep, buf, size, &urb))) return false;
        while (R_FAILED(eventWait(&ep->CompletionEvent, 100'000'000ULL))) {
            UsbState state = UsbState_Configured;
            usbDsGetState(&state);
            if (stop || state != UsbState_Configured) { // unplugged, or turned off
                usbDsEndpoint_Cancel(ep);
                eventWait(&ep->CompletionEvent, 1'000'000'000ULL);
                eventClear(&ep->CompletionEvent);
                return false;
            }
        }
        eventClear(&ep->CompletionEvent);
        UsbDsReportData report{};
        if (R_FAILED(usbDsEndpoint_GetReportData(ep, &report))) return false;
        return R_SUCCEEDED(usbDsParseReportData(&report, urb, nullptr, &done));
    }

    bool m_open = false;
    UsbDsInterface* m_intf = nullptr;
    UsbDsEndpoint* m_in = nullptr;
    UsbDsEndpoint* m_out = nullptr;
    UsbDsEndpoint* m_event = nullptr;
};

// ---------------------------------------------------------------- a session

class Session {
public:
    Session(Usb& usb, const std::atomic<bool>& stop) : m_usb(usb), m_stop(stop) {}

    // Commands until the cable goes (or stop).
    void serve() {
        for (;;) {
            std::size_t got = 0;
            if (!m_usb.read(got, m_stop)) return;
            if (got < 12) continue;
            std::uint32_t length = 0, tid = 0;
            std::uint16_t type = 0, code = 0;
            std::memcpy(&length, m_usb.buf, 4);
            std::memcpy(&type, m_usb.buf + 4, 2);
            std::memcpy(&code, m_usb.buf + 6, 2);
            std::memcpy(&tid, m_usb.buf + 8, 4);
            if (type != kCommand) continue; // (the rest of a data phase not taken: skipped)
            std::uint32_t p[5] = {};
            const std::size_t params = std::min<std::size_t>(5, (std::min<std::size_t>(length, got) - 12) / 4);
            std::memcpy(p, m_usb.buf + 12, params * 4);
            if (!handle(code, tid, p)) return;
        }
    }

private:
    struct Obj {
        std::string path;
        std::uint32_t parent; // 0: in the root
    };

    // ---- what the computer is answered with

    bool respond(std::uint16_t rc, std::uint32_t tid, std::initializer_list<std::uint32_t> params = {}) {
        const std::uint32_t length = static_cast<std::uint32_t>(12 + 4 * params.size());
        const std::uint16_t type = kResponse;
        std::memcpy(m_usb.buf, &length, 4);
        std::memcpy(m_usb.buf + 4, &type, 2);
        std::memcpy(m_usb.buf + 6, &rc, 2);
        std::memcpy(m_usb.buf + 8, &tid, 4);
        std::size_t at = 12;
        for (std::uint32_t v : params) {
            std::memcpy(m_usb.buf + at, &v, 4);
            at += 4;
        }
        return m_usb.write(length, m_stop);
    }

    void header(std::uint64_t payload, std::uint16_t code, std::uint32_t tid) {
        const std::uint64_t total = 12 + payload;
        const std::uint32_t length = total > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(total);
        const std::uint16_t type = kData;
        std::memcpy(m_usb.buf, &length, 4);
        std::memcpy(m_usb.buf + 4, &type, 2);
        std::memcpy(m_usb.buf + 6, &code, 2);
        std::memcpy(m_usb.buf + 8, &tid, 4);
    }

    // A dataset to the computer, then OK.
    bool send(const Out& out, std::uint16_t code, std::uint32_t tid) {
        if (out.b.size() + 12 > kBufSize) return respond(GeneralError, tid);
        header(out.b.size(), code, tid);
        std::memcpy(m_usb.buf + 12, out.b.data(), out.b.size());
        const std::size_t total = 12 + out.b.size();
        return m_usb.write(total, m_stop) && m_usb.finish(total, m_stop) && respond(Ok, tid);
    }

    // Part of a file to the computer (the data phase only).
    bool sendFile(const std::string& path, std::uint64_t offset, std::uint64_t count, std::uint16_t code,
                  std::uint32_t tid) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        if (offset) std::fseek(f, static_cast<long>(offset), SEEK_SET);
        header(count, code, tid);
        std::uint64_t left = count, sent = 0;
        std::size_t at = 12;
        bool ok = true;
        do {
            const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(left, kBufSize - at));
            const std::size_t got = want ? std::fread(m_usb.buf + at, 1, want, f) : 0;
            if (got < want) std::memset(m_usb.buf + at + got, 0, want - got); // (shorter than it said: zeros)
            left -= want;
            ok = m_usb.write(at + want, m_stop);
            sent += at + want;
            at = 0;
        } while (ok && left > 0);
        std::fclose(f);
        return ok && m_usb.finish(sent, m_stop);
    }

    // A dataset from the computer (small: one or two transfers) without its header.
    bool receive(std::vector<std::uint8_t>& out) {
        std::size_t got = 0;
        if (!m_usb.read(got, m_stop) || got < 12) return false;
        std::uint32_t length = 0;
        std::memcpy(&length, m_usb.buf, 4);
        out.assign(m_usb.buf + 12, m_usb.buf + got);
        while (length != 0xFFFFFFFFu && out.size() + 12 < length && got == kBufSize) {
            if (!m_usb.read(got, m_stop)) return false;
            out.insert(out.end(), m_usb.buf, m_usb.buf + got);
        }
        return true;
    }

    // A file from the computer, into `path`.
    bool receiveFile(const std::string& path) {
        FILE* f = std::fopen(path.c_str(), "wb");
        std::size_t got = 0;
        if (!m_usb.read(got, m_stop) || got < 12) {
            if (f) std::fclose(f);
            return false;
        }
        std::uint32_t length = 0;
        std::memcpy(&length, m_usb.buf, 4);
        std::uint64_t written = got - 12;
        bool ok = f && std::fwrite(m_usb.buf + 12, 1, got - 12, f) == got - 12;
        // Until it has all it said, or (4 GB and up: "0xFFFFFFFF") a short transfer ends it.
        while (got == kBufSize && (length == 0xFFFFFFFFu || written + 12 < length)) {
            if (!m_usb.read(got, m_stop)) {
                ok = false;
                break;
            }
            if (f && got && std::fwrite(m_usb.buf, 1, got, f) != got) ok = false;
            written += got;
        }
        if (f) std::fclose(f);
        return ok;
    }

    // ---- the SD card's files, by handle

    std::uint32_t idFor(const std::string& path, std::uint32_t parent) {
        auto it = m_ids.find(path);
        if (it != m_ids.end()) return it->second;
        const std::uint32_t id = m_next++;
        m_ids[path] = id;
        m_objs[id] = Obj{path, parent};
        return id;
    }

    const Obj* find(std::uint32_t id) const {
        auto it = m_objs.find(id);
        return it == m_objs.end() ? nullptr : &it->second;
    }

    // A folder's path for a parent handle ("" for one that isn't there).
    std::string folder(std::uint32_t parent) const {
        if (parent == 0 || parent == kRootParent) return kRootPath;
        const Obj* o = find(parent);
        return o ? o->path + "/" : std::string();
    }

    void forget(const std::string& path) { // it and everything in it
        const std::string inside = path + "/";
        for (auto it = m_objs.begin(); it != m_objs.end();) {
            if (it->second.path == path || it->second.path.rfind(inside, 0) == 0) {
                m_ids.erase(it->second.path);
                it = m_objs.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::vector<std::uint32_t> children(std::uint32_t parent) {
        std::vector<std::uint32_t> out;
        const std::string dir = folder(parent);
        if (dir.empty()) return out;
        const std::uint32_t parentId = parent == kRootParent ? 0 : parent;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec))
            out.push_back(idFor(dir + e.path().filename().string(), parentId));
        return out;
    }

    // ---- the operations

    bool handle(std::uint16_t code, std::uint32_t tid, const std::uint32_t* p) {
        switch (code) {
            case GetDeviceInfo: {
                Out o;
                o.u16(100);   // the standard's version
                o.u32(6);     // Microsoft's extension (MTP)
                o.u16(100);
                o.str("microsoft.com: 1.0;");
                o.u16(0);     // functional mode
                o.codes({GetDeviceInfo, OpenSession, CloseSession, GetStorageIDs, GetStorageInfo, GetNumObjects,
                         GetObjectHandles, GetObjectInfo, GetObject, DeleteObject, SendObjectInfo, SendObject,
                         GetDevicePropDesc, GetDevicePropValue, GetPartialObject, GetObjectPropsSupported,
                         GetObjectPropDesc, GetObjectPropValue, SetObjectPropValue});
                o.codes({});                               // events
                o.codes({kFriendlyName});                  // device properties
                o.codes({});                               // capture formats
                o.codes({kFileFormat, kFolderFormat});     // playback formats
                o.str("Nintendo");
                o.str("Switch");
                o.str("HomeNX");
                o.str("HomeNX-SD");
                return send(o, code, tid);
            }
            case OpenSession:
                m_objs.clear();
                m_ids.clear();
                m_next = 1;
                return respond(Ok, tid);
            case CloseSession:
                return respond(Ok, tid);
            case GetStorageIDs: {
                Out o;
                o.u32(1);
                o.u32(kStorage);
                return send(o, code, tid);
            }
            case GetStorageInfo: {
                if (p[0] != kStorage) return respond(InvalidStorage, tid);
                s64 total = 0, free = 0;
                if (FsFileSystem* fs = fsdevGetDeviceFileSystem("sdmc")) {
                    fsFsGetTotalSpace(fs, "/", &total);
                    fsFsGetFreeSpace(fs, "/", &free);
                }
                Out o;
                o.u16(0x0004); // removable
                o.u16(0x0002); // folders
                o.u16(0x0000); // read and write
                o.u64(static_cast<std::uint64_t>(total));
                o.u64(static_cast<std::uint64_t>(free));
                o.u32(0xFFFFFFFF);
                o.str("SD Card");
                o.str("");
                return send(o, code, tid);
            }
            case GetNumObjects:
            case GetObjectHandles: {
                if (p[0] != kStorage && p[0] != 0xFFFFFFFF) return respond(InvalidStorage, tid);
                if (folder(p[2]).empty()) return respond(InvalidParent, tid);
                const std::vector<std::uint32_t> ids = children(p[2]);
                if (code == GetNumObjects) return respond(Ok, tid, {static_cast<std::uint32_t>(ids.size())});
                Out o;
                o.u32(static_cast<std::uint32_t>(ids.size()));
                for (std::uint32_t id : ids) o.u32(id);
                return send(o, code, tid);
            }
            case GetObjectInfo: {
                const Obj* obj = find(p[0]);
                struct stat st {};
                if (!obj || stat(obj->path.c_str(), &st) != 0) return respond(InvalidHandle, tid);
                const bool dir = S_ISDIR(st.st_mode);
                Out o;
                o.u32(kStorage);
                o.u16(dir ? kFolderFormat : kFileFormat);
                o.u16(0); // not protected
                o.u32(static_cast<std::uint32_t>(std::min<std::uint64_t>(static_cast<std::uint64_t>(st.st_size), 0xFFFFFFFFull)));
                o.u16(0); // no thumbnail: format, size, width, height
                o.u32(0);
                o.u32(0);
                o.u32(0);
                o.u32(0); // not a picture: width, height, depth
                o.u32(0);
                o.u32(0);
                o.u32(obj->parent);
                o.u16(dir ? 0x0001 : 0x0000); // a folder
                o.u32(0);
                o.u32(0);
                o.str(leaf(obj->path));
                o.str(dateText(st.st_mtime));
                o.str(dateText(st.st_mtime));
                o.str("");
                return send(o, code, tid);
            }
            case GetObject:
            case GetPartialObject: {
                const Obj* obj = find(p[0]);
                struct stat st {};
                if (!obj || stat(obj->path.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) return respond(InvalidHandle, tid);
                const std::uint64_t size = static_cast<std::uint64_t>(st.st_size);
                const std::uint64_t offset = code == GetPartialObject ? std::min<std::uint64_t>(p[1], size) : 0;
                const std::uint64_t count = code == GetPartialObject ? std::min<std::uint64_t>(p[2], size - offset) : size;
                if (!sendFile(obj->path, offset, count, code, tid)) return respond(GeneralError, tid);
                return code == GetPartialObject ? respond(Ok, tid, {static_cast<std::uint32_t>(count)}) : respond(Ok, tid);
            }
            case DeleteObject: {
                const Obj* obj = find(p[0]);
                if (!obj) return respond(InvalidHandle, tid);
                const std::string path = obj->path;
                std::error_code ec;
                std::filesystem::remove_all(path, ec);
                forget(path);
                return respond(ec ? AccessDenied : Ok, tid);
            }
            case SendObjectInfo: {
                std::vector<std::uint8_t> info;
                if (!receive(info)) return false;
                const std::string dir = folder(p[1]);
                if (dir.empty() || info.size() < 53) return respond(dir.empty() ? InvalidParent : GeneralError, tid);
                std::uint16_t format = 0;
                std::uint32_t size = 0;
                std::memcpy(&format, info.data() + 4, 2);
                std::memcpy(&size, info.data() + 8, 4);
                std::size_t at = 52;
                std::string name = readStr(info.data(), info.size(), at);
                std::replace(name.begin(), name.end(), '/', '_');
                if (name.empty() || name == "." || name == "..") return respond(GeneralError, tid);
                const std::string path = dir + name;
                const std::uint32_t parentId = p[1] == kRootParent ? 0 : p[1];
                if (format == kFolderFormat) {
                    std::error_code ec;
                    std::filesystem::create_directories(path, ec);
                    if (ec) return respond(AccessDenied, tid);
                    const std::uint32_t id = idFor(path, parentId);
                    m_pending.clear();
                    return respond(Ok, tid, {kStorage, p[1], id});
                }
                if (FILE* f = std::fopen(path.c_str(), "wb")) { // there now (an empty one: a file of 0 bytes ends here)
                    std::fclose(f);
                } else {
                    return respond(AccessDenied, tid);
                }
                forget(path);
                const std::uint32_t id = idFor(path, parentId);
                m_pending = path;
                (void)size;
                return respond(Ok, tid, {kStorage, p[1], id});
            }
            case SendObject: {
                if (m_pending.empty()) { // (no SendObjectInfo first: what comes is taken and dropped)
                    std::vector<std::uint8_t> drop;
                    receive(drop);
                    return respond(NoValidObjectInfo, tid);
                }
                const std::string path = m_pending;
                m_pending.clear();
                return respond(receiveFile(path) ? Ok : GeneralError, tid);
            }
            case GetDevicePropDesc:
            case GetDevicePropValue: {
                if (p[0] != kFriendlyName) return respond(DevicePropNotSupported, tid);
                Out o;
                if (code == GetDevicePropDesc) {
                    o.u16(kFriendlyName);
                    o.u16(TypeStr);
                    o.u8(0); // get only
                    o.str("Nintendo Switch");
                }
                o.str("Nintendo Switch");
                if (code == GetDevicePropDesc) o.u8(0); // no form
                return send(o, code, tid);
            }
            case GetObjectPropsSupported: {
                Out o;
                o.codes({PropStorage, PropFormat, PropProtection, PropSize, PropFileName, PropModified, PropParent,
                         PropUniqueId, PropName});
                return send(o, code, tid);
            }
            case GetObjectPropDesc: {
                std::uint16_t type = 0;
                switch (p[0]) {
                    case PropStorage: case PropParent: type = TypeU32; break;
                    case PropFormat: case PropProtection: type = TypeU16; break;
                    case PropSize: type = TypeU64; break;
                    case PropUniqueId: type = TypeU128; break;
                    case PropFileName: case PropModified: case PropName: type = TypeStr; break;
                    default: return respond(InvalidPropCode, tid);
                }
                Out o;
                o.u16(static_cast<std::uint16_t>(p[0]));
                o.u16(type);
                o.u8(p[0] == PropFileName || p[0] == PropName ? 1 : 0); // a name can be changed (rename)
                switch (type) {                                      // the default value
                    case TypeU16: o.u16(0); break;
                    case TypeU32: o.u32(0); break;
                    case TypeU64: o.u64(0); break;
                    case TypeU128: o.u64(0); o.u64(0); break;
                    default: o.str(""); break;
                }
                o.u32(0); // group
                o.u8(0);  // no form
                return send(o, code, tid);
            }
            case GetObjectPropValue: {
                const Obj* obj = find(p[0]);
                struct stat st {};
                if (!obj || stat(obj->path.c_str(), &st) != 0) return respond(InvalidHandle, tid);
                const bool dir = S_ISDIR(st.st_mode);
                Out o;
                switch (p[1]) {
                    case PropStorage: o.u32(kStorage); break;
                    case PropFormat: o.u16(dir ? kFolderFormat : kFileFormat); break;
                    case PropProtection: o.u16(0); break;
                    case PropSize: o.u64(static_cast<std::uint64_t>(st.st_size)); break;
                    case PropFileName: case PropName: o.str(leaf(obj->path)); break;
                    case PropModified: o.str(dateText(st.st_mtime)); break;
                    case PropParent: o.u32(obj->parent); break;
                    case PropUniqueId: o.u64(p[0]); o.u64(0); break;
                    default: return respond(InvalidPropCode, tid);
                }
                return send(o, code, tid);
            }
            case SetObjectPropValue: {
                std::vector<std::uint8_t> value;
                if (!receive(value)) return false;
                const Obj* obj = find(p[0]);
                if (!obj) return respond(InvalidHandle, tid);
                if (p[1] != PropFileName && p[1] != PropName) return respond(AccessDenied, tid);
                std::size_t at = 0;
                std::string name = readStr(value.data(), value.size(), at);
                std::replace(name.begin(), name.end(), '/', '_');
                if (name.empty()) return respond(GeneralError, tid);
                const std::string from = obj->path;
                const std::string to = from.substr(0, from.find_last_of('/') + 1) + name;
                const std::uint32_t parent = obj->parent;
                if (std::rename(from.c_str(), to.c_str()) != 0) return respond(AccessDenied, tid);
                forget(from);
                m_ids[to] = p[0]; // the same handle, its new name
                m_objs[p[0]] = Obj{to, parent};
                return respond(Ok, tid);
            }
            default:
                return respond(NotSupported, tid);
        }
    }

    Usb& m_usb;
    const std::atomic<bool>& m_stop;
    std::unordered_map<std::uint32_t, Obj> m_objs;
    std::unordered_map<std::string, std::uint32_t> m_ids;
    std::uint32_t m_next = 1;
    std::string m_pending; // the file SendObjectInfo made, which SendObject fills
};

} // namespace

void Mtp::start() {
    if (m_thread.joinable()) return;
    m_stop = false;
    m_state = State::Waiting;
    m_thread = std::thread([this]() { run(); });
}

void Mtp::stop() {
    m_stop = true;
    if (m_thread.joinable()) m_thread.join();
    m_state = State::Off;
}

void Mtp::run() {
    nxui::moveToWorkerCores(); // (off the screen's core, below it)
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3B);
    Usb usb;
    if (!usb.open()) {
        usb.close();
        m_state = State::Failed;
        return;
    }
    while (!m_stop) {
        m_state = State::Waiting;
        if (!usb.waitConfigured(m_stop)) break;
        m_state = State::Connected;
        Session(usb, m_stop).serve(); // until the cable goes
    }
    usb.close();
}

} // namespace cards
