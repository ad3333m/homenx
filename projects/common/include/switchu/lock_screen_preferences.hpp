#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <string>

namespace switchu::lock_screen {
inline bool enabled() {
    std::string frontend;
    std::getline(std::ifstream("sdmc:/config/SwitchU/frontend.txt"), frontend);
    if (frontend.empty() || frontend.rfind("cards", 0) == 0) {
        std::ifstream in("sdmc:/config/CardsNX/settings.json");
        const auto settings = nlohmann::json::parse(in, nullptr, false);
        if (!settings.is_object()) return false;
        const auto at = settings.find("lockScreen");
        return at != settings.end() && at->is_boolean() && at->get<bool>();
    }
    std::error_code ec;
    return std::filesystem::exists("sdmc:/config/SwitchU/lock_screen", ec);
}
}
