#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace cards {

template <typename Game>
std::vector<Game> presentGameCards(const std::vector<Game>& page, const std::vector<Game>& inserted) {
    if (inserted.empty()) return page;
    std::vector<Game> result;
    result.reserve(page.size() + inserted.size());
    for (const Game& title : inserted) {
        if (std::any_of(result.begin(), result.end(), [&](const Game& game) { return game.id == title.id; })) continue;
        const auto existing = std::find_if(page.begin(), page.end(), [&](const Game& game) {
            return game.id == title.id;
        });
        result.push_back(existing == page.end() ? title : *existing);
    }
    for (const Game& game : page)
        if (std::none_of(inserted.begin(), inserted.end(), [&](const Game& title) { return game.id == title.id; }))
            result.push_back(game);
    return result;
}

template <typename Game>
std::vector<Game> presentGameCard(const std::vector<Game>& page, const Game* inserted) {
    return presentGameCards(page, inserted ? std::vector<Game>{*inserted} : std::vector<Game>{});
}

template <typename Game>
int gameCardSelection(const std::vector<Game>& page, std::uint64_t id, int fallback) {
    for (std::size_t index = 0; index < page.size(); ++index)
        if (page[index].id == id) return static_cast<int>(index);
    return std::clamp(fallback, 0, std::max(0, static_cast<int>(page.size()) - 1));
}

} // namespace cards