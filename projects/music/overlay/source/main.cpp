#include <bit>
#define TESLA_INIT_IMPL
#include <tesla.hpp>
#include <switube/ipc_protocol.h>
#include <algorithm>
#include <cstdio>

namespace {
using Item = tsl::elm::ListItem;
Service service{};

bool browse(u32 command, const std::string &query = {}, u64 generation = 0, u32 index = 0) {
    if (!serviceIsActive(&service))
        return false;
    SwitubeIpcBrowseRequest request{};
    request.generation = generation;
    request.index = index;
    std::snprintf(request.query, sizeof(request.query), "%s", query.c_str());
    return R_SUCCEEDED(serviceDispatchIn(&service, command, request));
}

void showError(std::string &message, const char *action, Result rc) {
    char text[96];
    std::snprintf(text, sizeof(text), "%s (0x%X)", action, rc);
    message = text;
}

class Results : public tsl::Gui {
    std::string query, message;
    tsl::elm::List *list{};
    u64 previous = 0, lastPoll = 0;
    bool waiting = false;
    bool downloads = false;
    bool playlists = false;
    std::string pageTitle;
    std::string requestedTitle;

  public:
    explicit Results(std::string text, bool local = false, bool saved = false) : query(std::move(text)), downloads(local), playlists(saved) {}
    tsl::elm::Element *createUI() override {
        auto *frame = new tsl::elm::HeaderOverlayFrame(115);
        frame->setHeader(new tsl::elm::CustomDrawer([this](auto *r, int x, int y, int w, int) {
            r->drawString(!pageTitle.empty() ? pageTitle.c_str() : playlists ? "Playlists" : downloads ? "Downloads" : query.empty() ? "Discover" : query.c_str(), false, x + 24, y + 50, 29, 0xFFFF,
                          w - 48);
            r->drawString(message.c_str(), false, x + 24, y + 86, 17, 0xFFCA, w - 48);
        }));
        list = new tsl::elm::List();
        frame->setContent(list);
        SwitubeIpcBrowseHeader page{};
        if (serviceIsActive(&service) &&
            R_SUCCEEDED(serviceDispatchOut(&service, SwitubeIpcCommand_GetBrowseHeader, page))) {
            previous = page.generation;
            waiting =
                browse(playlists ? SwitubeIpcCommand_ListPlaylists : downloads ? SwitubeIpcCommand_BrowseLibrary : query.empty() ? SwitubeIpcCommand_BrowseHome : SwitubeIpcCommand_BrowseSearch, query);
        }
        message = waiting ? "Finding songs…" : "Music service unavailable. Go back to reconnect.";
        return frame;
    }
    void update() override {
        auto now = armGetSystemTick();
        if ((!waiting && requestedTitle.empty()) || armTicksToNs(now - lastPoll) < 250000000)
            return;
        lastPoll = now;
        SwitubeIpcBrowseHeader page{};
        Result rc = serviceDispatchOut(&service, SwitubeIpcCommand_GetBrowseHeader, page);
        if (R_FAILED(rc)) {
            waiting = false;
            showError(message, "Music service disconnected", rc);
            return;
        }
        if (!waiting) {
            SwitubeIpcStatus current{};
            if (page.state == SwitubeBrowseState_Error) {
                message = page.message;
                requestedTitle.clear();
            } else if (R_SUCCEEDED(serviceDispatchOut(&service, SwitubeIpcCommand_GetStatus, current)) &&
                       current.playback_state == SwitubePlaybackState_Playing &&
                       requestedTitle.compare(0, std::strlen(current.title), current.title) == 0 &&
                       current.title[0]) {
                message = "Playing: " + requestedTitle;
                requestedTitle.clear();
            }
            return;
        }
        if (page.generation == previous || page.state == SwitubeBrowseState_Loading)
            return;
        waiting = false;
        if (page.state == SwitubeBrowseState_Error) {
            message = page.message;
            return;
        }
        if (playlists) pageTitle = page.title;
        message = playlists && pageTitle == "Playlists" ? "Select a playlist" : "Select a song to play";
        for (u32 i = 0; i < std::min(page.item_count, 32u); ++i) {
            SwitubeIpcBrowseItem item{};
            rc = serviceDispatchInOut(&service, SwitubeIpcCommand_GetBrowseItem, i, item);
            if (R_FAILED(rc) || item.generation != page.generation)
                break;
            auto *row = new Item(item.name, item.subtitle);
            if (item.kind != SwitubeBrowseItemKind_Track) {
                row->setClickListener([this, gen=page.generation, i](u64 keys) {
                    if (!(keys & HidNpadButton_A)) return false;
                    if (browse(SwitubeIpcCommand_BrowseOpen, "", gen, i)) {
                        removeFocus();
                        list->clear();
                        previous = gen;
                        waiting = true;
                        requestedTitle.clear();
                    }
                    return true;
                });
                list->addItem(row);
                continue;
            }
            row->setClickListener([this, gen = page.generation, i, title = std::string(item.name)](u64 keys) {
                if (!(keys & HidNpadButton_A))
                    return false;
                requestedTitle = title;
                message = browse(SwitubeIpcCommand_BrowsePlay, "", gen, i) ? "Opening stream…"
                                                                           : "Play request failed";
                return true;
            });
            list->addItem(row);
        }
    }
};

constexpr int KeyColumns = 10;
const char *const keyLabels[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0",     "q",   "w",   "e", "r",
                                 "t", "y", "u", "i", "o", "p", "a", "s", "d", "f",     "g",   "h",   "j", "k",
                                 "l", "-", "z", "x", "c", "v", "b", "n", "m", "Space", "Del", "Find"};
class Keyboard : public tsl::elm::Element {
    std::string query;
    int selected = 0;
    void selectKey() {
        const std::string key = keyLabels[selected];
        if (key == "Find")
            submit();
        else if (key == "Del")
            erase();
        else if (query.size() < 100)
            query += key == "Space" ? " " : key;
    }

