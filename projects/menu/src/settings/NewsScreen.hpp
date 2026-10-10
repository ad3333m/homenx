#pragma once

#include "TabbedOverlayScreen.hpp"
#include <nxui/core/Texture.hpp>
#include <memory>

// HOME-menu News in the SwitchU overlay style: one tab per article, the
// article picture and headline in the header, paragraphs below.
class NewsScreen final : public TabbedOverlayScreen {
public:
    struct Article {
        std::string topic;
        std::string subject;
        std::string body;
        std::string footer;
        std::string linkUrl;
        std::shared_ptr<nxui::Texture> image;
    };

    NewsScreen();

    // Empty articles shows `emptyMessage` instead.
    void setArticles(std::vector<Article> articles, std::string emptyMessage);
    void onOpenLink(StringCb cb) { m_openLinkCb = std::move(cb); }

protected:
    void buildTabs() override;
    float overlayHeaderHeight() const override { return 196.f; }
    float overlayTabWidth() const override { return 300.f; }
    void drawOverlayHeader(nxui::Renderer& ren, const nxui::Rect& panel, float opacity) override;

private:
    std::vector<Article> m_articles;
    std::string m_emptyMessage;
    StringCb m_openLinkCb;
};
