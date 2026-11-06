#include "spotify/SpotifyBrowser.hpp"

#include <nlohmann/json.hpp>
#include <switch.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <unordered_set>

namespace {

using Json = nlohmann::json;
using Item = SpotifyBrowser::Item;
using Kind = SpotifyBrowser::Kind;

constexpr std::uint64_t kPageTimeoutMs = 20000;
constexpr std::uint64_t kConnectWaitMs = 8000;
constexpr std::uint32_t kCollectionMaxPages = 20; // 500 songs
constexpr const char* kLikedUri = "spotify:collection:tracks";

// The player's IPC: 1.3 gives covers (command 38), 1.4 opens a URI (39);
// both appended after this copy of its ABI (1.2).
constexpr std::uint32_t kApiWithArtwork = (1u << 16) | 3u;
constexpr std::uint32_t kApiWithOpenUri = (1u << 16) | 4u;
constexpr std::uint32_t kGetBrowseItemArtwork = 38;
constexpr std::uint32_t kBrowseOpenUri = 39;
struct BrowseArtwork {
    std::uint32_t struct_size;
    std::uint32_t abi_version;
    std::uint64_t generation;
    std::uint32_t index;
    std::uint32_t reserved;
    char url[192];
};
static_assert(sizeof(BrowseArtwork) == 216, "Spotify player artwork ABI changed");

std::string tr(const char* english) { return SpotifyBrowser::text(english); }

std::string fixed(const char* s, std::size_t n) {
    std::size_t len = 0;
    while (len < n && s[len]) ++len;
    return std::string(s, len);
}

bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

std::string enc(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string str(const Json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// The picture nearest 300 px (Spotify lists 640, 300 and 64).
std::string image(const Json& j) {
    const auto it = j.find("images");
    if (it == j.end() || !it->is_array() || it->empty()) return {};
    std::string best;
    int bestDiff = 1 << 30;
    for (const auto& im : *it) {
        if (!im.is_object()) continue;
        const int w = im.contains("width") && im["width"].is_number() ? im["width"].get<int>() : 300;
        const int diff = std::abs(w - 300);
        if (diff < bestDiff) {
            bestDiff = diff;
            best = str(im, "url");
        }
    }
    return best;
}

std::string artists(const Json& j) {
    std::string out;
    const auto it = j.find("artists");
    if (it == j.end() || !it->is_array()) return out;
    for (const auto& a : *it) {
        if (!a.is_object()) continue;
        if (!out.empty()) out += ", ";
        out += str(a, "name");
    }
    return out;
}

Item playlistItem(const Json& j) {
    Item i;
    i.kind = Kind::Playlist;
    i.uri = str(j, "uri");
    i.name = str(j, "name");
    if (j.contains("owner") && j["owner"].is_object()) i.subtitle = str(j["owner"], "display_name");
    i.imageUrl = image(j);
    return i;
}

Item albumItem(const Json& j) {
    Item i;
    i.kind = Kind::Album;
    i.uri = str(j, "uri");
    i.name = str(j, "name");
    i.subtitle = artists(j);
    i.imageUrl = image(j);
    return i;
}

Item artistItem(const Json& j) {
    Item i;
    i.kind = Kind::Artist;
    i.uri = str(j, "uri");
    i.name = str(j, "name");
    i.subtitle = tr("Artist");
    i.imageUrl = image(j);
    return i;
}

Item trackItem(const Json& j) {
    Item i;
    i.kind = Kind::Track;
    i.uri = str(j, "uri");
    i.name = str(j, "name");
    i.subtitle = artists(j);
    if (j.contains("duration_ms") && j["duration_ms"].is_number()) i.durationMs = j["duration_ms"].get<std::uint32_t>();
    if (j.contains("album") && j["album"].is_object()) {
        i.detail = str(j["album"], "name");
        i.imageUrl = image(j["album"]);
        i.pageUri = str(j["album"], "uri"); // played from its album
    }
    return i;
}

Item likedItem() {
    Item i;
    i.kind = Kind::Liked;
    i.uri = kLikedUri;
    i.name = tr("Liked Songs");
    i.subtitle = tr("Playlist");
    return i;
}

// The "Next ..." / "Previous ..." entries the player adds around long lists.
bool isPaging(const PlayerIpcBrowseItem& it) {
    if (it.kind != PlayerBrowseItemKind_Section) return false;
    const std::string n = fixed(it.name, sizeof(it.name));
    return startsWith(n, "Next ") || startsWith(n, "Previous ");
}

[[noreturn]] void fail(const char* what, Result rc) {
    char b[96];
    std::snprintf(b, sizeof(b), "%s (0x%X)", what, (unsigned)rc);
    throw std::runtime_error(b);
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

} // namespace

static SpotifyBrowser::Translate g_translate;

void SpotifyBrowser::setTranslate(Translate fn) { g_translate = std::move(fn); }

std::string SpotifyBrowser::text(const char* english) { return g_translate ? g_translate(english) : english; }

void SpotifyBrowser::start() {
    if (m_running.exchange(true)) return;
    m_thread = std::thread([this] { run(); });
}

void SpotifyBrowser::stop() {
    if (!m_running.exchange(false)) return;
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    m_cursor = {};
    publish([](State& s) { // whatever was half done reloads next time
        if (s.home == Load::Loading) s.home = Load::None;
        if (s.library == Load::Loading) s.library = Load::None;
        if (s.search == Load::Loading) s.search = Load::None;
        if (s.collection.load == Load::Loading) s.collection.load = Load::None;
    });
    std::lock_guard<std::mutex> lock(m_mu);
    m_jobs.clear();
}

SpotifyBrowser::State SpotifyBrowser::state() const {
    std::lock_guard<std::mutex> lock(m_mu);
    return m_state;
}

template <class F> void SpotifyBrowser::publish(F&& change) {
    {
        std::lock_guard<std::mutex> lock(m_mu);
        change(m_state);
    }
    ++m_version;
}

void SpotifyBrowser::post(bool urgent, std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lock(m_mu);
        if (urgent) m_jobs.push_front(std::move(job));
        else m_jobs.push_back(std::move(job));
    }
    m_cv.notify_all();
}

void SpotifyBrowser::run() {
    while (m_running.load()) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(m_mu);
            m_cv.wait(lock, [this] { return !m_jobs.empty() || !m_running.load(); });
            if (!m_running.load()) break;
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
        }
        job();
    }
}

