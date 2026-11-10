#include "BluetoothManager.hpp"
#include "core/DebugLog.hpp"
#include <atomic>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace bluetooth {

#ifdef SWITCHU_MENU

namespace {

constexpr s32 kMaxDiscovered = 15;
constexpr s32 kMaxPaired = 10;

Event g_connectionEvent;
UEvent g_stopEvent; // Finalize() wakes the thread with it (it used to wake twice a second to check)

std::vector<BtmAudioDevice> g_pairedDevices;
std::recursive_mutex g_pairedLock;
std::atomic_bool g_pairedChanged = false;

BtmAudioDevice g_connectedDevice = {};
std::recursive_mutex g_connectedLock;
std::atomic_bool g_connectedChanged = false;

std::vector<BtmAudioDevice> g_discoveredDevices;
std::recursive_mutex g_discoveredLock;
std::atomic_bool g_discoveredChanged = false;

std::atomic_bool g_threadRunning = false;
std::atomic_bool g_discovering = false;
std::atomic_bool g_initialized = false;

Thread g_thread;
alignas(0x1000) u8 g_threadStack[32 * 1024];

bool UpdateList(std::vector<BtmAudioDevice>& dst, const std::vector<BtmAudioDevice>& src, std::atomic_bool& changed) {
    bool prev = changed;
    if (src.size() != dst.size()) {
        changed = true;
    } else if (!dst.empty() && memcmp(dst.data(), src.data(), sizeof(BtmAudioDevice) * dst.size()) != 0) {
        changed = true;
    }
    dst = src;
    return changed != prev;
}

std::string SanitizedDeviceName(const BtmAudioDevice& device) {
    size_t len = 0;
    while (len < sizeof(device.name) && device.name[len] != '\0')
        ++len;

    std::string name(device.name, device.name + len);
    name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char ch) {
        return ch < 0x20 || ch == 0x7F;
    }), name.end());

    if (!name.empty())
        return name;

    char fallback[32] = {};
    std::snprintf(fallback, sizeof(fallback), "%02X:%02X:%02X:%02X:%02X:%02X",
                  device.addr.address[0], device.addr.address[1], device.addr.address[2],
                  device.addr.address[3], device.addr.address[4], device.addr.address[5]);
    return fallback;
}

void ReloadConnected() {
    std::lock_guard<std::recursive_mutex> lk(g_connectedLock);
    BtdrvAddress prevAddr = g_connectedDevice.addr;
    g_connectedDevice = {};

    s32 count = 0;
    Result rc = btmsysGetConnectedAudioDevices(&g_connectedDevice, 1, &count);
    if (R_FAILED(rc)) {
        DebugLog::log("[bt] GetConnectedAudioDevices failed: 0x%x", rc);
        return;
    }
    if (!AddressesEqual(g_connectedDevice.addr, prevAddr)) {
        g_connectedChanged = true;
        if (count > 0)
            DebugLog::log("[bt] Connected: %s", g_connectedDevice.name);
        else
            DebugLog::log("[bt] No connected audio device");
    }
}

void ReloadPaired() {
    std::lock_guard<std::recursive_mutex> lk(g_pairedLock);
    BtmAudioDevice buf[kMaxPaired] = {};
    s32 count = 0;
    Result rc = btmsysGetPairedAudioDevices(buf, kMaxPaired, &count);
    if (R_FAILED(rc)) {
        DebugLog::log("[bt] GetPairedAudioDevices failed: 0x%x", rc);
        return;
    }
    std::vector<BtmAudioDevice> list(buf, buf + count);
    if (UpdateList(g_pairedDevices, list, g_pairedChanged))
        DebugLog::log("[bt] Paired devices changed (%d)", count);
}

void ReloadDiscovered() {
    std::lock_guard<std::recursive_mutex> lk(g_discoveredLock);
    BtmAudioDevice buf[kMaxDiscovered] = {};
    s32 count = 0;
    Result rc = btmsysGetDiscoveredAudioDevice(buf, kMaxDiscovered, &count);
    if (R_FAILED(rc)) {
        DebugLog::log("[bt] GetDiscoveredAudioDevice failed: 0x%x", rc);
        return;
    }
    std::vector<BtmAudioDevice> list(buf, buf + count);
    if (UpdateList(g_discoveredDevices, list, g_discoveredChanged))
        DebugLog::log("[bt] Discovered devices changed (%d)", count);
}

void ThreadFunc(void*) {
    DebugLog::log("[bt] Thread started");
    Result rc = btmsysAcquireAudioDeviceConnectionEvent(&g_connectionEvent);
    if (R_FAILED(rc)) {
        DebugLog::log("[bt] AcquireAudioDeviceConnectionEvent failed: 0x%x", rc);
        return;
    }

    while (g_threadRunning) {
        s32 which = -1;
        rc = waitMulti(&which, UINT64_MAX, waiterForEvent(&g_connectionEvent), waiterForUEvent(&g_stopEvent));
        if (R_SUCCEEDED(rc) && which == 1)
            break; // Finalize()
        if (R_FAILED(rc) && rc != KERNELRESULT(TimedOut)) {
            // Only back off on a real error: an unconditional 1 ms sleep here
            // woke this thread 1000 times a second for the whole session, which
            // kept a core busy and defeated console sleep.
            DebugLog::log("[bt] eventWait error: 0x%x", rc);
            svcSleepThread(100'000'000);
            continue;
        }
        if (R_FAILED(rc))
            continue; // timed out: nothing changed
        ReloadConnected();
        ReloadPaired();
        ReloadDiscovered();
        // The service raises the event once per change; a short debounce keeps a
        // burst (pair + connect + profile) from reading the state repeatedly.
        svcSleepThread(50'000'000);
    }

    DebugLog::log("[bt] Thread exiting");
    eventClose(&g_connectionEvent);
}

} // anonymous namespace

