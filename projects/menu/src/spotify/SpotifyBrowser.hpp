#pragma once
#include "spotify/SpotifyPlayer.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// What the Spotify panel shows and plays. Like Discord's panel, HOME asks
// Spotify itself (the Web API, with the Music panel's sign-in) for Home, the
// playlists, the library and search, covers included. A playlist's or album's
// songs, and playing them, go through the background player (it can read any
// playlist, which the Web API no longer lets small apps do): it opens the
// collection by its URI as a page, and plays an item of that page.
// Its own thread; the panel polls state() when version() changes.
class SpotifyBrowser {
public:
    // GET a path under the Web API's /v1; the HTTP status (0: no network or sign-in).
    using WebGet = std::function<long(const std::string& path, std::string& out)>;

    enum class Kind { Track, Playlist, Album, Artist, Show, Liked };
    struct Item {
        Kind kind = Kind::Track;
        std::string uri, name, subtitle, detail, imageUrl;
        std::uint32_t durationMs = 0;
        // A song read from the player's page: that page and its place on it.
        // A song from the Web API: its album, to play it from there.
        std::string pageUri;
        std::uint32_t page = 0, index = 0;
        bool fromPage = false;
        bool isTrack() const { return kind == Kind::Track; }
    };
    struct Shelf {
        std::string title;
        std::vector<Item> items;
    };
    enum class Load { None, Loading, Ready, Failed };
    struct Collection {
        Item of;
        std::vector<Item> items;
        Load load = Load::None;  // Loading until its last page is read
        std::string error;
    };
    struct State {
        Load home = Load::None;
        std::vector<Item> tiles;      // Home's top grid: what you played lately, then your playlists
        std::vector<Shelf> shelves;
        std::vector<Item> playlists;  // Liked Songs, then yours
        std::string homeError;
        Load library = Load::None;
        std::vector<Item> saved;      // Your Library (the left column): playlists, albums, artists
        std::string libraryError;
        Load search = Load::None;
        std::string query;
        std::vector<Shelf> results;
        std::string searchError;
        Collection collection;
        std::string playError;
        std::string account;          // the signed-in account's name (/me)
        std::string accountImage;     // its picture, when it has one
    };

    // The words on screen, in CardsNX's language (its lang.tsv): set once by HOME.
    using Translate = std::function<std::string(const std::string& english)>;
    static void setTranslate(Translate fn);
    static std::string text(const char* english);

    SpotifyBrowser(SpotifyPlayer& service, WebGet get) : m_service(service), m_get(std::move(get)) {}
    ~SpotifyBrowser() { stop(); }
    void start();
    void stop();  // drops queued work; what was read stays for next time

    void loadHome(bool force = false);
    void loadLibrary(bool force = false);
    void loadAccount();  // its name, once
    void reset();        // another account: forget everything read
    void search(const std::string& query);
    void open(const Item& collection);
    void play(const Item& track);
    void playCollection(const Item& collection);  // from its first song

    std::uint64_t version() const { return m_version.load(); }
    State state() const;

private:
    struct Page {
        std::uint64_t generation = 0;
        std::vector<Item> items;  // the songs (and albums) on it
        bool hasNext = false;
        std::uint32_t nextIndex = 0;  // the "Next ..." entry
    };

    void run();
    void post(bool urgent, std::function<void()> job);
    template <class F> void publish(F&& change);

    // Worker only; they throw std::runtime_error.
    std::string web(const std::string& path);
    PlayerIpcBrowseHeader header();
    void request(std::uint32_t command, std::uint64_t generation, std::uint32_t index, const std::string& query = {});
    void readPage(const PlayerIpcBrowseHeader& h, const std::string& uri, std::uint32_t page, Page& out);
    void waitPage(std::uint64_t after, const std::string& uri, std::uint32_t page, Page& out);
    void waitReady();
    void ensure(const std::string& uri, std::uint32_t page);
    void collectionPage(std::uint64_t serial, Item of, std::uint32_t page);
    void playOnPage(const Item& track);

    SpotifyPlayer& m_service;
    WebGet m_get;
    std::thread m_thread;
    mutable std::mutex m_mu;
    std::condition_variable m_cv;
    std::deque<std::function<void()>> m_jobs;
    std::atomic<bool> m_running{false};
    std::atomic<std::uint64_t> m_version{0};
    std::atomic<std::uint64_t> m_collectionSerial{0};
    State m_state;  // under m_mu
    bool m_accountAsked = false;  // under m_mu

    // Worker only: the player's page HOME last opened.
    struct Cursor {
        bool valid = false;
        std::string uri;
        std::uint32_t page = 0;
        Page data;
    } m_cursor;
    bool m_artwork = false;
};