  public:
    void erase() {
        if (!query.empty())
            query.pop_back();
    }
    void submit() {
        if (!query.empty())
            tsl::changeTo<Results>(query);
    }
    bool input(u64 keys) {
        if (keys & HidNpadButton_Plus)
            submit();
        else if (keys & HidNpadButton_X)
            erase();
        else if (keys & HidNpadButton_A)
            selectKey();
        else if (keys & HidNpadButton_AnyRight)
            selected = selected / 10 * 10 + (selected + 1) % 10;
        else if (keys & HidNpadButton_AnyLeft)
            selected = selected / 10 * 10 + (selected + 9) % 10;
        else if (keys & HidNpadButton_AnyDown)
            selected = (selected + 10) % 40;
        else if (keys & HidNpadButton_AnyUp)
            selected = (selected + 30) % 40;
        else
            return false;
        return true;
    }
    void layout(u16, u16, u16, u16) override {}
    void draw(tsl::gfx::Renderer *r) override {
        int x = getX(), y = getY(), width = getWidth(), cell = width / KeyColumns;
        r->drawString(query.empty() ? "Artist or song" : query.c_str(), false, x, y + 32, 24, 0xFFFF, width);
        r->drawRect(x, y + 47, width, 2, 0xFFBA);
        for (int i = 0; i < 40; ++i) {
            int left = x + i % 10 * cell, top = y + 76 + i / 10 * 56;
            r->drawRect(left, top, cell - 3, 51, i == selected ? 0xF875 : 0xF333);
            r->drawString(keyLabels[i], false, left + 4, top + 31, i >= 37 ? 11 : 21, 0xFFFF, cell - 6);
        }
        r->drawString("X  Delete       +  Find songs", false, x, y + 350, 17, 0xFFCA, width);
    }
    bool onTouch(tsl::elm::TouchEvent event, s32 x, s32 y, s32, s32, s32, s32) override {
        if (event != tsl::elm::TouchEvent::Release)
            return false;
        int col = x - getX(), row = y - getY() - 76, cell = getWidth() / 10;
        if (col < 0 || col >= cell * 10 || row < 0 || row >= 224)
            return false;
        selected = row / 56 * 10 + col / cell;
        selectKey();
        return true;
    }
};
class Search : public tsl::Gui {
    Keyboard *keyboard{};
    tsl::elm::Element *createUI() override {
        auto *frame = new tsl::elm::OverlayFrame("Search", "YouTube Music");
        keyboard = new Keyboard();
        frame->setContent(keyboard);
        return frame;
    }
    bool handleInput(u64 keys, u64, const HidTouchState &, HidAnalogStickState,
                     HidAnalogStickState) override {
        return keyboard->input(keys);
    }
};

class Player : public tsl::Gui {
    SwitubeIpcStatus status{};
    std::string connection = "Connecting…";
    tsl::elm::TrackBar *volume{};
    Item *play{};
    int pendingVolume = -1;
    u64 lastPoll = 0;
    bool checkedInstall = false;
    bool installed = false;
    u64 launchTick = 0;