// ---------------------------------------------------------------- the Web API

std::string SpotifyBrowser::web(const std::string& path) {
    std::string out;
    const long status = m_get ? m_get(path, out) : 0;
    if (status == 200) return out;
    if (status == 0) throw std::runtime_error(tr("Spotify can't be reached"));
    if (status == 401 || status == 403) throw std::runtime_error(tr("Sign in to Spotify again"));
    throw std::runtime_error("Spotify " + std::to_string(status));
}

void SpotifyBrowser::loadHome(bool force) {
    {
        std::lock_guard<std::mutex> lock(m_mu);
        if (!force && (m_state.home == Load::Loading || m_state.home == Load::Ready)) return;
        m_state.home = Load::Loading;
        m_state.homeError.clear();
    }
    ++m_version;
    post(false, [this] {
        try {
            // Your playlists (the left column too).
            std::vector<Item> mine;
            for (int offset = 0; offset < 200; offset += 50) {
                const Json j = Json::parse(web("/me/playlists?limit=50&offset=" + std::to_string(offset)));
                for (const auto& p : j.value("items", Json::array()))
                    if (p.is_object()) mine.push_back(playlistItem(p));
                if (!j.contains("next") || j["next"].is_null()) break;
            }
            std::vector<Item> column{likedItem()};
            column.insert(column.end(), mine.begin(), mine.end());
            publish([&](State& s) { s.playlists = column; });

            std::vector<Shelf> shelves;
            // What you played lately: its playlists and albums, then its songs.
            Shelf resume{std::string(), {}};
            Shelf recent{tr("Recently played"), {}};
            try {
                const Json j = Json::parse(web("/me/player/recently-played?limit=50"));
                std::vector<std::string> albums, artistIds;
                std::unordered_set<std::string> seen, seenTracks;
                for (const auto& e : j.value("items", Json::array())) {
                    if (!e.is_object() || !e.contains("track") || !e["track"].is_object()) continue;
                    const Json& t = e["track"];
                    if (recent.items.size() < 20 && seenTracks.insert(str(t, "uri")).second)
                        recent.items.push_back(trackItem(t));
                    if (!e.contains("context") || !e["context"].is_object()) continue;
                    const std::string uri = str(e["context"], "uri");
                    if (uri.empty() || seen.size() >= 10 || !seen.insert(uri).second) continue;
                    Item placeholder;
                    placeholder.uri = uri;
                    if (uri.find(":collection") != std::string::npos) {
                        resume.items.push_back(likedItem());
                    } else if (startsWith(uri, "spotify:playlist:")) {
                        auto it = std::find_if(mine.begin(), mine.end(), [&](const Item& p) { return p.uri == uri; });
                        if (it != mine.end()) {
                            resume.items.push_back(*it);
                        } else {
                            try { // someone else's: its name and cover
                                const std::string id = uri.substr(uri.rfind(':') + 1);
                                resume.items.push_back(playlistItem(Json::parse(
                                    web("/playlists/" + id + "?fields=uri,name,images,owner(display_name)"))));
                            } catch (...) {
                            }
                        }
                    } else if (startsWith(uri, "spotify:album:")) {
                        albums.push_back(uri.substr(uri.rfind(':') + 1));
                        resume.items.push_back(placeholder); // (filled below)
                    } else if (startsWith(uri, "spotify:artist:")) {
                        artistIds.push_back(uri.substr(uri.rfind(':') + 1));
                        resume.items.push_back(placeholder);
                    }
                }
                // One request per item (the batch endpoints are gone for recent Client IDs; at most 10).
                auto fill = [&](const char* kind, const std::vector<std::string>& ids, Item (*make)(const Json&)) {
                    for (const auto& id : ids) {
                        try {
                            const Json x = Json::parse(web(std::string("/") + kind + "/" + id));
                            if (!x.is_object()) continue;
                            const Item made = make(x);
                            for (Item& i : resume.items)
                                if (i.uri == made.uri) i = made;
                        } catch (...) {
                        }
                    }
                };
                fill("albums", albums, albumItem);
                fill("artists", artistIds, artistItem);
                resume.items.erase(std::remove_if(resume.items.begin(), resume.items.end(),
                                                  [](const Item& i) { return i.name.empty(); }),
                                   resume.items.end());
            } catch (...) {
            }
            // The grid: what you played lately, topped up with your playlists.
            std::vector<Item> tiles = resume.items;
            for (const Item& p : column) {
                if (tiles.size() >= 8) break;
                if (std::none_of(tiles.begin(), tiles.end(), [&](const Item& t) { return t.uri == p.uri; }))
                    tiles.push_back(p);
            }
            if (tiles.size() > 8) tiles.resize(8);
            if (!recent.items.empty()) shelves.push_back(std::move(recent));
            if (!mine.empty()) shelves.push_back({tr("Your playlists"), mine});
            try {
                Shelf top{tr("Your top artists"), {}};
                const Json j = Json::parse(web("/me/top/artists?limit=12"));
                for (const auto& a : j.value("items", Json::array()))
                    if (a.is_object()) top.items.push_back(artistItem(a));
                if (!top.items.empty()) shelves.push_back(std::move(top));
            } catch (...) {
            }
            try {
                Shelf saved{tr("Your albums"), {}};
                const Json j = Json::parse(web("/me/albums?limit=20"));
                for (const auto& e : j.value("items", Json::array()))
                    if (e.is_object() && e.contains("album") && e["album"].is_object())
                        saved.items.push_back(albumItem(e["album"]));
                if (!saved.items.empty()) shelves.push_back(std::move(saved));
            } catch (...) {
            }
            publish([&](State& s) {
                s.tiles = std::move(tiles);
                s.shelves = std::move(shelves);
                s.home = Load::Ready;
            });
        } catch (const std::exception& e) {
            const std::string msg = e.what();
            publish([&](State& s) {
                s.home = Load::Failed;
                s.homeError = msg;
            });
        }
    });
}

