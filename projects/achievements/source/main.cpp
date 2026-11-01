// HomeNX Achievements: a Tesla/Ultrahand overlay. Over a game it shows that
// game's achievements (switchu/achievements.hpp: milestones from its play data)
// and how far along each is, then the ones across every game. HOME (CardsNX)
// keeps what's earned (config/SwitchU/achievements.txt) and says the new ones.
#include <bit> // libtesla uses std::bit_cast without including it

#define TESLA_INIT_IMPL
#include <tesla.hpp>

#include <switchu/achievement_packs.hpp>
#include <switchu/achievements.hpp>
#include <switchu/control_cache.hpp>

#include <cstdio>
#include <string>

namespace {
namespace ach = switchu::achievements;

// The game being played (0: none): the application's process, its program id.
std::uint64_t runningTitle() {
    std::uint64_t pid = 0, tid = 0;
    if (R_FAILED(pmdmntGetApplicationProcessId(&pid))) return 0;
    if (R_FAILED(pminfoGetProgramId(&tid, pid))) return 0;
    return tid;
}

std::string hours(std::uint64_t seconds) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(seconds) / 3600.0);
    return buf;
}

// "Earned", or how far along ("3.2 / 10 h", "4 / 10").
std::string progressText(const ach::Milestone& m, const ach::Play& p, bool earned) {
    if (earned) return "Earned";
    if (m.hours) return hours(p.seconds) + " / " + std::to_string(m.goal) + " h";
    return std::to_string(p.launches) + " / " + std::to_string(m.goal);
}

class AchievementsGui : public tsl::Gui {
public:
    tsl::elm::Element* createUI() override {
        const ach::Earned earned = ach::load();
        const std::uint64_t tid = runningTitle();
        auto* frame = new tsl::elm::OverlayFrame("Achievements", "HomeNX");
        auto* list = new tsl::elm::List();
        if (tid) {
            std::string game = switchu::control_cache::formatTitleId(tid);
            switchu::control_cache::Meta meta;
            if (switchu::control_cache::readMeta(tid, meta)) game = meta.name[0] ? meta.name : meta.english_name;
            ach::Play play;
            PdmPlayStatistics st{};
            if (R_SUCCEEDED(pdmqryQueryPlayStatisticsByApplicationId(tid, false, &st))) {
                play.seconds = st.playtime / 1000000000ULL;
                play.launches = st.total_launches;
            }
            const switchu::packs::Pack pack = switchu::packs::load(tid); // its own (docs/achievement-packs.md)
            auto packDone = [&](const switchu::packs::Achievement& p) {
                return earned.when.count(ach::key(tid, ("p." + p.id).c_str())) != 0;
            };
            int got = 0;
            int count = static_cast<int>(sizeof(ach::kPerGame) / sizeof(ach::kPerGame[0]) + pack.list.size());
            for (const auto& p : pack.list) got += packDone(p) ? 1 : 0;
            for (const ach::Milestone& m : ach::kPerGame)
                got += earned.when.count(ach::key(tid, m.id)) || ach::progress(m, play) >= 1.f ? 1 : 0;
            list->addItem(new tsl::elm::CategoryHeader(game + "  " + std::to_string(got) + " / " + std::to_string(count)));
            for (const auto& p : pack.list) {
                auto* item = new tsl::elm::ListItem(p.name);
                item->setValue(packDone(p) ? "Earned" : "Locked", !packDone(p));
                list->addItem(item);
            }
            for (const ach::Milestone& m : ach::kPerGame) {
                const bool done = earned.when.count(ach::key(tid, m.id)) || ach::progress(m, play) >= 1.f;
                auto* item = new tsl::elm::ListItem(m.name);
                item->setValue(progressText(m, play, done), !done);
                list->addItem(item);
            }
            list->addItem(new tsl::elm::CategoryHeader("Today's play adds up back on HOME", true));
        } else {
            list->addItem(new tsl::elm::CategoryHeader("Open a game to see its achievements"));
        }
        const ach::Play all{earned.totalSeconds, earned.gamesPlayed};
        list->addItem(new tsl::elm::CategoryHeader("All your games  " + hours(all.seconds) + " h", true));
        for (const ach::Milestone& m : ach::kOverall) {
            const bool done = earned.when.count(ach::overallKey(m.id)) || ach::progress(m, all) >= 1.f;
            auto* item = new tsl::elm::ListItem(m.name);
            item->setValue(progressText(m, all, done), !done);
            list->addItem(item);
        }
        frame->setContent(list);
        return frame;
    }
};

class AchievementsOverlay : public tsl::Overlay {
public:
    void initServices() override {
        fsdevMountSdmc();
        m_pmdmnt = R_SUCCEEDED(pmdmntInitialize());
        m_pminfo = R_SUCCEEDED(pminfoInitialize());
        m_pdm = R_SUCCEEDED(pdmqryInitialize());
    }

    void exitServices() override {
        if (m_pdm) pdmqryExit();
        if (m_pminfo) pminfoExit();
        if (m_pmdmnt) pmdmntExit();
        fsdevUnmountAll();
    }

    std::unique_ptr<tsl::Gui> loadInitialGui() override { return initially<AchievementsGui>(); }

private:
    bool m_pmdmnt = false, m_pminfo = false, m_pdm = false;
};

} // namespace

int main(int argc, char** argv) {
    return tsl::loop<AchievementsOverlay>(argc, argv);
}
