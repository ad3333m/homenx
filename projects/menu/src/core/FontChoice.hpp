#pragma once
#include <fstream>
#include <string>

// HomeNX's extra fonts (romfs/fonts, SIL OFL 1.1: FONTS.txt there). What HOME,
// Discord in HOME and the Discord overlay use is one file name each in
// sdmc:/config/SwitchU/font_<which>.txt ("" = their usual font). The overlay
// reads font_overlay.txt itself.
namespace fontchoice {

struct Font {
    const char* name;
    const char* file;
};
inline constexpr Font kFonts[] = {
    {"Nunito", "Nunito.ttf"},
    {"Quicksand", "Quicksand.ttf"},
    {"Fredoka", "Fredoka.ttf"},
    {"Poppins", "Poppins.ttf"},
    {"Montserrat", "Montserrat.ttf"},
    {"Inter", "Inter.ttf"},
    {"Roboto", "Roboto.ttf"},
    {"Rubik", "Rubik.ttf"},
    {"Lexend", "Lexend.ttf"},
    {"Atkinson Hyperlegible", "AtkinsonHyperlegible.ttf"},
    {"Comfortaa", "Comfortaa.ttf"},
    {"Baloo 2", "Baloo2.ttf"},
    {"Patrick Hand", "PatrickHand.ttf"},
    {"Space Mono", "SpaceMono.ttf"},
    {"VT323 (pixel)", "VT323.ttf"},
};

inline std::string configPath(const char* which) {
    return std::string("sdmc:/config/SwitchU/font_") + which + ".txt";
}

inline std::string chosen(const char* which) {
    std::string file;
    std::ifstream in(configPath(which));
    std::getline(in, file);
    return file;
}

inline void choose(const char* which, const std::string& file) {
    std::ofstream(configPath(which), std::ios::trunc) << file;
}

} // namespace fontchoice