void SpotifyBrowser::loadAccount() {
    {
        std::lock_guard<std::mutex> lock(m_mu);
        if (!m_state.account.empty() || m_accountAsked) return;
        m_accountAsked = true;
    }
    post(false, [this] {
        try {
            const Json j = Json::parse(web("/me"));
            std::string name = str(j, "display_name");
            if (name.empty()) name = str(j, "id");
            std::string image;
            if (j.contains("images") && j["images"].is_array() && !j["images"].empty() && j["images"][0].is_object())
                image = str(j["images"][0], "url");
            publish([&](State& s) {
                s.account = name;
                s.accountImage = image;
            });
        } catch (...) {
            std::lock_guard<std::mutex> lock(m_mu);
            m_accountAsked = false; // (tries again later)
        }
    });
}

void SpotifyBrowser::reset() {
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_state = State{};
        m_accountAsked = false;
        m_jobs.clear();
    }
    ++m_collectionSerial;
    ++m_version;
}

void SpotifyBrowser::loadLibrary(bool force) {
    {
        std::lock_guard<std::mutex> lock(m_mu);
        if (!force && (m_state.library == Load::Loading || m_state.library == Load::Ready)) return;
        m_state.library = Load::Loading;
        m_state.libraryError.clear();
    }
    ++m_version;
    post(false, [this] {
        try {
            std::vector<Item> saved{likedItem()};
            for (int offset = 0; offset < 200; offset += 50) {
                const Json j = Json::parse(web("/me/playlists?limit=50&offset=" + std::to_string(offset)));
                for (const auto& p : j.value("items", Json::array()))
                    if (p.is_object()) saved.push_back(playlistItem(p));
                if (!j.contains("next") || j["next"].is_null()) break;
            }
            try {
                const Json j = Json::parse(web("/me/albums?limit=50"));
                for (const auto& e : j.value("items", Json::array()))
                    if (e.is_object() && e.contains("album") && e["album"].is_object())
                        saved.push_back(albumItem(e["album"]));
            } catch (...) {
            }
            try {
                const Json j = Json::parse(web("/me/following?type=artist&limit=50"));
                if (j.contains("artists") && j["artists"].is_object())
                    for (const auto& a : j["artists"].value("items", Json::array()))
                        if (a.is_object()) saved.push_back(artistItem(a));
            } catch (...) {
            }
            publish([&](State& s) {
                s.saved = std::move(saved);
                s.library = Load::Ready;
            });
        } catch (const std::exception& e) {
            const std::string msg = e.what();
            publish([&](State& s) {
                s.library = Load::Failed;
                s.libraryError = msg;
            });
        }
    });
}