    void checkInstall() {
        checkedInstall = true;
        FsFileSystem sd{};
        Result rc = fsOpenSdCardFileSystem(&sd);
        if (R_FAILED(rc)) { showError(connection, "Read SD card", rc); return; }
        FsDirEntryType type{};
        rc = fsFsGetEntryType(&sd, "/atmosphere/contents/4200000000484D59/exefs.nsp", &type);
        if (R_FAILED(rc) || type != FsDirEntryType_File) {
            showError(connection, "Check music exefs.nsp", rc);
        } else {
            installed = true;
            connection = "Press X to start the music module.";
        }
        fsFsClose(&sd);
    }

    void startInstalledModule() {
        if (serviceIsActive(&service)) return;
        installed = false;
        checkInstall();
        if (!installed) return;
        Result rc = pmdmntInitialize();
        if (R_SUCCEEDED(rc)) {
            u64 pid;
            rc = pmdmntGetProcessId(&pid, HOMENX_MUSIC_PROGRAM_ID);
            pmdmntExit();
            if (R_SUCCEEDED(rc)) {
                connection = "Process running; waiting for music IPC.";
                launchTick = armGetSystemTick();
                return;
            }
        }
        rc = pmshellInitialize();
        if (R_FAILED(rc)) { showError(connection, "Open process manager", rc); return; }
        NcmProgramLocation location{HOMENX_MUSIC_PROGRAM_ID, NcmStorageId_None, {}};
        u64 pid;
        rc = pmshellLaunchProgram(0, &location, &pid);
        pmshellExit();
        if (rc == KERNELRESULT(OutOfResource) && R_SUCCEEDED(pmdmntInitialize())) {
            // Atmosphère pm:dmnt reports usage for the NPDM's applet resource group.
            struct { u32 group, resource; } in{2, LimitableResource_Memory};
            struct { u64 used, limit; } memory{}, threads{};
            Result memRc = serviceDispatchInOut(pmdmntGetServiceSession(), 65001, in, memory);
            in.resource = LimitableResource_Threads;
            Result threadRc = serviceDispatchInOut(pmdmntGetServiceSession(), 65001, in, threads);
            pmdmntExit();
            if (R_SUCCEEDED(memRc) && R_SUCCEEDED(threadRc)) {
                char detail[80];
                std::snprintf(detail, sizeof(detail), "0x10801: %lu MiB / %lu threads free",
                              (memory.limit - std::min(memory.limit, memory.used)) >> 20,
                              threads.limit - std::min(threads.limit, threads.used));
                connection = detail;
            } else showError(connection, "Start music module", rc);
        } else if (R_FAILED(rc)) showError(connection, "Start music module", rc);
        else { launchTick = armGetSystemTick(); connection = "Starting music module…"; }
    }

    void control(u32 command) {
        if (!serviceIsActive(&service))
            return;
        auto rc = serviceDispatch(&service, command);
        if (R_FAILED(rc))
            showError(connection, "Playback request failed", rc);
    }
    void drawHeader(tsl::gfx::Renderer *r, int x, int y, int w) {
        const int cx = x + 67, cy = y + 97;
        r->drawRect(x, y, w, 216, 0xF322);
        r->drawCircle(cx, cy, 46, true, 0xF111);
        for (int radius : {29, 34, 40})
            r->drawCircle(cx, cy, radius, false, 0xF444);
        r->drawCircle(cx, cy, 15, true, 0xFA7F);
        r->drawCircle(cx, cy, 4, true, 0xF111);
        r->drawString("YouTube Music", false, x + 22, y + 30, 17, 0xFFCA);
        r->drawString(status.title[0] ? status.title : "HomeNX Music", false, x + 130, y + 93, 24, 0xFFFF,
                      w - 152);
        r->drawString(status.artist, false, x + 130, y + 122, 17, 0xFCCC, w - 152);
        const int length = w - 48;
        r->drawRect(x + 24, y + 157, length, 4, 0xF555);
        if (status.duration_ms) {
            int filled = static_cast<u64>(std::min(status.position_ms, status.duration_ms)) * length /
                         status.duration_ms;
            r->drawRect(x + 24, y + 157, filled, 4, 0xFA7F);
        }
        char elapsed[48];
        unsigned sec = status.position_ms / 1000, total = status.duration_ms / 1000;
        std::snprintf(elapsed, sizeof(elapsed), "%u:%02u / %u:%02u", sec / 60, sec % 60, total / 60,
                      total % 60);
        r->drawString(connection.empty() ? elapsed : connection.c_str(), false, x + 24, y + 191, 16, 0xFDDD,
                      length);
    }

