#pragma once

#include <filesystem>
#include <fstream>
#include <string_view>
#include <string>
#include <vector>

namespace switchu::boot_modules {

namespace fs = std::filesystem;

struct Known {
    const char* id;
    const char* name;
};

// Only modules with a documented standalone boot2 installation.
inline constexpr Known supported[] = {{"00FF0000636C6BFF", "sys-clk"}};
enum class State { Enabled, Parked, Unknown, Conflict };

struct Module {
    std::string id;
    std::string name;
    fs::path path;
    State state;
    bool manageable;
};

inline int regular(const fs::path& path) {
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory || status.type() == fs::file_type::not_found)
        return 0;
    if (error || !fs::is_regular_file(status)) return -1;
    return 1;
}

inline bool directory(const fs::path& path) {
    std::error_code error;
    return fs::is_directory(fs::symlink_status(path, error)) && !error;
}

inline constexpr const char* activeFlag = "flags/boot2.flag";
inline constexpr const char* parkedFlag = "flags/boot2.homenx-disabled";

inline State state(const fs::path& path) {
    if (!directory(path.parent_path()) || !directory(path) ||
        !directory(path / "flags") || regular(path / "exefs.nsp") != 1 ||
        regular(path / "boot2.flag") != 0)
        return State::Unknown;
    char magic[4]{};
    if (!std::ifstream(path / "exefs.nsp", std::ios::binary).read(magic, sizeof(magic)) ||
        std::string_view(magic, sizeof(magic)) != "PFS0")
        return State::Unknown;
    const int active = regular(path / activeFlag);
    const int parked = regular(path / parkedFlag);
    if (active < 0 || parked < 0 || (active && parked)) return State::Conflict;
    if (active) return State::Enabled;
    if (parked) return State::Parked;
    return State::Unknown;
}

inline std::vector<Module> discover(const fs::path& root, std::error_code& error) {
    std::vector<Module> modules;
    if (!directory(root)) {
        error = std::make_error_code(std::errc::not_a_directory);
        return modules;
    }
    for (fs::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
        const auto path = it->path();
        auto id = path.filename().string();
        if (id.size() != 16 || id.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos ||
            !directory(path))
            continue;
        for (auto& c : id)
            if (c >= 'a' && c <= 'f') c -= 'a' - 'A';
        const Known* known = nullptr;
        for (const auto& entry : supported)
            if (id == entry.id) known = &entry;
        if (!known && regular(path / activeFlag) != 1 && regular(path / "boot2.flag") != 1)
            continue;
        const auto current = state(path);
        modules.push_back({id, known ? known->name : id, path, current, known != nullptr});
    }
    return modules;
}

struct Change {
    Module module;
    State target;
};

inline std::string apply(const std::vector<Change>& changes) {
    const auto eligible = [](State value) { return value == State::Enabled || value == State::Parked; };
    for (const auto& change : changes) {
        bool allowed = false;
        for (const auto& entry : supported)
            if (change.module.id == entry.id) allowed = true;
        auto leaf = change.module.path.filename().string();
        for (auto& c : leaf)
            if (c >= 'a' && c <= 'f') c -= 'a' - 'A';
        if (!allowed || leaf != change.module.id || !change.module.manageable ||
            !eligible(change.target) || !eligible(change.module.state) ||
            state(change.module.path) != change.module.state)
            return "Boot configuration changed or is unsupported. Reopen Settings and inspect the flags.";
    }

    const auto move = [](const Module& module, State from, State to) -> std::error_code {
        if (state(module.path) != from)
            return std::make_error_code(std::errc::state_not_recoverable);
        std::error_code error;
        if (from != to)
            fs::rename(module.path / (from == State::Enabled ? activeFlag : parkedFlag),
                       module.path / (to == State::Enabled ? activeFlag : parkedFlag), error);
        return error;
    };
    for (size_t i = 0; i < changes.size(); ++i) {
        const auto& change = changes[i];
        auto error = move(change.module, change.module.state, change.target);
        if (!error) continue;
        std::string message = "Boot flag change failed for " + change.module.id + ": " + error.message();
        while (i > 0) {
            const auto& previous = changes[--i];
            if (previous.module.state != previous.target &&
                move(previous.module, previous.target, previous.module.state))
                message += ". Rollback failed for " + previous.module.id;
        }
        return message;
    }
    return {};
}

} // namespace switchu::boot_modules
