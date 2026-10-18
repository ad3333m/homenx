#include "WeatherDrawing.hpp"
#include <nxui/core/Font.hpp>
#include <nxui/core/Renderer.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <string>

namespace {

using nxui::Color;

enum class Sky { Clear, Partly, Cloudy, Fog, Rain, Snow, Storm };

// WMO weather codes, as Open-Meteo sends them.
Sky skyOf(int code) {
    if (code == 0) return Sky::Clear;
    if (code <= 2) return Sky::Partly;
    if (code == 3) return Sky::Cloudy;
    if (code == 45 || code == 48) return Sky::Fog;
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return Sky::Snow;
    if (code >= 95) return Sky::Storm;
    return Sky::Rain; // drizzle, rain, showers
}

const char* wordOf(int code) {
    if (code == 0) return "Clear";
    if (code == 1) return "Mostly clear";
    if (code == 2) return "Partly cloudy";
    if (code == 3) return "Cloudy";
    if (code == 45 || code == 48) return "Fog";
    if (code >= 51 && code <= 57) return "Drizzle";
    if (code >= 61 && code <= 67) return "Rain";
    if (code >= 71 && code <= 77) return "Snow";
    if (code >= 80 && code <= 82) return "Showers";
    if (code == 85 || code == 86) return "Snow showers";
    if (code >= 95) return "Thunderstorm";
    return "Weather";
}

Color skyColor(Sky sky, bool day) {
    if (!day) return {0.11f, 0.15f, 0.31f, 1.f};
    switch (sky) {
        case Sky::Clear: return {0.26f, 0.55f, 0.90f, 1.f};
        case Sky::Partly: return {0.36f, 0.58f, 0.86f, 1.f};
        case Sky::Snow: return {0.52f, 0.63f, 0.78f, 1.f};
        case Sky::Storm: return {0.27f, 0.30f, 0.40f, 1.f};
        default: return {0.42f, 0.50f, 0.61f, 1.f};
    }
}

void text(nxui::Renderer& ren, nxui::Font* font, const std::string& s, float x, float y, float scale, float maxW,
          const Color& c) {
    const float w = font->measure(s).x;
    if (w > 0.f && w * scale > maxW) scale = std::max(0.2f, maxW / w);
    ren.drawText(s, {x, y}, font, c, scale);
}

void textCentered(nxui::Renderer& ren, nxui::Font* font, const std::string& s, float cx, float y, float scale,
                  const Color& c) {
    ren.drawText(s, {cx - font->measure(s).x * scale * 0.5f, y}, font, c, scale);
}

void cloud(nxui::Renderer& ren, float cx, float cy, float d, const Color& c) {
    ren.drawCircle({cx - d * 0.15f, cy + d * 0.02f}, d * 0.16f, c);
    ren.drawCircle({cx + d * 0.03f, cy - d * 0.07f}, d * 0.21f, c);
    ren.drawCircle({cx + d * 0.2f, cy + d * 0.04f}, d * 0.14f, c);
    ren.drawRoundedRect({cx - d * 0.31f, cy + d * 0.02f, d * 0.65f, d * 0.17f}, c, d * 0.085f);
}

void sunOrMoon(nxui::Renderer& ren, float cx, float cy, float r, bool day, const Color& sky, float a) {
    if (day) {
        const Color sun{1.f, 0.80f, 0.24f, a};
        for (int i = 0; i < 8; ++i) {
            const float t = i * 3.14159265f / 4.f;
            ren.drawLine({cx + std::cos(t) * r * 1.35f, cy + std::sin(t) * r * 1.35f},
                         {cx + std::cos(t) * r * 1.75f, cy + std::sin(t) * r * 1.75f}, sun, std::max(2.f, r * 0.16f));
        }
        ren.drawCircle({cx, cy}, r, sun);
    } else {
        ren.drawCircle({cx, cy}, r, Color{0.96f, 0.93f, 0.78f, a});
        ren.drawCircle({cx + r * 0.45f, cy - r * 0.32f}, r * 0.86f, sky.withAlpha(a));
    }
}

void weatherIcon(nxui::Renderer& ren, float cx, float cy, float d, int code, bool day, const Color& skyC, float a) {
    const Sky sky = skyOf(code);
    const Color white{1.f, 1.f, 1.f, 0.96f * a};
    switch (sky) {
        case Sky::Clear:
            sunOrMoon(ren, cx, cy, d * 0.24f, day, skyC, a);
            return;
        case Sky::Partly:
            sunOrMoon(ren, cx - d * 0.13f, cy - d * 0.12f, d * 0.19f, day, skyC, a);
            cloud(ren, cx + d * 0.04f, cy + d * 0.08f, d * 0.9f, white);
            return;
        case Sky::Cloudy:
            cloud(ren, cx, cy, d, white);
            return;
        case Sky::Fog:
            for (int i = 0; i < 3; ++i)
                ren.drawRoundedRect({cx - d * (0.34f - i * 0.06f), cy - d * 0.18f + i * d * 0.17f, d * (0.68f - i * 0.08f),
                                     d * 0.08f},
                                    white, d * 0.04f);
            return;
        default:
            break;
    }
    // Rain, snow and storms fall from a cloud.
    cloud(ren, cx, cy - d * 0.12f, d, sky == Sky::Storm ? Color{0.86f, 0.88f, 0.92f, a} : white);
    const float top = cy + d * 0.16f;
    for (int i = 0; i < 3; ++i) {
        const float x = cx - d * 0.18f + i * d * 0.18f;
        if (sky == Sky::Snow) {
            ren.drawCircle({x, top + d * 0.08f + (i % 2) * d * 0.07f}, d * 0.045f, white);
        } else if (sky == Sky::Rain) {
            ren.drawLine({x + d * 0.04f, top}, {x - d * 0.03f, top + d * 0.17f}, Color{0.62f, 0.82f, 1.f, a},
                         std::max(2.f, d * 0.045f));
        }
    }
    if (sky == Sky::Storm) {
        const Color bolt{1.f, 0.84f, 0.2f, a};
        const float w = std::max(2.5f, d * 0.055f);
        ren.drawLine({cx + d * 0.05f, top - d * 0.02f}, {cx - d * 0.06f, top + d * 0.13f}, bolt, w);
        ren.drawLine({cx - d * 0.06f, top + d * 0.13f}, {cx + d * 0.05f, top + d * 0.13f}, bolt, w);
        ren.drawLine({cx + d * 0.05f, top + d * 0.13f}, {cx - d * 0.05f, top + d * 0.3f}, bolt, w);
    }
}

std::string degrees(int t) { return std::to_string(t) + "\xC2\xB0"; }

std::tm today() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    return local;
}

