#pragma once

#include "SwitchUInstallation.hpp"
#include "ReleaseUpdater.hpp"
#include "UpdateProgressDialog.hpp"
#include "widgets/ActionButton.hpp"
#include "widgets/OverlayDialog.hpp"
#include "widgets/SelectionCursor.hpp"

#include <nxui/Activity.hpp>
#include <nxui/Theme.hpp>
#include <nxui/core/Font.hpp>
#include <nxui/core/Texture.hpp>
#include <nxui/widgets/GlassPanel.hpp>
#include <nxui/widgets/Label.hpp>

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <string>

namespace switchu::manager {

class ManagerActivity final : public nxui::Activity {
public:
    bool onCreate() override;
    void onDestroy() override;
    void onUpdate(float dt) override;
    void onRender(nxui::Renderer& renderer) override;
    nxui::Widget* focusRoot() override;

private:
    struct ButtonView {
        std::shared_ptr<ActionButton> button;
        std::shared_ptr<nxui::Label> label;
    };

    ButtonView createButton(const nxui::Rect& rect, const std::string& text,
                            std::function<void()> action);
    void refreshState();
    void refreshPresentation();
    void requestToggle();
    void performPendingToggle();
    void requestReboot();
    void rebootNow();
    void requestUpdate();
    void startUpdateCheck();
    void startUpdateInstall();
    void syncUpdater();
    void beginUpdateInputBlock();
    void endUpdateInputBlock();
    void showError(ToggleError error, int detail);
    std::string errorText(ToggleError error, int detail) const;
    void setButtonVisible(ButtonView& view, bool visible, bool focusable);
    void focusFirstAvailable();

    nxui::Theme m_theme = nxui::Theme::dark();
    nxui::Font m_titleFont;
    nxui::Font m_bodyFont;
    nxui::Font m_smallFont;
    SwitchUInstallation m_installation;
    InstallationSnapshot m_snapshot;
    InstallationState m_runtimeState = InstallationState::Missing;

    std::shared_ptr<nxui::Widget> m_backdrop; // animated background
    std::shared_ptr<nxui::GlassPanel> m_heroCard;   // the on/off state and its button
    std::shared_ptr<nxui::GlassPanel> m_updateCard;
    std::shared_ptr<nxui::GlassPanel> m_infoCard;
    std::shared_ptr<nxui::Widget> m_decor;    // power ring, icons, version pill
    std::shared_ptr<nxui::Label> m_updateInfoLabel;
    nxui::Texture m_appIcon;
    nxui::Color m_stateColor{0.55f, 0.6f, 0.68f, 1.f}; // green on, slate off, amber unknown
    void drawDecor(nxui::Renderer& r);
    std::shared_ptr<nxui::Label> m_titleLabel;
    std::shared_ptr<nxui::Label> m_subtitleLabel;
    std::shared_ptr<nxui::Label> m_statusLabel;
    std::shared_ptr<nxui::Label> m_detailLabel;
    std::shared_ptr<nxui::Label> m_noticeLabel;
    std::shared_ptr<nxui::Label> m_helpLabel;
    ButtonView m_toggleButton;
    ButtonView m_updateButton;
    ButtonView m_rebootButton;
    ButtonView m_laterButton;
    std::shared_ptr<OverlayDialog> m_dialog;
    std::shared_ptr<UpdateProgressDialog> m_updateProgressDialog;
    std::shared_ptr<SelectionCursor> m_cursor;

    bool m_restartRequired = false;
    bool m_loading = false;
    bool m_rebooting = false;
    bool m_pendingTargetEnabled = false;
    int m_operationDelayFrames = 0;
    int m_rebootDelayFrames = 0;
    std::string m_errorMessage;

    enum class UpdateUiState {
        Idle,
        Checking,
        Available,
        UpToDate,
        Installing,
        Error,
    };
    UpdateUiState m_updateState = UpdateUiState::Idle;
    // Settings > HomeNX > Install opens this app with an "autostart" file.
    bool m_autoUpdateChecked = false;
    bool m_autoInstall = false;
    ReleaseInfo m_latestRelease;
    std::future<ReleaseInfo> m_updateCheckFuture;
    std::future<UpdateInstallResult> m_updateInstallFuture;
    std::atomic<float> m_updateDownloadProgress{0.f};
    std::atomic<float> m_updateInstallProgress{0.f};
    std::atomic<int> m_updateWorkerStage{static_cast<int>(UpdateWorkerStage::Idle)};
    int m_lastDownloadPercent = -1;
    int m_lastInstallPercent = -1;
    int m_lastUpdateStage = static_cast<int>(UpdateWorkerStage::Idle);
    bool m_exitLockedForUpdate = false;
    bool m_homeBlockedForUpdate = false;
    std::string m_updateError;
};

} // namespace switchu::manager
