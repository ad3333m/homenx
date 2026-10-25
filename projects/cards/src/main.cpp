// CardsNX: a HOME screen with box-art cards (see CardsActivity).
#include "CardsActivity.hpp"

#include <nxui/Application.hpp>
#include <nxui/core/Renderer.hpp>
#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <cstdio>
#include <memory>
#include <string>

extern "C" void userAppInit(void) {
    romfsInit();
    nsInitialize();
    pdmqryInitialize();
    psmInitialize();
    nifmInitialize(NifmServiceType_User);
    accountInitialize(AccountServiceType_Application);
}

extern "C" void userAppExit(void) {
    accountExit();
    nifmExit();
    psmExit();
    pdmqryExit();
    nsExit();
    plExit();
    romfsExit();
}

int main(int, char**) {
    // HomeNX's HOME asks for a Reboot to Hekate this way: Atmosphere sends only
    // some processes' reboots to the payload, and hbl (this) is one of them.
    if (std::remove("sdmc:/config/SwitchU/cardsnx_reboot_hekate") == 0) {
        std::string error;
        cards::power(cards::Power::Hekate, error); // returns only if it didn't happen
    }
    SDL_Init(SDL_INIT_AUDIO);
    TTF_Init();
    nxui::Renderer::setShaderBasePath("romfs:/shaders/");

    nxui::Application application;
    application.setActivity(std::make_unique<cards::CardsActivity>());
    const bool ok = application.initialize();
    if (ok) application.run();
    application.shutdown();

    TTF_Quit();
    SDL_Quit();
    return ok ? 0 : 1;
}