std::string formatted(const std::tm& t, const char* format) {
    char buf[48]{};
    std::strftime(buf, sizeof(buf), format, &t);
    return buf;
}

// Today as a calendar page: the weekday in red over the day's number.
void datePage(nxui::Renderer& ren, nxui::Font* font, const nxui::Rect& r, float s, bool onSky, float a) {
    const std::tm t = today();
    const float cx = r.x + r.width * 0.5f;
    const Color red = onSky ? Color{1.f, 0.62f, 0.6f, a} : Color{0.93f, 0.26f, 0.27f, a};
    const Color ink = onSky ? Color{1.f, 1.f, 1.f, a} : Color{0.11f, 0.11f, 0.13f, a};
    const Color grey = onSky ? Color{1.f, 1.f, 1.f, 0.72f * a} : Color{0.45f, 0.45f, 0.5f, a};
    std::string weekday = formatted(t, "%A");
    for (char& c : weekday) c = (char)std::toupper((unsigned char)c);
    const float big = std::min(2.4f * s, r.height * 0.5f / std::max(1.f, font->measure("28").y));
    textCentered(ren, font, weekday, cx, r.y + r.height * 0.1f, 0.44f * s, red);
    textCentered(ren, font, std::to_string(t.tm_mday), cx, r.y + r.height * 0.24f, big, ink);
    textCentered(ren, font, formatted(t, "%B"), cx, r.y + r.height * 0.76f, 0.42f * s, grey);
}

} // namespace

