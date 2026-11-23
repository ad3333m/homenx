#pragma once
#include "Switube.hpp"
#include "yt_music_client.hpp"
#include <atomic>
#include <memory>

namespace cards {

// YouTube catalog and stream resolution use the credited Switube client.
// Control and UI state stay with the HomeNX Music worker.
class NativeYoutube {
public:
    explicit NativeYoutube(std::atomic<bool>& cancel) : m_cancel(cancel) {}
    ~NativeYoutube();
    static std::vector<std::uint8_t> fetchCover(const std::string& url, const std::atomic<bool>& cancelled);
    bool handles(Switube::Browse cmd) const;
    Switube::Page browse(Switube::Browse cmd, std::uint64_t generation, unsigned index, const std::string& query);
    void tick();
    std::string takePlaybackError();
    void stop();
    void describeQueue(Switube::Now& now) const;
    bool hasQueue() const { return !m_queue.empty(); }
    bool active() const;
    Switube::Now now() const;
    bool command(Switube::Cmd cmd, int value);
private:
    bool ready();
    bool play(unsigned index);
    int nextIndex() const;
    bool playNext();
    bool download(unsigned index);
    Switube::Page page(std::string message = {}) const;

    std::atomic<bool>& m_cancel;
    switube::YtMusicClient m_client;
    std::vector<switube::YtTrack> m_tracks, m_queue;
    std::shared_ptr<std::atomic<bool>> m_completed = std::make_shared<std::atomic<bool>>(false);
    std::string m_title = "YouTube Music";
    std::string m_error;
    std::uint64_t m_generation = std::uint64_t{1} << 63;
    int m_index = -1;
    int m_volume = 80;
    int m_repeat = 0;
    bool m_initialized = false, m_started = false, m_shuffle = false;
};

} // namespace cards
