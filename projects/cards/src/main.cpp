// HomeNX Cards: a test HOME screen with box-art cards (see CardsActivity).
#include "CardsActivity.hpp"

#include <nxui/Application.hpp>
#include <nxui/core/Renderer.hpp>
#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <memory>

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
    SDL_Init(0);
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