void drawWeatherWidget(nxui::Renderer& ren, const nxui::Rect& r, float radius, float s, int columns,
                       const switchu::widgets::WeatherNow* weather, nxui::Font* font, float a) {
    const bool valid = weather && weather->valid;
    const bool day = !valid || weather->day;
    const int code = valid ? weather->code : 2;
    const Color sky = skyColor(skyOf(code), day);
    ren.drawRoundedRect(r, sky.withAlpha(a), radius);
    if (!font) return;

    const nxui::Rect left{r.x, r.y, columns > 1 ? r.width * 0.56f : r.width, r.height};
    const float pad = 12.f * s;
    const float maxW = left.width - pad * 2.f;
    const Color white{1.f, 1.f, 1.f, a}, soft{1.f, 1.f, 1.f, 0.78f * a};
    const float d = std::min(left.width, left.height) * 0.42f;
    weatherIcon(ren, left.x + left.width - pad - d * 0.42f, left.y + pad + d * 0.36f, d, code, day, sky, a);

    if (!valid) {
        text(ren, font, "Weather", left.x + pad, left.y + pad, 0.5f * s, maxW - d * 0.6f, white);
        text(ren, font, weather && weather->loading ? "Loading..." : "Offline", left.x + pad,
             left.y + left.height - pad - 20.f * s, 0.42f * s, maxW, soft);
    } else {
        const std::string city = weather->city.empty() ? std::string("Here") : weather->city;
        text(ren, font, city, left.x + pad, left.y + pad, 0.46f * s, maxW - d * 0.7f, white);
        text(ren, font, degrees(weather->temp), left.x + pad, left.y + left.height * 0.3f, 1.45f * s, maxW, white);
        text(ren, font, wordOf(weather->code), left.x + pad, left.y + left.height - pad - 40.f * s, 0.42f * s, maxW, white);
        text(ren, font, "H:" + degrees(weather->high) + "  L:" + degrees(weather->low), left.x + pad,
             left.y + left.height - pad - 20.f * s, 0.4f * s, maxW, soft);
    }

    if (columns > 1) {
        const float x = r.x + left.width;
        ren.drawLine({x, r.y + pad * 1.5f}, {x, r.y + r.height - pad * 1.5f}, Color{1.f, 1.f, 1.f, 0.25f * a},
                     std::max(1.f, 1.5f * s));
        datePage(ren, font, {x, r.y, r.width - left.width, r.height}, s, true, a);
    }
}

void drawCalendarWidget(nxui::Renderer& ren, const nxui::Rect& r, float radius, float s, int columns,
                        bool sundayFirst, nxui::Font* font, float a) {
    ren.drawRoundedRect(r, Color{0.98f, 0.98f, 0.98f, 0.97f * a}, radius);
    if (!font) return;
    if (columns < 2) {
        datePage(ren, font, r, s, false, a);
        return;
    }

    // The month: its name, the weekdays, and today circled in red.
    const std::tm t = today();
    std::tm first = t;
    first.tm_mday = 1;
    std::mktime(&first);
    std::tm last = t;
    last.tm_mon += 1;
    last.tm_mday = 0;
    std::mktime(&last);
    const int days = last.tm_mday;
    const int lead = sundayFirst ? first.tm_wday : (first.tm_wday + 6) % 7;
    const int rows = (lead + days + 6) / 7;

    const Color red{0.93f, 0.26f, 0.27f, a}, ink{0.11f, 0.11f, 0.13f, a}, grey{0.55f, 0.55f, 0.6f, a};
    const float pad = 14.f * s;
    ren.drawText(formatted(t, "%B"), {r.x + pad, r.y + pad * 0.8f}, font, red, 0.5f * s);
    const float cellW = (r.width - pad * 2.f) / 7.f;
    const float gridTop = r.y + pad * 0.8f + 30.f * s;
    const float cellH = (r.y + r.height - pad * 0.6f - gridTop) / (rows + 1);
    const char* names = sundayFirst ? "SMTWTFS" : "MTWTFSS";
    const float lineH = font->measure("0").y;
    for (int c = 0; c < 7; ++c)
        textCentered(ren, font, std::string(1, names[c]), r.x + pad + cellW * (c + 0.5f),
                     gridTop + (cellH - lineH * 0.34f * s) * 0.5f, 0.34f * s, grey);
    const float numScale = std::min(0.42f * s, cellH * 0.7f / std::max(1.f, lineH));
    for (int day = 1; day <= days; ++day) {
        const int cell = lead + day - 1;
        const float cx = r.x + pad + cellW * (cell % 7 + 0.5f);
        const float cy = gridTop + cellH * (cell / 7 + 1.5f);
        const bool isToday = day == t.tm_mday;
        if (isToday) ren.drawCircle({cx, cy}, std::min(cellW, cellH) * 0.46f, red);
        textCentered(ren, font, std::to_string(day), cx, cy - lineH * numScale * 0.5f, numScale,
                     isToday ? Color{1.f, 1.f, 1.f, a} : ink);
    }
}
