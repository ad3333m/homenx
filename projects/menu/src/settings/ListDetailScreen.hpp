#pragma once

#include "TabbedOverlayScreen.hpp"
#include <nxui/core/Texture.hpp>
#include <functional>
#include <memory>

// SwitchU-style overlay with a list on the left and the selected entry drawn
// on the right (picture, title, text). Used for News, Friends, Homebrew and Miis.
// Right / A moves into the entry so up/down scroll it; left / B goes back.
class ListDetailScreen final : public TabbedOverlayScreen {
public:
    struct Entry {
        std::string listName;   // left column
        std::string kicker;     // small line above the title
        std::string title;
        std::string body;       // paragraphs separated by newlines
        std::string footer;
        std::string actionHint;          // e.g. "Press A to launch"; shown when onActivate is set
        std::function<void()> onActivate; // A inside the entry
        std::shared_ptr<nxui::Texture> image;
        // Loads `image` the first time the entry is selected (keeps opening fast).
        std::function<std::shared_ptr<nxui::Texture>()> lazyImage;
        bool roundImage = false;          // avatar style instead of a banner
        bool unread = false;              // green dot on its row in the list
        std::function<void()> onShown;    // first time it's shown (marks it read)
        // Draws a portrait in the image slot instead of a texture (Mii faces).
        std::function<void(nxui::Renderer&, const nxui::Rect&, float opacity)> drawPortrait;
    };

    ListDetailScreen();

    // With no entries the right side shows emptyTitle / emptyMessage.
    void setEntries(std::vector<Entry> entries, std::string emptyTitle, std::string emptyMessage);

protected:
    void buildTabs() override;
    float overlayTabWidth() const override { return 330.f; }
    bool usesCustomContentLayout() const override { return true; }
    void drawCustomContent(nxui::Renderer& ren, const nxui::Rect& panel,
                           const nxui::Rect& content, float opacity) override;
    void onContentUpdate(float dt) override;
    void onContentRender(nxui::Renderer& ren) override;
    bool handleCustomPressA() override;
    bool handleCustomNavUp() override;
    bool handleCustomNavDown() override;
    bool handleCustomNavLeft() override;
    bool handleCustomNavRight() override;

private:
    struct Line {
        std::string text;
        nxui::Font* font = nullptr;
        float scale = 1.f;
        float y = 0.f;
        int tone = 0; // 0 primary, 1 secondary, 2 accent
    };
    const Entry* currentEntry() const;
    void layout(float width);
    void scrollBy(float dy);

    std::vector<Entry> m_entries;
    Entry m_empty;

    // Layout of the selected entry, rebuilt when the tab or width changes.
    std::vector<Line> m_lines;
    nxui::Rect m_imageRect{};
    float m_contentHeight = 0.f;
    int m_layoutTab = -1;
    float m_layoutWidth = 0.f;
    float m_viewHeight = 0.f;
    float m_scroll = 0.f;
    float m_scrollGoal = 0.f;
};
