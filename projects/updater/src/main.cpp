// Homebrew Manager: the .nro apps on the SD card, updated from GitHub releases
// and the HB App Store.
#include "UpdaterActivity.hpp"

#include <nxui/Application.hpp>
#include <nxui/core/I18n.hpp>
#include <nxui/core/Renderer.hpp>
#include <switchu/file_log.hpp>
#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <memory>

extern "C" {
    size_t __nx_heap_size = 0x06000000; // the HB App Store's list alone is 1.6 MB of JSON
}

extern "C" void userAppInit(void) {
    romfsInit();
}

extern "C" void userAppExit(void) {
    romfsExit();
}

int main(int, char**) {
    switchu::FileLog::open("updater");
    SDL_Init(0);
    TTF_Init();
    nxui::Renderer::setShaderBasePath("romfs:/shaders/");
    nxui::I18n::instance().initialize("romfs:/i18n", "en-US");

    nxui::Application application;
    application.setActivity(std::make_unique<updater::UpdaterActivity>());
    const bool ok = application.initialize();
    if (ok) application.run();
    application.shutdown();

    TTF_Quit();
    SDL_Quit();
    switchu::FileLog::close();
    return ok ? 0 : 1;
}
