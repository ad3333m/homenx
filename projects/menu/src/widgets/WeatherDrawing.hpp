#pragma once
#include "core/WidgetStore.hpp"
#include <nxui/core/Types.hpp>

namespace nxui { class Font; class Renderer; }

// HomeNX: the Weather and Calendar widgets' faces (s = the icon's zoom).
void drawWeatherWidget(nxui::Renderer& ren, const nxui::Rect& r, float radius, float s, int columns,
                       const switchu::widgets::WeatherNow* weather, nxui::Font* font, float opacity);
void drawCalendarWidget(nxui::Renderer& ren, const nxui::Rect& r, float radius, float s, int columns,
                        bool sundayFirst, nxui::Font* font, float opacity);