void Initialize() {
    if (g_initialized) return;

    Result rc = btmsysInitialize();
    if (R_FAILED(rc)) {
        DebugLog::log("[bt] btmsysInitialize failed: 0x%x", rc);
        return;
    }

    ReloadPaired();
    ReloadConnected();

    g_threadRunning = true;
    ueventCreate(&g_stopEvent, false);
    rc = threadCreate(&g_thread, ThreadFunc, nullptr, g_threadStack, sizeof(g_threadStack), 0x2C, -2);
    if (R_FAILED(rc)) {
        DebugLog::log("[bt] threadCreate failed: 0x%x", rc);
        btmsysExit();
        return;
    }
    rc = threadStart(&g_thread);
    if (R_FAILED(rc)) {
        DebugLog::log("[bt] threadStart failed: 0x%x", rc);
        threadClose(&g_thread);
        btmsysExit();
        return;
    }

    g_initialized = true;
    DebugLog::log("[bt] Initialized");
}

void Finalize() {
    if (!g_initialized) return;

    if (g_discovering) StopDiscovery();

    g_threadRunning = false;
    ueventSignal(&g_stopEvent);
    threadWaitForExit(&g_thread);
    threadClose(&g_thread);
    btmsysExit();
    g_initialized = false;
    DebugLog::log("[bt] Finalized");
}

bool IsAvailable() { return g_initialized; }

std::vector<BtmAudioDevice> ListPairedAudioDevices() {
    std::lock_guard<std::recursive_mutex> lk(g_pairedLock);
    return g_pairedDevices;
}

bool HasPairedChanges() {
    bool v = g_pairedChanged;
    if (v) g_pairedChanged = false;
    return v;
}

BtmAudioDevice GetConnectedAudioDevice() {
    std::lock_guard<std::recursive_mutex> lk(g_connectedLock);
    return g_connectedDevice;
}

bool HasConnectedChanges() {
    bool v = g_connectedChanged;
    if (v) g_connectedChanged = false;
    return v;
}

std::vector<BtmAudioDevice> ListDiscoveredAudioDevices() {
    std::lock_guard<std::recursive_mutex> lk(g_discoveredLock);
    return g_discoveredDevices;
}

bool HasDiscoveredChanges() {
    bool v = g_discoveredChanged;
    if (v) g_discoveredChanged = false;
    return v;
}

Result ConnectAudioDevice(const BtmAudioDevice& device) {
    g_connectedChanged = true;
    g_discoveredChanged = true;
    g_pairedChanged = true;
    return btmsysConnectAudioDevice(device.addr);
}

Result DisconnectAudioDevice(const BtmAudioDevice& device) {
    {
        std::lock_guard<std::recursive_mutex> lk(g_connectedLock);
        g_connectedDevice = {};
    }
    g_connectedChanged = true;
    return btmsysDisconnectAudioDevice(device.addr);
}

Result UnpairAudioDevice(const BtmAudioDevice& device) {
    g_pairedChanged = true;
    g_discoveredChanged = true;
    return btmsysRemoveAudioDevicePairing(device.addr);
}

std::string DeviceName(const BtmAudioDevice& device) {
    return SanitizedDeviceName(device);
}

void StartDiscovery() {
    Result rc = btmsysStartAudioDeviceDiscovery();
    if (R_FAILED(rc))
        DebugLog::log("[bt] StartDiscovery failed: 0x%x", rc);
    else {
        g_discovering = true;
        g_discoveredChanged = true;
        DebugLog::log("[bt] Discovery started");
    }
}

void StopDiscovery() {
    Result rc = btmsysStopAudioDeviceDiscovery();
    if (R_FAILED(rc))
        DebugLog::log("[bt] StopDiscovery failed: 0x%x", rc);
    else
        DebugLog::log("[bt] Discovery stopped");
    g_discovering = false;
    g_discoveredChanged = true;
}

bool IsDiscovering() { return g_discovering; }

#else

void Initialize() {}
void Finalize() {}
bool IsAvailable() { return false; }
std::vector<BtmAudioDevice> ListPairedAudioDevices() { return {}; }
bool HasPairedChanges() { return false; }
BtmAudioDevice GetConnectedAudioDevice() { return {}; }
bool HasConnectedChanges() { return false; }
std::vector<BtmAudioDevice> ListDiscoveredAudioDevices() { return {}; }
bool HasDiscoveredChanges() { return false; }
Result ConnectAudioDevice(const BtmAudioDevice&) { return 0; }
Result DisconnectAudioDevice(const BtmAudioDevice&) { return 0; }
Result UnpairAudioDevice(const BtmAudioDevice&) { return 0; }
std::string DeviceName(const BtmAudioDevice&) { return {}; }
void StartDiscovery() {}
void StopDiscovery() {}
bool IsDiscovering() { return false; }

#endif

}