  public:
    tsl::elm::Element *createUI() override {
        auto *frame = new tsl::elm::HeaderOverlayFrame(216);
        frame->setHeader(new tsl::elm::CustomDrawer(
            [this](auto *r, int x, int y, int w, int) { drawHeader(r, x, y, w); }));
        auto *list = new tsl::elm::List();
        auto row = [list](const char *title, auto action) {
            auto *item = new Item(title);
            item->setClickListener([action](u64 keys) {
                if (!(keys & HidNpadButton_A))
                    return false;
                action();
                return true;
            });
            list->addItem(item);
            return item;
        };
        play = row("Play / pause", [this] { control(SwitubeIpcCommand_PlayPause); });
        play->setValue("Y");
        list->addItem(new tsl::elm::CategoryHeader("L  Previous     R  Next     •     Music volume"));
        volume = new tsl::elm::TrackBar("♪");
        volume->setValueChangedListener([this](u8 value) { pendingVolume = value; });
        list->addItem(volume);
        row("Find a song", [] { tsl::changeTo<Search>(); });
        row("Playlists", [] { tsl::changeTo<Results>(std::string{}, false, true); });
        row("Downloads", [] { tsl::changeTo<Results>(std::string{}, true); });
        row("Discover songs", [] { tsl::changeTo<Results>(std::string{}); });
        frame->setContent(list);
        return frame;
    }
    bool handleInput(u64 keys, u64, const HidTouchState &, HidAnalogStickState,
                     HidAnalogStickState) override {
        if ((keys & HidNpadButton_X) && !serviceIsActive(&service))
            startInstalledModule();
        else if (keys & HidNpadButton_L)
            control(SwitubeIpcCommand_Previous);
        else if (keys & HidNpadButton_R)
            control(SwitubeIpcCommand_Next);
        else if (keys & HidNpadButton_Y)
            control(SwitubeIpcCommand_PlayPause);
        else
            return false;
        return true;
    }
    void update() override {
        auto now = armGetSystemTick();
        if (armTicksToNs(now - lastPoll) < 250000000)
            return;
        lastPoll = now;
        if (!serviceIsActive(&service)) {
            auto name = smEncodeName(HOMENX_MUSIC_SERVICE);
            u8 registered = 0;
            Result rc = tipcDispatchInOut(smGetServiceSessionTipc(), 65100, name, registered);
            if (R_FAILED(rc)) {
                showError(connection, "Check music service", rc);
                return;
            }
            if (!registered) {
                if (!checkedInstall) checkInstall();
                if (launchTick && armTicksToNs(now-launchTick) > 8000000000ULL) {
                    launchTick = 0;
                    u64 pid;
                    Result probe = pmdmntInitialize();
                    if (R_SUCCEEDED(probe)) {
                        probe = pmdmntGetProcessId(&pid, HOMENX_MUSIC_PROGRAM_ID);
                        pmdmntExit();
                        connection = R_SUCCEEDED(probe) ? "Process running; music IPC unavailable."
                                                        : "Process exited before music IPC.";
                    } else showError(connection, "Query music process", probe);
                }
                return;
            }
            checkedInstall = false;
            launchTick = 0;
            rc = smGetService(&service, HOMENX_MUSIC_SERVICE);
            if (R_FAILED(rc)) {
                showError(connection, "Connect to music service", rc);
                return;
            }
        }
        if (pendingVolume >= 0) {
            u32 value = pendingVolume * 10;
            if (R_SUCCEEDED(serviceDispatchIn(&service, SwitubeIpcCommand_SetVolume, value)))
                pendingVolume = -1;
        }
        Result rc = serviceDispatchOut(&service, SwitubeIpcCommand_GetStatus, status);
        if (R_FAILED(rc)) {
            serviceClose(&service);
            status = {};
            showError(connection, "Music service disconnected", rc);
            return;
        }
        connection.clear();
        if (pendingVolume < 0)
            volume->setProgress(std::min(status.volume_permille / 10, 100u));
        play->setText(status.playback_state == SwitubePlaybackState_Playing ? "Pause" : "Play");
        SwitubeIpcBrowseHeader page{};
        if (R_SUCCEEDED(serviceDispatchOut(&service, SwitubeIpcCommand_GetBrowseHeader, page)) &&
            page.state == SwitubeBrowseState_Error)
            connection = page.message;
    }
};
class Overlay : public tsl::Overlay {
    void initServices() override { smInitialize(); }
    void exitServices() override {
        serviceClose(&service);
        smExit();
    }
    std::unique_ptr<tsl::Gui> loadInitialGui() override { return initially<Player>(); }
};
} // namespace
int main(int argc, char **argv) { return tsl::loop<Overlay>(argc, argv); }