void SpotifyBrowser::search(const std::string& query) {
    publish([&](State& s) {
        s.search = Load::Loading;
        s.query = query;
        s.results.clear();
        s.searchError.clear();
        s.playError.clear();
    });
    post(true, [this, query] {
        try {
            const Json j = Json::parse(web("/search?type=track,artist,album,playlist&limit=10&q=" + enc(query)));
            std::vector<Shelf> shelves;
            auto add = [&](const char* field, const char* title, Item (*make)(const Json&)) {
                if (!j.contains(field) || !j[field].is_object()) return;
                Shelf shelf{tr(title), {}};
                for (const auto& x : j[field].value("items", Json::array()))
                    if (x.is_object()) shelf.items.push_back(make(x)); // (Spotify sends some nulls)
                if (!shelf.items.empty()) shelves.push_back(std::move(shelf));
            };
            add("tracks", "Songs", trackItem);
            add("artists", "Artists", artistItem);
            add("albums", "Albums", albumItem);
            add("playlists", "Playlists", playlistItem);
            publish([&](State& s) {
                if (s.query != query) return; // a newer search
                s.results = std::move(shelves);
                s.search = Load::Ready;
            });
        } catch (const std::exception& e) {
            const std::string msg = e.what();
            publish([&](State& s) {
                if (s.query != query) return;
                s.search = Load::Failed;
                s.searchError = msg;
            });
        }
    });
}

// ---------------------------------------------------------------- the player's pages

