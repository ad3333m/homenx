#pragma once
#include <nxui/widgets/GlassWidget.hpp>
#include <nxui/core/Texture.hpp>
#include <nxui/core/I18n.hpp>
#include <nxui/core/Types.hpp>
#include <string>



class AppletButton : public nxui::GlassWidget {
public:
    AppletButton();
    ~AppletButton() override;

    void setIcon(nxui::Texture* tex)        { m_icon = tex; }
    void setIconCircular(bool enabled)      { m_iconCircular = enabled; }
    void setBadge(bool on)                  { m_badge = on; } // HomeNX: a red dot (unread)
    nxui::Texture* icon() const             { return m_icon; }

    void setLabel(const std::string& l);
    void setLabelKey(const std::string& key, const std::string& fallback = "");
    const std::string& label() const    { return m_label; }

    bool hitTest(float sx, float sy) const { return m_rect.contains(sx, sy); }

protected:
    void onContentRender(nxui::Renderer& ren) override;

private:
    void refreshLocalizedLabel();

    nxui::Texture* m_icon = nullptr;
    bool           m_iconCircular = false;
    bool           m_badge = false;
    std::string    m_label;
    std::string    m_labelSource;
    std::string    m_labelKey;
    std::string    m_labelFallback;
    bool           m_useLabelKey = false;
    int            m_i18nListenerId = -1;
};