PlayerIpcBrowseHeader SpotifyBrowser::header() {
    PlayerIpcBrowseHeader h{};
    Result rc = 0;
    if (!m_service.withSession([&](void* s) {
            rc = serviceDispatchOut(static_cast<Service*>(s), PlayerIpcCommand_GetBrowseHeader, h);
        }))
        throw std::runtime_error(tr("Spotify isn't ready"));
    if (R_FAILED(rc)) fail("Spotify", rc);
    return h;
}

void SpotifyBrowser::request(std::uint32_t command, std::uint64_t generation, std::uint32_t index,
                             const std::string& query) {
    PlayerIpcBrowseRequest req{};
    req.generation = generation;
    req.index = index;
    std::snprintf(req.query, sizeof(req.query), "%s", query.c_str());
    Result rc = 0;
    if (!m_service.withSession([&](void* s) { rc = serviceDispatchIn(static_cast<Service*>(s), command, req); }))
        throw std::runtime_error(tr("Spotify isn't ready"));
    if (R_FAILED(rc)) fail("Spotify", rc);
}

void SpotifyBrowser::readPage(const PlayerIpcBrowseHeader& h, const std::string& uri, std::uint32_t page, Page& out) {
    out = {};
    out.generation = h.generation;
    for (std::uint32_t i = 0; i < h.item_count; ++i) {
        PlayerIpcBrowseItem it{};
        BrowseArtwork art{};
        Result rc = 0, artRc = 1;
        if (!m_service.withSession([&](void* s) {
                Service* srv = static_cast<Service*>(s);
                rc = serviceDispatchInOut(srv, PlayerIpcCommand_GetBrowseItem, i, it);
                if (R_SUCCEEDED(rc) && m_artwork && it.kind == PlayerBrowseItemKind_Track)
                    artRc = serviceDispatchInOut(srv, kGetBrowseItemArtwork, i, art);
            }))
            throw std::runtime_error(tr("Spotify isn't ready"));
        if (R_FAILED(rc)) fail("Spotify", rc);
        if (isPaging(it)) {
            if (startsWith(fixed(it.name, sizeof(it.name)), "Next ")) {
                out.hasNext = true;
                out.nextIndex = i;
            }
            continue;
        }
        if (it.kind != PlayerBrowseItemKind_Track) continue; // (an artist's albums: no URI to open them by)
        Item e;
        e.kind = Kind::Track;
        e.name = fixed(it.name, sizeof(it.name));
        e.subtitle = fixed(it.subtitle, sizeof(it.subtitle));
        e.detail = fixed(it.detail, sizeof(it.detail));
        e.durationMs = it.duration_ms;
        e.pageUri = uri;
        e.page = page;
        e.index = i;
        e.fromPage = true;
        if (R_SUCCEEDED(artRc) && art.generation == h.generation) e.imageUrl = fixed(art.url, sizeof(art.url));
        out.items.push_back(std::move(e));
    }
}

void SpotifyBrowser::waitPage(std::uint64_t after, const std::string& uri, std::uint32_t page, Page& out) {
    const std::uint64_t deadline = SpotifyPlayer::nowMs() + kPageTimeoutMs;
    while (m_running.load() && SpotifyPlayer::nowMs() < deadline) {
        const PlayerIpcBrowseHeader h = header();
        if (h.generation > after) {
            if (h.state == PlayerBrowseState_Error) {
                m_cursor.valid = false;
                const std::string msg = fixed(h.message, sizeof(h.message));
                throw std::runtime_error(msg.empty() ? tr("Spotify didn't answer") : msg);
            }
            if (h.state == PlayerBrowseState_Ready) {
                readPage(h, uri, page, out);
                if (header().generation == h.generation) return;
                after = h.generation; // the overlay moved it while it was read
            }
        }
        sleepMs(40);
    }
    m_cursor.valid = false;
    throw std::runtime_error(m_running.load() ? tr("Spotify didn't answer") : "Stopped");
}

void SpotifyBrowser::waitReady() {
    for (std::uint64_t waited = 0; waited < kConnectWaitMs && m_running.load(); waited += 100) {
        const auto snap = m_service.snapshot();
        if (snap.link == SpotifyPlayer::Link::Connected && snap.status.service_state == PlayerServiceState_Ready) {
            if (snap.apiVersion < kApiWithOpenUri)
                throw std::runtime_error(tr("Spotify's player on this Switch needs an update"));
            m_artwork = snap.apiVersion >= kApiWithArtwork;
            return;
        }
        sleepMs(100);
    }
    throw std::runtime_error(tr("Spotify isn't ready"));
}

// Puts the player's page on `uri`, at `page`: reopened when the overlay moved it.
void SpotifyBrowser::ensure(const std::string& uri, std::uint32_t page) {
    waitReady();
    const bool here = m_cursor.valid && m_cursor.uri == uri && m_cursor.page <= page
                      && header().generation == m_cursor.data.generation;
    if (!here) {
        const std::uint64_t before = header().generation;
        request(kBrowseOpenUri, 0, 0, uri);
        m_cursor = {};
        waitPage(before, uri, 0, m_cursor.data);
        m_cursor.valid = true;
        m_cursor.uri = uri;
    }
    while (m_cursor.page < page) {
        if (!m_cursor.data.hasNext) throw std::runtime_error("No next page");
        const std::uint64_t before = header().generation;
        request(PlayerIpcCommand_BrowseOpen, m_cursor.data.generation, m_cursor.data.nextIndex);
        Page next;
        waitPage(before, uri, m_cursor.page + 1, next);
        m_cursor.data = std::move(next);
        ++m_cursor.page;
    }
}

void SpotifyBrowser::open(const Item& collection) {
    const std::uint64_t serial = ++m_collectionSerial;
    publish([&](State& s) {
        s.collection = {};
        s.collection.of = collection;
        s.collection.load = Load::Loading;
        s.playError.clear();
    });
    post(false, [this, serial, collection] { collectionPage(serial, collection, 0); });
}

// One page a job, so a Play can go between two pages.
void SpotifyBrowser::collectionPage(std::uint64_t serial, Item of, std::uint32_t page) {
    if (serial != m_collectionSerial.load()) return;
    bool more = false;
    std::vector<Item> items;
    std::string error;
    try {
        ensure(of.uri, page);
        items = m_cursor.data.items;
        more = m_cursor.data.hasNext && page + 1 < kCollectionMaxPages;
    } catch (const std::exception& e) {
        error = e.what();
    }
    if (serial != m_collectionSerial.load()) return;
    publish([&](State& s) {
        auto& c = s.collection;
        for (Item& e : items) c.items.push_back(std::move(e));
        c.error = error;
        c.load = !error.empty() ? Load::Failed : more ? Load::Loading : Load::Ready;
    });
    if (more) post(false, [this, serial, of = std::move(of), page] { collectionPage(serial, of, page + 1); });
}

void SpotifyBrowser::playOnPage(const Item& track) {
    if (track.fromPage) {
        ensure(track.pageUri, track.page);
        request(PlayerIpcCommand_BrowsePlay, m_cursor.data.generation, track.index);
        return;
    }
    // A song from search or Home: from its album, found by its name.
    if (track.pageUri.empty()) throw std::runtime_error(tr("This can't be played here"));
    for (std::uint32_t page = 0; page < 4; ++page) {
        ensure(track.pageUri, page);
        for (const Item& e : m_cursor.data.items)
            if (e.name == track.name) {
                request(PlayerIpcCommand_BrowsePlay, m_cursor.data.generation, e.index);
                return;
            }
        if (!m_cursor.data.hasNext) break;
    }
    throw std::runtime_error(tr("This can't be played here"));
}

void SpotifyBrowser::play(const Item& track) {
    post(true, [this, track] {
        std::string error;
        try {
            playOnPage(track);
        } catch (const std::exception& e) {
            error = e.what();
        }
        publish([&](State& s) { s.playError = error; });
    });
}

void SpotifyBrowser::playCollection(const Item& collection) {
    post(true, [this, collection] {
        std::string error;
        try {
            ensure(collection.uri, 0);
            if (m_cursor.data.items.empty()) throw std::runtime_error(tr("Nothing to play here"));
            request(PlayerIpcCommand_BrowsePlay, m_cursor.data.generation, m_cursor.data.items.front().index);
        } catch (const std::exception& e) {
            error = e.what();
        }
        publish([&](State& s) { s.playError = error; });
    });
}
