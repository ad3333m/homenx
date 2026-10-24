#include "WiiUMenuApp.hpp"
#include "widgets/GlossyIcon.hpp"
#include "widgets/FolderPalette.hpp"
#include "DebugLog.hpp"
#include "friends/FriendStore.hpp"
#include "core/BackgroundThread.hpp"
#include "themeshop/ThemeHttp.hpp"
#include "social/NotificationLog.hpp"
#include <nlohmann/json.hpp>
#include <nxui/core/ThreadPool.hpp>
#include <mutex>
#ifdef SWITCHU_MENU
#include <switchu/control_cache.hpp>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <unordered_set>
#include <nxui/core/I18n.hpp>

namespace {
constexpr float kEditGhostMoveDuration = 0.20f;
// How long a dragged game must rest on a folder tile before it opens.
constexpr float kEditDragFolderHoldDelay = 0.55f;
}

bool WiiUMenuApp::isEditableIcon(nxui::Widget* w) const {
    if (!w || w->tag() != "glossy_icon")
        return false;
    auto* icon = static_cast<GlossyIcon*>(w);
    const int index = findTitleIndex(icon->titleId());
    if (index < 0)
        return false;
    return m_openFolderId != 0
        ? m_model.at(index).isApplication()
        : (m_model.at(index).isApplication() || m_model.at(index).isFolder() ||
           m_model.at(index).isWidget());
}

std::string WiiUMenuApp::accessibilityContextFor(nxui::Widget* w) const {
    auto& i18n = nxui::I18n::instance();
    if (!w)
        return {};
    if (m_dialog && m_dialog->isActive() && w == m_dialog.get())
        return i18n.tr("accessibility.context.dialog", "Dialog");
    if (m_contextMenu && m_contextMenu->isActive() && w == m_contextMenu.get())
        return i18n.tr("accessibility.context.menu", "Context menu");
    if (m_userSelect && m_userSelect->isActive() && w == m_userSelect.get())
        return i18n.tr("accessibility.context.profile_selection", "Profile selection");
    if (m_settings && m_settings->isActive() && w == m_settings.get())
        return i18n.tr("accessibility.context.settings", "Settings");
    if (m_themeShop && m_themeShop->isActive() && w == m_themeShop.get())
        return i18n.tr("accessibility.context.themes", "Themes");
    if (w->tag() == "glossy_icon" && m_grid) {
        if (m_appLayoutMode == AppLayoutMode::DynamicLine)
            return i18n.tr("accessibility.context.main_menu", "Main menu")
                 + ", " + i18n.tr("accessibility.context.dynamic_line", "Center line");
        return i18n.tr("accessibility.context.main_menu", "Main menu")
             + ", " + i18n.tr("accessibility.context.page", "page") + " "
             + std::to_string(m_grid->currentPage() + 1)
             + " " + i18n.tr("accessibility.context.of", "of") + " "
             + std::to_string(m_grid->totalPages());
    }
    for (const auto& btn : m_sidebar.leftButtons())
        if (btn.get() == w) return i18n.tr("accessibility.context.left_sidebar", "Left sidebar");
    for (const auto& btn : m_sidebar.rightButtons())
        if (btn.get() == w) return i18n.tr("accessibility.context.right_sidebar", "Right sidebar");
    if (m_profileLock && m_profileLock.get() == w)
        return i18n.tr("accessibility.context.user_profiles", "User profiles");
    return {};
}

std::string WiiUMenuApp::accessibilityActionsFor(nxui::Widget* w) const {
    auto& i18n = nxui::I18n::instance();
    if (!w)
        return {};
    if (m_editMode && w->tag() == "glossy_icon")
        return m_openFolderId != 0
            ? i18n.tr("accessibility.actions.edit_mode_folder", "Directional pad to choose the position. A to place. B to leave the folder while keeping the software selected.")
            : i18n.tr("accessibility.actions.edit_mode", "Directional pad to choose the new position. A to place or enter a folder. B to cancel.");
    if (w->tag() == "glossy_icon") {
        auto* icon = static_cast<GlossyIcon*>(w);
        if (icon->titleId() == 0)
            return i18n.tr("accessibility.actions.empty_slot", "Directional pad to navigate. Plus for options. Minus to change view.");
        const int index = findTitleIndex(icon->titleId());
        if (index >= 0 && m_model.at(index).isFolder())
            return i18n.tr("folder.open_hint", "A to open. Plus for folder options. Y to move. Minus to change view.");
        if (index >= 0 && m_model.at(index).isWidget() &&
            m_model.at(index).widgetType == switchu::widgets::WidgetType::RecentlyPlayed &&
            m_widgetStore.recentActivity().titleId != 0)
            return i18n.tr("accessibility.hints.recently_played_widget",
                           "A to launch the recently played game. Plus for widget options. Y to move.");
        if (index >= 0 && m_model.at(index).isWidget())
            return i18n.tr("accessibility.hints.widget",
                           "Plus for widget options. Y to move. Minus to change view.");
        return icon->isNotLaunchable()
            ? i18n.tr("accessibility.actions.game_blocked", "A to show the reason. Plus for options. Y to move. Minus to change view.")
            : i18n.tr("accessibility.actions.game_launchable", "A to launch. Plus for options. Y to move. Minus to change view.");
    }
    if (m_settings && w == m_settings.get())
        return i18n.tr("accessibility.actions.settings", "Up and down to choose a category. A or right to enter. B to close.");
    if (m_themeShop && w == m_themeShop.get())
        return i18n.tr("accessibility.actions.themes", "Up and down to navigate. A to choose. B to close.");
    if ((m_dialog && w == m_dialog.get()) || (m_userSelect && w == m_userSelect.get()))
        return i18n.tr("accessibility.actions.dialog", "Left and right to change choice. A to confirm. B to cancel.");
    if (m_contextMenu && w == m_contextMenu.get())
        return i18n.tr("accessibility.actions.context_menu", "Up and down to choose. A to confirm. B to close.");
    return {};
}

std::string WiiUMenuApp::accessibilityPositionFor(nxui::Widget* w) const {
    if (!w || !m_config.accessibilitySpeakPosition)
        return {};

    auto& i18n = nxui::I18n::instance();
    if (w->tag() == "glossy_icon" && m_grid) {
        const int global = m_grid->focusedGlobalIndex();
        if (global >= 0) {
            if (m_appLayoutMode == AppLayoutMode::DynamicLine) {
                const int total = (int)m_grid->allIcons().size();
                return i18n.tr("accessibility.context.dynamic_line", "Center line") + ", "
                     + i18n.tr("accessibility.position.item", "item") + " " + std::to_string(global + 1)
                     + " " + i18n.tr("accessibility.context.of", "of") + " " + std::to_string(total);
            }
            const int cols = std::max(1, m_grid->columns());
            const int rows = std::max(1, m_grid->rowsPerPage());
            const int local = global % std::max(1, m_grid->iconsPerPage());
            const int row = local / cols + 1;
            const int col = local % cols + 1;
            return i18n.tr("accessibility.position.row", "row") + " " + std::to_string(row)
                 + " " + i18n.tr("accessibility.context.of", "of") + " " + std::to_string(rows)
                 + ". " + i18n.tr("accessibility.position.column", "column") + " " + std::to_string(col)
                 + " " + i18n.tr("accessibility.context.of", "of") + " " + std::to_string(cols);
        }
    }

    auto describeLinear = [&](const auto& buttons) -> std::string {
        for (int i = 0; i < (int)buttons.size(); ++i) {
            if (buttons[(size_t)i].get() == w) {
                return std::to_string(i + 1) + " "
                     + i18n.tr("accessibility.context.of", "of") + " "
                     + std::to_string((int)buttons.size());
            }
        }
        return {};
    };

    if (auto text = describeLinear(m_sidebar.leftButtons()); !text.empty())
        return text;
    if (auto text = describeLinear(m_sidebar.rightButtons()); !text.empty())
        return text;

    return {};
}

void WiiUMenuApp::announceFocusedWidget(nxui::Widget* w) {
    if (!w)
        return;

    std::string hint = accessibilityActionsFor(w);
    std::string position = accessibilityPositionFor(w);

    std::string originalHint = w->accessibilityHint();
    if (m_accessibility.speakHints()) {
        if (!hint.empty())
            w->setAccessibilityHint(hint);
    } else {
        w->setAccessibilityHint({});
    }
    const bool forceRepeat = w->tag() == "glossy_icon";
    std::string summary = w->accessibilitySummary();
    if (summary.empty())
        summary = w->tag();
    m_accessibility.announceStructuredFocus(accessibilityContextFor(w),
                                            position,
                                            summary,
                                            forceRepeat);
    if ((m_accessibility.speakHints() && !hint.empty()) || !m_accessibility.speakHints())
        w->setAccessibilityHint(originalHint);
}

nxui::Texture* WiiUMenuApp::adoptEditGhostTexture(GlossyIcon* sourceIcon) {
    m_editGhostTexture.reset();
    if (!sourceIcon)
        return nullptr;

    // The held tile crosses between two unrelated streamer models when it
    // enters or leaves a folder. A borrowed pool texture can be detached or
    // recycled at that point, replacing the carried icon with a spinner (or,
    // worse, another game's icon). Give the ghost one stable texture for the
    // complete edit operation. The source bytes include custom SGDB icons.
    const auto bytes = AppListLoader::loadIconData(sourceIcon->titleId());
    if (!bytes.empty()) {
        auto owned = std::make_unique<nxui::Texture>();
        if (owned->loadFromMemory(app().gpu(), app().renderer(),
                                  bytes.data(), bytes.size(), 192)) {
            m_editGhostTexture = std::move(owned);
            return m_editGhostTexture.get();
        }
    }
    return sourceIcon->texture();
}

void WiiUMenuApp::startEditGhost(GlossyIcon* sourceIcon) {
    stopEditGhost();
    if (!sourceIcon)
        return;

    if (m_editSourceIndex >= 0)
        m_iconStreamer.setPinnedIndex(m_editSourceIndex);

    m_editSourceIcon = sourceIcon;
    m_editSourceIcon->setOpacity(0.10f);

    auto ghost = std::make_shared<GlossyIcon>();
    ghost->setTag("edit_ghost");
    ghost->setFocusable(false);
    ghost->setTitle(sourceIcon->title());
    ghost->setTitleId(sourceIcon->titleId());
    ghost->copyWidgetPresentationFrom(*sourceIcon);
    // copyWidgetPresentationFrom also copies the source's borrowed streamer
    // pointer. Install the owned texture afterwards so leaving a folder cannot
    // make the ghost inherit the first remaining game's recycled pool slot.
    ghost->setTexture(adoptEditGhostTexture(sourceIcon));
    ghost->setIsGameCard(sourceIcon->isGameCard());
    ghost->setGameCardTexture(sourceIcon->gameCardTexture());
    ghost->setNotLaunchable(sourceIcon->isNotLaunchable());
    ghost->setCornerRadius(sourceIcon->cornerRadius());
    ghost->setBlurEnabled(false);
    ghost->setPanelOpacity(0.84f);
    ghost->setOpacity(0.84f);
    ghost->setScale(1.06f);
    ghost->forceVisible();

    m_editGhostTargetRect = m_grid
        ? m_grid->gridSpanRect(m_editSourceIndex,
                               ghost->gridSpanColumns(), ghost->gridSpanRows())
        : sourceIcon->focusRect();
    ghost->setRect(m_editGhostTargetRect);
    m_editGhostRect.setImmediate(m_editGhostTargetRect);
    m_editGhostRectInit = true;
    m_editGhostPulse = 0.f;

    m_editGhostIcon = ghost;
    DebugLog::log("[edit] ghost started index=%d title=0x%016lX ownedTexture=%d",
                  m_editSourceIndex,
                  static_cast<unsigned long>(sourceIcon->titleId()),
                  m_editGhostTexture ? 1 : 0);
}

void WiiUMenuApp::stopEditGhost() {
    if (m_editGhostIcon || m_editGhostTexture) {
        DebugLog::log("[edit] ghost stopping index=%d ownedTexture=%d",
                      m_editSourceIndex, m_editGhostTexture ? 1 : 0);
    }
    detachEditSourceIcon();

    m_editGhostIcon.reset();
    m_editGhostTexture.reset();
    m_editGhostRectInit = false;
    m_editGhostPulse = 0.f;
    m_editGhostTouchFollow = false;
    m_editDragEdgeDir = 0;
    m_editDragEdgeHold = 0.f;
    m_editDragFolderHold = 0.f;
    m_editDragFolderOpenWait = 0.f;
}

void WiiUMenuApp::detachEditSourceIcon() {
    m_iconStreamer.clearPinnedIndex();
    if (m_editSourceIcon)
        m_editSourceIcon->setOpacity(1.f);
    m_editSourceIcon = nullptr;
    if (m_editGhostIcon && !m_editGhostTexture)
        m_editGhostIcon->setTexture(nullptr);
    syncEditJiggle();
}

void WiiUMenuApp::syncEditJiggle() {
    if (!m_grid)
        return;
    const auto& icons = m_grid->allIcons();
    for (std::size_t i = 0; i < icons.size(); ++i) {
        if (!icons[i])
            continue;
        const bool on = m_editMode && icons[i].get() != m_editSourceIcon;
        icons[i]->setJiggle(on, static_cast<float>(i) * 1.7f);
    }
}

void WiiUMenuApp::reattachEditSourceIcon() {
    if (!m_editMode || !m_grid || m_editHeldTitleId == 0)
        return;
    const int index = findTitleIndex(m_editHeldTitleId);
    if (index < 0 || index >= static_cast<int>(m_grid->allIcons().size())) {
        m_editSourceIndex = -1;
        return;
    }
    auto& icon = m_grid->allIcons()[static_cast<std::size_t>(index)];
    if (!icon)
        return;
    m_editSourceIndex = index;
    m_editSourceIcon = icon.get();
    m_editSourceIcon->setOpacity(0.10f);
    syncEditJiggle();
    m_iconStreamer.setPinnedIndex(index);
    if (m_editGhostIcon && !m_editGhostTexture)
        m_editGhostIcon->setTexture(m_editSourceIcon->texture());
}

void WiiUMenuApp::syncEditPlacementAfterModelChange(bool preferEmptySlot) {
    if (!m_editMode || !m_grid || m_model.count() <= 0)
        return;

    const int count = m_model.count();
    int target = m_editTargetIndex;
    const bool stale = target < 0 || target >= count;

    if (preferEmptySlot || stale) {
        target = 0;
        if (preferEmptySlot) {
            for (int i = 0; i < count; ++i) {
                const auto& entry = m_model.at(i);
                if (entry.kind == GridEntryKind::Empty ||
                    (entry.titleId == 0 &&
                     entry.kind != GridEntryKind::WidgetContinuation)) {
                    target = i;
                    break;
                }
            }
        } else {
            const int focused = m_grid->focusedGlobalIndex();
            if (focused >= 0 && focused < count)
                target = focused;
        }
    }

    m_editTargetIndex = std::clamp(target, 0, count - 1);
    if (m_grid->focusGlobalIndex(m_editTargetIndex)) {
        if (auto* focused = m_grid->focusManager().current()) {
            focusManager().setFocus(focused);
            if (focused->tag() == "glossy_icon")
                bindEditActions(static_cast<GlossyIcon*>(focused));
        }
    }

    const int spanColumns = m_editGhostIcon
        ? std::max(1, m_editGhostIcon->gridSpanColumns()) : 1;
    const int spanRows = m_editGhostIcon
        ? std::max(1, m_editGhostIcon->gridSpanRows()) : 1;
    m_editGhostTargetRect = m_grid->gridSpanRect(
        m_editTargetIndex, spanColumns, spanRows);
    if (m_editGhostIcon)
        m_editGhostIcon->setRect(m_editGhostTargetRect);
    updateCursor();
    DebugLog::log("[edit] sync placement target=%d preferEmpty=%d stale=%d",
                  m_editTargetIndex, preferEmptySlot ? 1 : 0, stale ? 1 : 0);
}

void WiiUMenuApp::updateEditGhost(float dt) {
    if (!m_editMode || !m_editGhostIcon)
        return;

    if (m_editGhostTouchFollow) {
        // Free touch drag: keep the tile under the finger instead of snapping it
        // to the grid slots (the tracked slot still drives the drop target and
        // the cursor). Dragging past the grid's side edge flips pages, and
        // holding there keeps flipping.
        float w = m_editGhostTargetRect.width;
        float h = m_editGhostTargetRect.height;
        if (w <= 1.f || h <= 1.f) {
            const nxui::Rect cur = m_editGhostIcon->rect();
            w = cur.width;
            h = cur.height;
        }
        const nxui::Rect r{m_editGhostTouchPos.x - w * 0.5f,
                           m_editGhostTouchPos.y - h * 0.5f, w, h};
        m_editGhostRect.setImmediate(r);
        m_editGhostRectInit = true;
        m_editGhostPulse += dt;
        const float pulse = 0.80f + 0.08f * std::sin(m_editGhostPulse * 8.f);
        m_editGhostIcon->setOpacity(pulse);
        m_editGhostIcon->setPanelOpacity(std::min(1.f, pulse + 0.12f));
        m_editGhostIcon->setScale(1.07f + 0.025f * std::sin(m_editGhostPulse * 7.f));
        m_editGhostIcon->setRect(r);

        if (m_grid && m_appLayoutMode != AppLayoutMode::DynamicLine) {
            const nxui::Rect content = m_grid->contentRect();
            constexpr float kEdgeZone = 48.f;
            int dir = 0;
            if (content.width > 0.f) {
                if (m_editGhostTouchPos.x < content.x + kEdgeZone)
                    dir = -1;
                else if (m_editGhostTouchPos.x > content.x + content.width - kEdgeZone)
                    dir = +1;
            }
            if (dir == 0) {
                m_editDragEdgeDir = 0;
                m_editDragEdgeHold = 0.f;
            } else {
                if (dir != m_editDragEdgeDir) {
                    m_editDragEdgeDir = dir;
                    m_editDragEdgeHold = 0.f;
                }
                m_editDragEdgeHold += dt;
                constexpr float kEdgeHoldDelay = 0.45f;
                if (m_editDragEdgeHold >= kEdgeHoldDelay) {
                    m_editDragEdgeHold = 0.f;
                    flipPage(dir);
                }
            }
        }

        // Resting a dragged game on a folder tile for a moment opens that
        // folder, so the release can pick the exact slot inside instead of
        // dropping blind on the tile. Slot tracking pauses while the folder
        // swaps in, because the hits still belong to the root model.
        if (m_editDragFolderOpenWait > 0.f) {
            m_editDragFolderOpenWait -= dt;
            if (m_openFolderId != 0)
                m_editDragFolderOpenWait = 0.f;
        }
        const bool folderOpening = m_editDragFolderOpenWait > 0.f;
        const bool canHoverOpen =
            m_openFolderId == 0 && !folderOpening && !m_folderCaptureRequested &&
            m_editHeldTitleId != 0 && m_editHeldTitleId < kFolderTitleIdPrefix &&
            !switchu::widgets::isWidgetTitleId(m_editHeldTitleId) &&
            m_editTargetIndex >= 0 && m_editTargetIndex < m_model.count();
        if (canHoverOpen) {
            const AppEntry hovered = m_model.at(m_editTargetIndex);
            if (hovered.isFolder()) {
                m_editDragFolderHold += dt;
                if (m_editDragFolderHold >= kEditDragFolderHoldDelay) {
                    m_editDragFolderHold = 0.f;
                    m_editDragFolderOpenWait = 0.75f;
                    // The slot index belongs to the root model that is about to
                    // be replaced; clearing it lets the folder's rebuild pick
                    // the first free slot for the carried ghost.
                    m_editTargetIndex = -1;
                    detachEditSourceIcon();
                    unbindEditActions();
                    requestOpenFolder(hovered.folderId);
                }
            } else {
                m_editDragFolderHold = 0.f;
            }
        } else {
            m_editDragFolderHold = 0.f;
        }
        return;
    }

    bool discreteTarget = false;
    if (m_grid && m_editTargetIndex >= 0) {
        const int target = m_editTargetIndex;
        m_editGhostTargetRect = m_grid->gridSpanRect(
            target, m_editGhostIcon->gridSpanColumns(),
            m_editGhostIcon->gridSpanRows());
        discreteTarget = !m_grid->isLayoutMorphing();
    } else if (m_cursor && m_cursor->isVisible()) {
        m_editGhostTargetRect = m_cursor->currentRect();
    } else if (auto* cur = focusManager().current()) {
        if (cur->tag() == "glossy_icon")
            m_editGhostTargetRect = cur->focusRect().expanded(4.f);
    }

    m_editGhostPulse += dt;
    float pulse = 0.80f + 0.08f * std::sin(m_editGhostPulse * 8.f);
    m_editGhostIcon->setOpacity(pulse);
    m_editGhostIcon->setPanelOpacity(std::min(1.f, pulse + 0.12f));
    m_editGhostIcon->setScale(1.07f + 0.025f * std::sin(m_editGhostPulse * 7.f));

    if (!m_editGhostRectInit) {
        m_editGhostRect.setImmediate(m_editGhostTargetRect);
        m_editGhostRectInit = true;
    } else if (!discreteTarget) {
        m_editGhostRect.setImmediate(m_editGhostTargetRect);
    } else {
        const nxui::Rect cur = m_editGhostRect.target();
        constexpr float eps = 0.5f;
        if (std::abs(cur.x - m_editGhostTargetRect.x) >= eps ||
            std::abs(cur.y - m_editGhostTargetRect.y) >= eps ||
            std::abs(cur.width - m_editGhostTargetRect.width) >= eps ||
            std::abs(cur.height - m_editGhostTargetRect.height) >= eps) {
            const nxui::Vec2 from = m_editGhostRect.value().center();
            const nxui::Vec2 to = m_editGhostTargetRect.center();
            const float dist = (to - from).length();
            const float dur = kEditGhostMoveDuration
                            * (1.f + std::clamp(dist / 320.f, 0.f, 2.4f) * 0.50f);
            m_editGhostRect.set(m_editGhostTargetRect, dur, nxui::Easing::outCubic);
        }
    }

    m_editGhostIcon->setRect(m_editGhostRect.value());
}

void WiiUMenuApp::unbindEditActions() {
    if (!m_editBoundIcon)
        return;
    m_editBoundIcon->clearActions();
    m_editBoundIcon = nullptr;
}

void WiiUMenuApp::bindEditActions(GlossyIcon* icon) {
    if (!icon)
        return;
    if (m_editBoundIcon == icon)
        return;

    unbindEditActions();
    m_editBoundIcon = icon;

    icon->addAction(static_cast<uint64_t>(nxui::Button::A), [this]() {
        activateEditModeTarget();
    });
    icon->addAction(static_cast<uint64_t>(nxui::Button::B), [this]() {
        if (m_openFolderId != 0) {
            closeFolder(true);
        } else {
            exitEditMode();
            m_audio.playSfx(Sfx::ModalHide);
        }
    });
    icon->addDirectionAction(nxui::FocusDirection::LEFT, [this]() {
        moveFocusedIcon(nxui::FocusDirection::LEFT);
    });
    icon->addDirectionAction(nxui::FocusDirection::RIGHT, [this]() {
        moveFocusedIcon(nxui::FocusDirection::RIGHT);
    });
    icon->addDirectionAction(nxui::FocusDirection::UP, [this]() {
        moveFocusedIcon(nxui::FocusDirection::UP);
    });
    icon->addDirectionAction(nxui::FocusDirection::DOWN, [this]() {
        moveFocusedIcon(nxui::FocusDirection::DOWN);
    });
}

void WiiUMenuApp::enterEditMode() {
    auto* cur = focusManager().current();
    if (!isEditableIcon(cur))
        return;

    auto* icon = static_cast<GlossyIcon*>(cur);
    m_editMode = true;
    m_editSourceIndex = m_grid ? m_grid->focusedGlobalIndex() : -1;
    m_editTargetIndex = m_editSourceIndex;
    m_editHeldTitleId = icon->titleId();
    m_editOriginFolderId = m_openFolderId;
    m_editOriginFolderIndex = m_openFolderId != 0 ? m_editSourceIndex : -1;
    m_editOriginRootSlot = m_openFolderId == 0 ? m_editSourceIndex : -1;
    m_editHeldTitle = icon->title();
    startEditGhost(icon);
    bindEditActions(icon);
    syncEditJiggle();
    m_titlePill->setText(nxui::I18n::instance().tr("game.move_prefix", "Move: ") + m_editHeldTitle);
    m_titlePill->setVisible(true);
    m_accessibility.announce(nxui::I18n::instance().tr(
        "accessibility.move_mode.enter",
        "Moving game: ") + m_editHeldTitle, true, true);
}

void WiiUMenuApp::exitEditMode() {
    if (!m_editMode)
        return;

    m_editMode = false;
    unbindEditActions();
    m_editSourceIndex = -1;
    m_editTargetIndex = -1;
    m_editOriginRootSlot = -1;
    m_editOriginFolderIndex = -1;
    m_editOriginFolderId = 0;
    m_editHeldTitleId = 0;
    m_editHeldTitle.clear();
    stopEditGhost();
    syncEditJiggle();

    auto* cur = focusManager().current();
    if (isEditableIcon(cur)) {
        auto* icon = static_cast<GlossyIcon*>(cur);
        m_titlePill->setText(pillTitle(icon));
        m_titlePill->setVisible(true);
    } else {
        m_titlePill->hideAnimated();
    }

    if (m_layoutDirty)
        saveMenuLayout();
}

bool WiiUMenuApp::commitEditModePlacement() {
    if (!m_editMode || !m_grid)
        return false;

    int from = m_editSourceIndex;
    int target = m_editTargetIndex;
    if (from < 0 || target < 0 || from >= m_model.count() || target >= m_model.count())
        return false;
    if (m_model.at(from).titleId == 0)
        return false;

    int oldPage = m_grid->currentPage();
    bool changed = (from != target);
    DebugLog::log("[edit] commit from=%d target=%d changed=%d", from, target,
                  changed ? 1 : 0);
    if (m_appLayoutMode == AppLayoutMode::DynamicLine) {
        if (!changed)
            return true;

        std::vector<std::uint64_t> visibleOrder;
        visibleOrder.reserve(static_cast<std::size_t>(m_model.count()));
        for (const auto& entry : m_model.entries()) {
            if (entry.titleId != 0 && entry.kind != GridEntryKind::WidgetContinuation)
                visibleOrder.push_back(entry.titleId);
        }
        auto sourceIt = std::find(visibleOrder.begin(), visibleOrder.end(),
                                  m_editHeldTitleId);
        if (sourceIt == visibleOrder.end())
            return false;
        const std::size_t sourceOrder = static_cast<std::size_t>(
            std::distance(visibleOrder.begin(), sourceIt));
        const std::uint64_t targetTitleId = m_model.at(target).titleId;
        std::size_t destinationOrder = visibleOrder.empty() ? 0 : visibleOrder.size() - 1;
        if (targetTitleId != 0) {
            const auto destinationIt = std::find(visibleOrder.begin(), visibleOrder.end(),
                                                 targetTitleId);
            if (destinationIt == visibleOrder.end())
                return false;
            destinationOrder = static_cast<std::size_t>(
                std::distance(visibleOrder.begin(), destinationIt));
            std::swap(visibleOrder[sourceOrder], visibleOrder[destinationOrder]);
        } else {
            const auto held = visibleOrder[sourceOrder];
            visibleOrder.erase(visibleOrder.begin() +
                               static_cast<std::ptrdiff_t>(sourceOrder));
            visibleOrder.push_back(held);
            destinationOrder = visibleOrder.size() - 1;
        }

        std::vector<std::size_t> visibleSlots;
        if (m_openFolderId != 0) {
            auto* folder = m_folderStore.find(m_openFolderId);
            if (!folder)
                return false;
            folder->titleIds = visibleOrder;
            if (!saveFoldersOrReport("move_single_row"))
                return false;
        } else {
            const std::unordered_set<std::uint64_t> visibleIds(
                visibleOrder.begin(), visibleOrder.end());
            visibleSlots.reserve(visibleOrder.size());
            for (std::size_t index = 0; index < m_layoutSlots.size(); ++index) {
                if (visibleIds.count(m_layoutSlots[index]))
                    visibleSlots.push_back(index);
            }
            if (visibleSlots.size() != visibleOrder.size())
                return false;
        }

        detachEditSourceIcon();
        unbindEditActions();
        if (m_openFolderId != 0) {
            m_editSourceIndex = static_cast<int>(destinationOrder);
            m_editTargetIndex = m_editSourceIndex;
            applyDisplayModel(buildOpenFolderModel(m_openFolderId),
                              m_editHeldTitleId, false);
        } else {
            for (std::size_t index = 0; index < visibleOrder.size(); ++index)
                m_layoutSlots[visibleSlots[index]] = visibleOrder[index];
            m_editSourceIndex = static_cast<int>(destinationOrder);
            m_editTargetIndex = m_editSourceIndex;
            m_editOriginRootSlot = m_editSourceIndex;
            m_layoutDirty = true;
            applyDisplayModel(buildRootFolderModel(), m_editHeldTitleId, false);
        }
        reattachEditSourceIcon();
        if (auto* focused = m_grid->focusManager().current()) {
            focusManager().setFocus(focused);
            if (focused->tag() == "glossy_icon")
                bindEditActions(static_cast<GlossyIcon*>(focused));
        }
        return true;
    }

    if (m_openFolderId == 0) {
        const auto sizeForTitle = [this](std::uint64_t titleId) {
            const auto widgetId = switchu::widgets::widgetIdFromTitleId(titleId);
            if (const auto* widget = widgetId ? m_widgetStore.find(widgetId) : nullptr)
                return switchu::widgets::validatedSize(
                    widget->type, widget->size, AppLayoutMode::Grid);
            return gameGridSize(titleId, AppLayoutMode::Grid);
        };
        const std::uint64_t displacedTitleId = m_model.at(target).titleId;
        const auto heldSize = sizeForTitle(m_editHeldTitleId);
        const auto displacedSize = sizeForTitle(displacedTitleId);
        const int columns = std::max(1, m_grid->columns());
        const auto footprintsOverlap = [columns](
                int firstAnchor, switchu::widgets::WidgetSize first,
                int secondAnchor, switchu::widgets::WidgetSize second) {
            for (int firstY = 0; firstY < first.rows; ++firstY) {
                for (int firstX = 0; firstX < first.columns; ++firstX) {
                    const int firstCell = firstAnchor + firstY * columns + firstX;
                    for (int secondY = 0; secondY < second.rows; ++secondY)
                        for (int secondX = 0; secondX < second.columns; ++secondX)
                            if (firstCell == secondAnchor + secondY * columns + secondX)
                                return true;
                }
            }
            return false;
        };
        if (!canPlaceGridItem(target, heldSize, m_editHeldTitleId,
                              displacedTitleId))
            return false;

        int displacedAnchor = from;
        if (displacedTitleId != 0 &&
            (!canPlaceGridItem(displacedAnchor, displacedSize,
                               m_editHeldTitleId, displacedTitleId) ||
             footprintsOverlap(target, heldSize,
                               displacedAnchor, displacedSize))) {
            displacedAnchor = -1;
            int bestDistance = std::numeric_limits<int>::max();
            for (int candidate = 0;
                 candidate < static_cast<int>(m_layoutSlots.size()); ++candidate) {
                if (!canPlaceGridItem(candidate, displacedSize,
                                      m_editHeldTitleId, displacedTitleId) ||
                    footprintsOverlap(target, heldSize, candidate, displacedSize))
                    continue;
                const int distance = std::abs(candidate - from);
                if (distance < bestDistance) {
                    displacedAnchor = candidate;
                    bestDistance = distance;
                }
            }
            if (displacedAnchor < 0)
                return false;
        }

        if (changed) {
            detachEditSourceIcon();
            unbindEditActions();
            std::replace(m_layoutSlots.begin(), m_layoutSlots.end(),
                         m_editHeldTitleId, std::uint64_t{0});
            if (displacedTitleId != 0)
                std::replace(m_layoutSlots.begin(), m_layoutSlots.end(),
                             displacedTitleId, std::uint64_t{0});
            m_layoutSlots[static_cast<std::size_t>(target)] = m_editHeldTitleId;
            if (displacedTitleId != 0)
                m_layoutSlots[static_cast<std::size_t>(displacedAnchor)] = displacedTitleId;
            m_editSourceIndex = target;
            m_editOriginRootSlot = target;
            m_layoutDirty = true;
            applyDisplayModel(buildRootFolderModel(), m_editHeldTitleId, false);
            reattachEditSourceIcon();
            // Both tiles traded places: glide each one across the gap the other
            // left. Started after the rebuild, which force-visibles every icon.
            m_grid->animateSwap(from, target,
                                displacedTitleId != 0 ? target : -1,
                                displacedTitleId != 0 ? displacedAnchor : -1);
            if (auto* focused = m_grid->focusManager().current()) {
                focusManager().setFocus(focused);
                if (focused->tag() == "glossy_icon")
                    bindEditActions(static_cast<GlossyIcon*>(focused));
            }
        }
        return true;
    }
    if (changed) {
        // Keep the three index-based stores atomic. A catalogue refresh can
        // resize the streamer between entering move mode and committing it;
        // partially swapping the model used to leave texture/index pointers
        // inconsistent for the next render.
        if (!m_iconStreamer.swapIndices(from, target)) {
            DebugLog::log("[edit] rejected stale streamer swap from=%d target=%d", from, target);
            return false;
        }
        if (!m_model.swapEntries(from, target)) {
            m_iconStreamer.swapIndices(from, target);
            return false;
        }
        if (!m_grid->swapSlots(from, target)) {
            m_model.swapEntries(from, target);
            m_iconStreamer.swapIndices(from, target);
            return false;
        }
        if (!m_layoutSlots.empty() && from < (int)m_layoutSlots.size() && target < (int)m_layoutSlots.size())
            std::swap(m_layoutSlots[from], m_layoutSlots[target]);

        m_editSourceIndex = target;
        m_iconStreamer.setPinnedIndex(m_editSourceIndex);
        m_layoutDirty = true;
    }

    m_grid->focusGlobalIndex(target);

    int newPage = m_grid->currentPage();
    if (changed || newPage != oldPage) {
        m_iconStreamer.onPageChanged(newPage, m_grid->iconsPerPage(),
                                     app().gpu(), app().renderer(),
                                     m_grid->allIcons());
    }
    for (auto* icon : m_grid->pageIcons()) {
        if (icon)
            icon->forceVisible();
    }
    // Same-page swap inside a folder: the two icons glide past each other. The
    // forced refresh above would cancel an earlier start, so arm it afterwards.
    if (changed)
        m_grid->animateSwap(from, target, target, from);

    if (auto* cur = m_grid->focusManager().current())
        focusManager().setFocus(cur);

    if (m_editGhostIcon) {
        if (auto* focused = m_grid->focusManager().current())
            m_editGhostTargetRect = focused->focusRect().expanded(4.f);
    }

    updateCursor();
    return true;
}

bool WiiUMenuApp::activateEditModeTarget() {
    if (!m_editMode || !m_grid)
        return false;

    if (m_editHeldTitleId == 0)
        return false;

    int target = m_editTargetIndex;
    if (target < 0 || target >= m_model.count()) {
        // A touch release can arrive before the rebuilt model has claimed a slot
        // for the carried ghost. Fall back to the first free slot so the gesture
        // ends with a placement instead of stranding the move.
        if (!m_touchEditDragActive && !m_editGhostTouchFollow)
            return false;
        // While the hover-opened folder is still swapping in, the model in hand
        // is the old root one: any index read from it would be the wrong
        // destination, so let the caller put the tile back instead.
        if (m_editDragFolderOpenWait > 0.f)
            return false;
        target = -1;
        for (int i = 0; i < m_model.count(); ++i) {
            const auto& entry = m_model.at(i);
            if (entry.kind == GridEntryKind::Empty ||
                (entry.titleId == 0 &&
                 entry.kind != GridEntryKind::WidgetContinuation)) {
                target = i;
                break;
            }
        }
        if (target < 0)
            return false;
        m_editTargetIndex = target;
    }

    const AppEntry targetEntry = m_model.at(target);
    if (m_openFolderId == 0 && targetEntry.isFolder() &&
        m_editHeldTitleId < kFolderTitleIdPrefix) {
        // Drop the root-grid index before the folder model loads. Leaving it
        // intact made page-2+ folders keep an out-of-range target, which pinned
        // the edit ghost at (0,0) and blocked further movement (#95).
        m_editTargetIndex = -1;
        if ((m_touchEditDragActive || m_editGhostTouchFollow) &&
            !switchu::widgets::isWidgetTitleId(m_editHeldTitleId)) {
            // Touch drag released on a folder tile: the game goes inside and
            // the folder opens on it. Keeping the move alive here used to strand
            // the gesture — the drag flags were cleared on touch-up while the
            // ghost stayed afloat with no bound icon, so the tile could not be
            // picked up again.
            // Widgets are HOME-grid only: the folder model resolves titles from
            // the app catalogue, so storing one there would hide it for good.
            const std::uint32_t folderId = targetEntry.folderId;
            const std::uint64_t titleId = m_editHeldTitleId;
            const int originSlot = m_editOriginRootSlot;
            if (!m_folderStore.placeTitle(folderId, titleId, 0) ||
                !saveFoldersOrReport("place_in_folder")) {
                // Folder full: reuse the carry flow so the move is not stranded
                // — the ghost stays bound to the opened folder and B cancels it.
                detachEditSourceIcon();
                unbindEditActions();
                requestOpenFolder(folderId);
                return false;
            }
            if (originSlot >= 0 && originSlot < static_cast<int>(m_layoutSlots.size())) {
                m_layoutSlots[static_cast<std::size_t>(originSlot)] = 0;
                m_layoutDirty = true;
            }
            exitEditMode();
            // Refresh the root grid first: the folder frame is captured from the
            // current render, so the moved tile must already be gone from it.
            // Focus lands on the folder tile, the anchor the folder returns to.
            applyDisplayModel(buildRootFolderModel(), folderTitleId(folderId), false);
            requestOpenFolder(folderId);
            return true;
        }
        detachEditSourceIcon();
        unbindEditActions();
        requestOpenFolder(targetEntry.folderId);
        return true;
    }

    if (m_openFolderId != 0 && m_editHeldTitleId < kFolderTitleIdPrefix) {
        // Folder order lives in folders.json, not the HOME layoutSlots swap.
        // Same-folder drops swap in place; inbound drops still insert.
        const std::uint32_t targetFolderId = m_openFolderId;
        if (!m_folderStore.placeTitle(targetFolderId, m_editHeldTitleId,
                                      static_cast<std::size_t>(target)) ||
            !saveFoldersOrReport("place_in_folder"))
            return false;

        if (m_editOriginFolderId == 0 && m_editOriginRootSlot >= 0 &&
            m_editOriginRootSlot < static_cast<int>(m_layoutSlots.size())) {
            m_layoutSlots[static_cast<std::size_t>(m_editOriginRootSlot)] = 0;
            m_layoutDirty = true;
        }
        const std::uint64_t titleId = m_editHeldTitleId;
        exitEditMode();
        applyDisplayModel(buildOpenFolderModel(targetFolderId), titleId, false);
        m_audio.playSfx(Sfx::ConfirmPositive);
        return true;
    }

    if (m_openFolderId == 0 && targetEntry.kind == GridEntryKind::Empty &&
        m_editOriginFolderId != 0 &&
        target < static_cast<int>(m_layoutSlots.size())) {
        if (!m_folderStore.removeTitle(m_editOriginFolderId, m_editHeldTitleId) ||
            !saveFoldersOrReport("move_out_of_folder"))
            return false;
        const std::uint64_t titleId = m_editHeldTitleId;
        m_layoutSlots[static_cast<std::size_t>(target)] = titleId;
        m_layoutDirty = true;
        exitEditMode();
        applyDisplayModel(buildRootFolderModel(), titleId, false);
        m_audio.playSfx(Sfx::ConfirmPositive);
        return true;
    }

    if (m_editSourceIndex < 0)
        return false;
    if (!commitEditModePlacement())
        return false;
    exitEditMode();
    m_audio.playSfx(Sfx::ConfirmPositive);
    return true;
}

bool WiiUMenuApp::moveFocusedIcon(nxui::FocusDirection dir) {
    if (!m_editMode || !m_grid)
        return false;

    const int from = m_editTargetIndex;
    if (from < 0 || from >= m_model.count())
        return false;

    if (m_grid->isDynamicLine()) {
        int target = from;
        if (dir == nxui::FocusDirection::LEFT)
            --target;
        else if (dir == nxui::FocusDirection::RIGHT)
            ++target;
        else
            return false;
        if (target < 0 || target >= m_model.count() ||
            !m_grid->focusGlobalIndex(target))
            return false;
        m_editTargetIndex = target;
        if (auto* focused = m_grid->focusManager().current())
            focusManager().setFocus(focused);
        m_editGhostTargetRect = m_grid->gridSpanRect(target, 1, 1);
        updateCursor();
        return true;
    }

    const int cols = std::max(1, m_grid->columns());
    const int rows = std::max(1, m_grid->rowsPerPage());
    const int perPage = std::max(1, m_grid->iconsPerPage());
    const int totalPages = std::max(1, m_grid->totalPages());

    const int page = from / perPage;
    const int local = from % perPage;
    const int col = local % cols;
    const int row = local / cols;
    const int spanColumns = m_editGhostIcon
        ? std::max(1, m_editGhostIcon->gridSpanColumns()) : 1;
    const int spanRows = m_editGhostIcon
        ? std::max(1, m_editGhostIcon->gridSpanRows()) : 1;

    int target = from;
    switch (dir) {
        case nxui::FocusDirection::LEFT:
            if (col > 0)
                target = from - 1;
            else if (page > 0)
                target = (page - 1) * perPage + row * cols + (cols - 1);
            else
                return false;
            break;
        case nxui::FocusDirection::RIGHT:
            if (col + spanColumns < cols)
                target = from + 1;
            else if (page + 1 < totalPages)
                target = (page + 1) * perPage + row * cols;
            else
                return false;
            break;
        case nxui::FocusDirection::UP:
            if (row > 0)
                target = from - cols;
            else
                return false;
            break;
        case nxui::FocusDirection::DOWN:
            if (row + spanRows < rows)
                target = from + cols;
            else
                return false;
            break;
    }

    if (target < 0 || target >= m_model.count() || target == from)
        return false;

    m_editTargetIndex = target;
    const int targetPage = target / perPage;
    if (targetPage != m_grid->currentPage()) {
        m_grid->startPageTransition(targetPage);
        m_iconStreamer.onPageChanged(targetPage, perPage,
                                     app().gpu(), app().renderer(),
                                     m_grid->allIcons());
    }
    // Focus the underlying selectable item when possible, but the edit anchor
    // remains valid even over an invisible continuation cell.
    if (m_grid->focusGlobalIndex(target)) {
        if (auto* focused = m_grid->focusManager().current())
            focusManager().setFocus(focused);
    }
    m_editGhostTargetRect = m_grid->gridSpanRect(
        m_editTargetIndex, spanColumns, spanRows);
    updateCursor();
    return true;
}

void WiiUMenuApp::wireFocusCallback() {
    focusManager().onFocusChanged([this](nxui::Widget*, nxui::Widget* cur) {
        updateCursor();
        announceFocusedWidget(cur);

        if ((m_contextMenu && m_contextMenu->isActive()) ||
            (m_dialog && m_dialog->isActive()) ||
            (m_themeShop && m_themeShop->isActive()) ||
            (m_settings && m_settings->isActive()) ||
            (m_steamGridDbPicker && m_steamGridDbPicker->isActive()) ||
            (m_userSelect && m_userSelect->isActive()))
            return;

        bool suppressSfx = m_suppressNextNavigateSfx;
        m_suppressNextNavigateSfx = false;
        if (!suppressSfx)
            m_audio.playSfx(Sfx::Navigate);

        if (cur && cur->tag() == "glossy_icon") {
            m_grid->focusManager().setFocus(cur);
            auto* icon = static_cast<GlossyIcon*>(cur);
            if (m_appLayoutMode == AppLayoutMode::DynamicLine) {
                const int focusedIndex = m_grid->focusedGlobalIndex();
                if (focusedIndex >= 0) {
                    m_iconStreamer.onPageChanged(focusedIndex, 1,
                                                 app().gpu(), app().renderer(),
                                                 m_grid->allIcons());
                }
            }
            if (m_steamGridDbBackdrop && icon->titleId() != 0
                && icon->titleId() < kFolderTitleIdPrefix)
                m_steamGridDbBackdrop->showTitle(icon->titleId());
            auto& i18n = nxui::I18n::instance();
            if (m_editMode) {
                bindEditActions(icon);
                if (m_editGhostIcon)
                    m_editGhostTargetRect = m_grid->gridSpanRect(
                        m_editTargetIndex,
                        m_editGhostIcon->gridSpanColumns(),
                        m_editGhostIcon->gridSpanRows());
                else
                    m_editGhostTargetRect = icon->focusRect();
                if (!m_editHeldTitle.empty())
                    m_titlePill->setText(i18n.tr("game.move_prefix", "Move: ") + m_editHeldTitle);
                else if (icon->titleId() != 0)
                    m_titlePill->setText(i18n.tr("game.move_prefix", "Move: ") + icon->title());
                else
                    m_titlePill->setText(i18n.tr("game.move", "Move"));
                m_titlePill->setVisible(true);
                return;
            }
            if (icon->titleId() == 0) {
                m_titlePill->hideAnimated();
                return;
            }
            m_titlePill->setText(pillTitle(icon));
            m_titlePill->setVisible(true);
        } else if (cur) {
            if (m_editMode)
                exitEditMode();
            for (auto& btn : m_sidebar.leftButtons()) {
                if (btn.get() == cur) { m_titlePill->setText(btn->label()); m_titlePill->setVisible(true); return; }
            }
            for (auto& btn : m_sidebar.rightButtons()) {
                if (btn.get() == cur) { m_titlePill->setText(btn->label()); m_titlePill->setVisible(true); return; }
            }
            if (m_profileLock && m_profileLock.get() == cur) {
                m_titlePill->setText(m_profileLock->statusText());
                m_titlePill->setVisible(true);
                return;
            }
            if (m_bell && m_bell.get() == cur) {
                m_titlePill->setText(nxui::I18n::instance().tr("notifications.title", "Notifications"));
                m_titlePill->setVisible(true);
                return;
            }
            m_titlePill->hideAnimated();
        } else {
            m_titlePill->hideAnimated();
        }
    });
    updateCursor();
    if (auto* cur = focusManager().current()) {
        if (cur->tag() == "glossy_icon") {
            auto* icon = static_cast<GlossyIcon*>(cur);
            if (icon->titleId() != 0)
                m_titlePill->setText(pillTitle(icon));
        }
    }
}

// HomeNX: work on a worker thread for the title pill's play times.
struct PlayTimesJob {
    std::mutex mu;
    bool done = false;
    std::vector<PlayRecord> records;
};

// A game's name in the title pill, with how long you've played it.
std::string WiiUMenuApp::pillTitle(GlossyIcon* icon) const {
    std::string text = icon->title();
    auto it = m_playMinutes.find(icon->titleId());
    if (it != m_playMinutes.end() && it->second > 0) text += "  \xC2\xB7  " + hoursText(it->second);
    return text;
}

// HomeNX: once a frame, cheap. The sleep timer, and the play times arriving.
void WiiUMenuApp::updateHomeExtras() {
#ifdef SWITCHU_MENU
    if (m_sleepTimerEnd && armGetSystemTick() >= m_sleepTimerEnd) {
        m_sleepTimerEnd = 0;
        m_sleepTimerStep = 0;
        DebugLog::log("[sleep-timer] sleeping");
        m_launcher.enterSleep();
    }
#endif
    // Controller pairing: done when the console has one more paired, or after 30 s.
    if (m_pairing && armGetSystemTick() >= m_pairingCheck) {
        m_pairingCheck = armGetSystemTick() + armNsToTicks(400'000'000ULL);
        u8 count = 0;
        const bool paired = R_SUCCEEDED(btmsysGetPairedGamepadCount(&count)) && count > m_pairingCount;
        if (paired || armGetSystemTick() >= m_pairingUntil) {
            stopPairing();
            DebugLog::log("[pairing] %s", paired ? "paired" : "timed out");
            if (m_dialog) {
                auto& i18n = nxui::I18n::instance();
                m_audio.playSfx(paired ? Sfx::ConfirmPositive : Sfx::ModalShow);
                m_dialog->show(i18n.tr("pairing.title", "Pair a controller"),
                               paired ? i18n.tr("pairing.done", "Paired. The controller is ready to use.")
                                      : i18n.tr("pairing.none", "No new controller paired. If its lights stopped "
                                                                "flashing, it was already paired and is connected."),
                               {{i18n.tr("button.ok", "OK"), [] {}, true}}, 0, {});
                focusManager().setFocus(m_dialog.get());
            }
        }
    }
    if (!m_playJobStarted && !m_allApps.empty()) {
        m_playJobStarted = true;
        AccountUid uid = activeProfileUid();
        if (!accountUidIsValid(&uid) && !m_profiles.empty()) uid = m_profiles.front().uid;
        if (!accountUidIsValid(&uid)) return;
        std::vector<std::uint64_t> ids;
        for (const AppEntry& app : m_allApps)
            if (app.isApplication() && app.titleId != 0) ids.push_back(app.titleId);
        auto job = std::make_shared<PlayTimesJob>();
        m_playJob = job;
        runInBackground([job, uid, ids = std::move(ids)]() {
            nxui::moveToWorkerCores();
            std::vector<PlayRecord> records = loadMyPlayHistory(uid, ids);
            std::lock_guard<std::mutex> lk(job->mu);
            job->records = std::move(records);
            job->done = true;
        });
    }
    if (m_playJob) {
        std::lock_guard<std::mutex> lk(m_playJob->mu);
        if (!m_playJob->done) return;
        for (const PlayRecord& r : m_playJob->records) m_playMinutes[r.titleId] = r.minutes;
        m_playJob.reset();
        if (auto* cur = focusManager().current(); cur && cur->tag() == "glossy_icon" && !m_editMode) {
            auto* icon = static_cast<GlossyIcon*>(cur);
            if (icon->titleId() != 0 && m_titlePill && m_titlePill->isVisible()) m_titlePill->setText(pillTitle(icon));
        }
    }
}

struct WeatherJob {
    std::mutex mu;
    bool done = false;
    switchu::widgets::WeatherNow result;
};

namespace {

constexpr const char* kWeatherCache = "sdmc:/config/SwitchU/weather.json";

// Where the console is (from its IP: the Switch has no GPS), then that
// place's weather from Open-Meteo, in °F where that's what people use.
switchu::widgets::WeatherNow fetchWeather() {
    switchu::widgets::WeatherNow w;
    w.loading = false;
    try {
        const auto where = nlohmann::json::parse(
            themeshop::http::getText("http://ip-api.com/json/?fields=status,lat,lon,city,countryCode"));
        if (where.value("status", std::string()) != "success") return w;
        const std::string country = where.value("countryCode", std::string());
        w.fahrenheit = country == "US" || country == "LR" || country == "MM" || country == "BS" || country == "KY";
        w.city = where.value("city", std::string());
        char url[360];
        std::snprintf(url, sizeof(url),
                      "https://api.open-meteo.com/v1/forecast?latitude=%.3f&longitude=%.3f"
                      "&current=temperature_2m,weather_code,is_day&daily=temperature_2m_max,temperature_2m_min"
                      "&forecast_days=1&timezone=auto%s",
                      where.value("lat", 0.0), where.value("lon", 0.0),
                      w.fahrenheit ? "&temperature_unit=fahrenheit" : "");
        const auto j = nlohmann::json::parse(themeshop::http::getText(url));
        const auto& now = j.at("current");
        w.temp = (int)std::lround(now.value("temperature_2m", 0.0));
        w.code = now.value("weather_code", 0);
        w.day = now.value("is_day", 1) != 0;
        w.high = (int)std::lround(j.at("daily").at("temperature_2m_max").at(0).get<double>());
        w.low = (int)std::lround(j.at("daily").at("temperature_2m_min").at(0).get<double>());
        w.valid = true;
    } catch (const std::exception& ex) {
        DebugLog::log("[weather] failed: %s", ex.what());
    }
    return w;
}

} // namespace

void WiiUMenuApp::updateWeather() {
    if (!m_weatherWanted) return;
    const std::int64_t now = std::time(nullptr);
    if (!m_weatherNext) { // the last weather straight away, fetched again once it's half an hour old
        std::int64_t at = 0;
        try {
            std::ifstream in(kWeatherCache);
            const auto j = nlohmann::json::parse(in);
            m_weather.temp = j.value("temp", 0);
            m_weather.high = j.value("high", 0);
            m_weather.low = j.value("low", 0);
            m_weather.code = j.value("code", 0);
            m_weather.day = j.value("day", true);
            m_weather.fahrenheit = j.value("fahrenheit", false);
            m_weather.city = j.value("city", std::string());
            m_weather.valid = true;
            at = j.value("at", (std::int64_t)0);
        } catch (...) {
        }
        m_weatherNext = std::max(now + 8, at + 1800);
    }
    if (auto job = m_weatherJob) {
        switchu::widgets::WeatherNow got;
        {
            std::lock_guard<std::mutex> lk(job->mu);
            if (!job->done) return;
            got = job->result;
        }
        m_weatherJob.reset();
        m_weatherNext = now + (got.valid ? 1800 : 300);
        if (!got.valid) {
            m_weather.loading = false; // keeps the last weather, if any
            return;
        }
        m_weather = got;
        nlohmann::json j = {{"temp", got.temp}, {"high", got.high}, {"low", got.low}, {"code", got.code},
                            {"day", got.day}, {"fahrenheit", got.fahrenheit}, {"city", got.city}, {"at", now}};
        std::ofstream(kWeatherCache, std::ios::trunc) << j.dump();
        return;
    }
    if (now < m_weatherNext || (m_launchAnim && m_launchAnim->isPlaying())) return;
    auto job = std::make_shared<WeatherJob>();
    m_weatherJob = job;
    runInBackground([job]() {
        nxui::moveToWorkerCores();
        switchu::widgets::WeatherNow w = fetchWeather();
        std::lock_guard<std::mutex> lk(job->mu);
        job->result = std::move(w);
        job->done = true;
    });
}

void WiiUMenuApp::updateHomeBanners(float dt) {
    if (!m_discordBanner || !m_nowPlaying) return;
    const bool launching = m_launchAnim && m_launchAnim->isPlaying();
    const bool panelUp = m_discordPanel && m_discordPanel->isActive();

    // A saved Discord account connects a few seconds after HOME settles, so its
    // DMs and mentions show here (the overlay shows them over games).
    if (!m_discordAutoStarted && !leaveSplashActive() && m_deferredInitialAssetFrames == 0 && !launching
        && !m_discord.stopping() && (m_discordAutoStart -= dt) <= 0.f) {
        m_discordAutoStarted = true;
        const bool saved = m_discord.hasSavedAccount();
        DebugLog::log("[discord] HOME alerts: %s", saved ? "connecting" : "no saved account");
        if (!m_discord.started() && saved) m_discord.activate();
    }
    // Leaving for an applet stops Discord; HOME showing again a while later
    // means it came back without restarting: connect again.
    if (m_discordAutoStarted && m_discord.stopping() && !launching) {
        if ((m_discordStoppedFor += dt) > 8.f) {
            m_discordStoppedFor = 0.f;
            if (m_discord.hasSavedAccount()) m_discord.activate();
        }
    } else {
        m_discordStoppedFor = 0.f;
    }

    DiscordService::Alert alert;
    const bool gotAlert = m_discord.takeAlert(alert);
    if (gotAlert) { // the notification centre keeps every one, shown or not
        notifications::add(("discord/" + std::to_string(alert.channelId) + "/" + std::to_string(alert.guildId)).c_str(),
                           alert.title, alert.text);
        if (m_bell) m_bell->setUnread(m_bell->unread() + 1);
    }
    if (gotAlert && !panelUp && !launching) {
        ensureDiscordPanel(); // loads the sender's picture
        m_shownAlert = std::move(alert);
        const std::string title = m_discordPanel->clean(m_shownAlert.title);
        m_discordBanner->setContent(title, m_discordPanel->clean(m_shownAlert.text));
        const char first = title.empty() ? '?' : title[0];
        m_discordBanner->setFallback(nxui::Color(0.345f, 0.396f, 0.949f, 1.f),
                                     std::string(1, (char)std::toupper((unsigned char)first)));
        m_discordBanner->setHintShown(true);
        m_bannerIsDiscord = true;
        m_discordBanner->slideIn();
        m_discordBannerLeft = 6.f;
        m_audio.playSfx(Sfx::ModalShow);
    }
    // Controllers running low: one banner each, again once they've been charged.
    if ((m_batteryCheckIn -= dt) <= 0.f && !m_discordBanner->shown() && !launching) {
        m_batteryCheckIn = 5.f;
        static const HidNpadIdType kPlayers[] = {HidNpadIdType_No1, HidNpadIdType_No2, HidNpadIdType_No3,
                                                 HidNpadIdType_No4, HidNpadIdType_No5, HidNpadIdType_No6,
                                                 HidNpadIdType_No7, HidNpadIdType_No8};
        for (int player = 0; player < 8 && !m_discordBanner->shown(); ++player) {
            const u32 styles = hidGetNpadStyleSet(kPlayers[player]);
            HidPowerInfo power[2]{};
            const char* names[2] = {nullptr, nullptr};
            const nxui::Texture* pictures[2] = {nullptr, nullptr};
            if (styles & HidNpadStyleTag_NpadFullKey) {
                hidGetNpadPowerInfoSingle(kPlayers[player], &power[0]);
                names[0] = "Pro Controller";
            } else if (styles & (HidNpadStyleTag_NpadJoyDual | HidNpadStyleTag_NpadJoyLeft | HidNpadStyleTag_NpadJoyRight)) {
                hidGetNpadPowerInfoSplit(kPlayers[player], &power[0], &power[1]);
                if (styles & (HidNpadStyleTag_NpadJoyDual | HidNpadStyleTag_NpadJoyLeft)) {
                    names[0] = "Joy-Con (L)";
                    pictures[0] = &m_batteryJoyconLeftTex;
                }
                if (styles & (HidNpadStyleTag_NpadJoyDual | HidNpadStyleTag_NpadJoyRight)) {
                    names[1] = "Joy-Con (R)";
                    pictures[1] = &m_batteryJoyconRightTex;
                }
            }
            for (int side = 0; side < 2; ++side) {
                const std::uint32_t bit = 1u << (player * 2 + side);
                const bool low = names[side] && !power[side].is_charging && !power[side].is_powered
                    && power[side].battery_level <= 1;
                if (!low) {
                    m_lowWarned &= ~bit;
                    continue;
                }
                if (m_lowWarned & bit) continue;
                m_lowWarned |= bit;
                auto& i18n = nxui::I18n::instance();
                m_discordBanner->setContent(std::string(names[side]) + " " + i18n.tr("battery.low_title", "battery low"),
                                            i18n.tr("battery.player", "Player") + " " + std::to_string(player + 1) + " - "
                                                + i18n.tr("battery.low_text", "charge it soon"));
                notifications::add("battery", std::string(names[side]) + " " + i18n.tr("battery.low_title", "battery low"),
                                   i18n.tr("battery.player", "Player") + " " + std::to_string(player + 1));
                if (m_bell) m_bell->setUnread(m_bell->unread() + 1);
                m_discordBanner->setPicture(pictures[side] && pictures[side]->valid() ? pictures[side] : nullptr, false);
                m_discordBanner->setFallback(nxui::Color(0.93f, 0.26f, 0.27f, 1.f), "!");
                m_discordBanner->setHintShown(false);
                m_bannerIsDiscord = false;
                m_discordBanner->slideIn();
                m_discordBannerLeft = 6.f;
                m_audio.playSfx(Sfx::ModalShow);
                break;
            }
        }
    }
    if (m_discordBanner->shown()) {
        if (m_bannerIsDiscord)
            m_discordBanner->setPicture(m_discordPanel ? m_discordPanel->avatar(m_shownAlert.avatarUrl) : nullptr, true);
        if ((m_discordBannerLeft -= dt) <= 0.f || panelUp || launching) m_discordBanner->slideOut();
    }

    // The Discord button's red dot: unread DMs.
    if (m_discord.version() != m_discordBadgeVersion) {
        m_discordBadgeVersion = m_discord.version();
        if (AppletButton* b = m_sidebar.button(SidebarManager::kButtonMii))
            b->setBadge(m_discord.started() && m_discord.unreadDms() > 0);
    }

    // Now playing: HOME's music, bottom left, while it plays.
    const bool playing = m_audio.isPlaying() && !m_audio.isPaused() && m_audio.trackCount() > 0;
    // Playing or paused; once the music has loaded (it loads on a worker, and
    // reading the track list meanwhile raced with it).
    const bool onTrack = m_audioStarted && m_config.musicEnabled && m_audio.trackCount() > 0;
    const bool showMusic = onTrack && !launching && !panelUp && !m_editMode
        && m_navigator.route() == switchu::navigation::Route::Home
        && !(m_quickSettings && m_quickSettings->isActive());
    const bool newTrack = onTrack && m_audio.currentIndex() != m_nowPlayingIndex;
    if (onTrack && (newTrack || playing != m_nowPlayingWasPlaying)) {
        m_nowPlayingIndex = m_audio.currentIndex();
        m_nowPlayingWasPlaying = playing;
        const auto& tracks = m_audio.tracks();
        if (m_nowPlayingIndex >= 0 && m_nowPlayingIndex < (int)tracks.size()) {
            const MusicTrackInfo& t = tracks[(size_t)m_nowPlayingIndex];
            const std::string title = t.title.empty() ? t.displayTitle() : t.title;
            std::string line = t.artist.empty() ? nxui::I18n::instance().tr("music.now_playing", "Now playing") : t.artist;
            if (!playing) line = nxui::I18n::instance().tr("music.paused", "Paused");
            m_nowPlaying->setContent(title, line);
            m_nowPlaying->setFallback(nxui::Color(0.95f, 0.36f, 0.45f, 1.f),
                                      std::string(1, (char)std::toupper((unsigned char)(title.empty() ? '?' : title[0]))));
            if (newTrack) { // the cover only when the track changes (it's decoded here)
                m_nowPlayingCover = nxui::Texture{};
                const std::vector<uint8_t>& art = m_audio.currentCoverArt();
                if (!art.empty())
                    m_nowPlayingCover.loadFromMemory(app().gpu(), app().renderer(), art.data(), art.size(), 96);
                m_nowPlaying->setPicture(m_nowPlayingCover.valid() ? &m_nowPlayingCover : nullptr, false);
            }
        }
    }
    const float duration = m_audio.durationSeconds();
    m_nowPlaying->setProgress(duration > 0.f ? m_audio.positionSeconds() / duration : -1.f);
    if (showMusic) m_nowPlaying->slideIn();
    else m_nowPlaying->slideOut();
}

bool WiiUMenuApp::isCurrentFocusableWidget(nxui::Widget* w) const {
    if (!w) return false;
    if (m_steamGridDbPicker && m_steamGridDbPicker.get() == w) return w->isFocusable();
    if (m_textEntry && m_textEntry.get() == w) return w->isFocusable();
    if (m_quickSettings && m_quickSettings.get() == w) return w->isFocusable();
    if (m_themeShop && m_themeShop.get() == w) return w->isFocusable();
    if (m_settings && m_settings.get() == w) return w->isFocusable();
    for (const auto& btn : m_sidebar.leftButtons())
        if (btn.get() == w) return w->isFocusable();
    for (const auto& btn : m_sidebar.rightButtons())
        if (btn.get() == w) return w->isFocusable();
    if (m_profileLock && m_profileLock.get() == w) return w->isFocusable();
    if (m_bell && m_bell.get() == w) return w->isFocusable();
    if (m_grid)
        for (const auto& icon : m_grid->allIcons())
            if (icon.get() == w) return w->isFocusable();
    return false;
}

int WiiUMenuApp::findTitleIndex(uint64_t titleId) const {
    if (titleId == 0)
        return -1;
    for (int i = 0; i < m_model.count(); ++i) {
        if (m_model.at(i).titleId == titleId)
            return i;
    }
    return -1;
}

bool WiiUMenuApp::focusTitle(uint64_t titleId, bool instantFocus) {
    if (!m_grid)
        return false;

    int idx = findTitleIndex(titleId);
    if (idx < 0 && titleId != 0) {
        const std::uint32_t folderId = m_folderStore.folderForTitle(titleId);
        if (folderId == 0 || folderId == m_openFolderId)
            return false;
        requestOpenFolder(folderId, titleId);
        idx = findTitleIndex(folderTitleId(folderId));
        if (idx < 0)
            return true;
    }

    int oldPage = m_grid->currentPage();
    if (!m_grid->focusGlobalIndex(idx, instantFocus))
        return false;

    if (m_grid->currentPage() != oldPage || titleId != 0) {
        m_iconStreamer.onPageChanged(m_grid->currentPage(), m_grid->iconsPerPage(),
                                     app().gpu(), app().renderer(),
                                     m_grid->allIcons());
    }

    if (auto* cur = m_grid->focusManager().current())
        focusManager().setFocus(cur);
    updateCursor();
    return true;
}

void WiiUMenuApp::markSuspendedIcon(uint64_t titleId) {
    setSuspendedIconVisuals(titleId);
    if (titleId != 0)
        focusTitle(titleId);

    if (auto* cur = m_grid ? m_grid->focusManager().current() : nullptr) {
        auto* icon = static_cast<GlossyIcon*>(cur);
        m_titlePill->setText(pillTitle(icon));
    }
}

void WiiUMenuApp::closeActiveOverlays(bool closeFolders) {
    // Transfer input ownership before starting any exit animation. An overlay
    // that is still visually fading out must never win focusRoot().
    m_navigator.resetToHome();
    if (m_editMode)
        exitEditMode();
    if (m_userSelect && m_userSelect->isActive())
        m_userSelect->hide();
    if (m_contextMenu && m_contextMenu->isActive())
        m_contextMenu->hide();
    if (m_dialog && m_dialog->isActive())
        m_dialog->hide();
    if (m_quickSettings && m_quickSettings->isActive())
        m_quickSettings->hide();
    if (m_discordPanel && m_discordPanel->isActive())
        m_discordPanel->hide();
    if (m_notifPanel && m_notifPanel->isActive())
        m_notifPanel->hide();
    if (m_social && m_social->isActive())
        m_social->hide();
    if (m_textEntry && m_textEntry->isActive())
        m_textEntry->hide(false);
    if (m_settings && m_settings->isActive())
        m_settings->hide();
    if (m_themeShop && m_themeShop->isActive())
        m_themeShop->hide();
    if (m_gameOptions && m_gameOptions->isActive())
        m_gameOptions->hide();
    if (m_detail && m_detail->isActive())
        m_detail->hide();
    if (m_folderOptions && m_folderOptions->isActive())
        m_folderOptions->hide();
    if (m_controllerTest && m_controllerTest->isActive())
        m_controllerTest->hide();
    if (m_profileCarousel && m_profileCarousel->isActive())
        m_profileCarousel->hide();
    if (m_steamGridDbPicker && m_steamGridDbPicker->isActive())
        m_steamGridDbPicker->hide();
    // Returning from a suspended title must keep the open folder — closing then
    // focusTitle(game) reopens it and re-captures a dimmed frosted backdrop.
    if (closeFolders && m_openFolderId != 0)
        closeFolder();
}

nxui::Widget* WiiUMenuApp::focusRoot() {
    if (m_leaveCaptureDeferred) return nullptr;
    if (m_leaveCapturePending) return nullptr;
    if (leaveSplashActive()) return nullptr;
    if (m_launchAnim && m_launchAnim->isPlaying()) return nullptr;
    if (m_folderCaptureRequested || m_folderClosing) return nullptr;
    if (m_progressDialog && m_progressDialog->isActive()) return m_progressDialog.get();
    if (m_textEntry && m_textEntry->isActive()) return m_textEntry.get();
    if (m_dialog && m_dialog->isActive()) return m_dialog.get();
    if (m_contextMenu && m_contextMenu->isActive()) return m_contextMenu.get();
    if (m_quickSettings && m_quickSettings->isActive()) return m_quickSettings.get();
    if (m_discordPanel && m_discordPanel->isActive()) return m_discordPanel.get();
    if (m_notifPanel && m_notifPanel->isActive()) return m_notifPanel.get();
    if (m_social && m_social->isActive()) return m_social.get();
    if (m_userSelect && m_userSelect->isActive()) return m_userSelect.get();
    if (m_steamGridDbPicker && m_steamGridDbPicker->isActive())
        return m_steamGridDbPicker.get();
    switch (m_navigator.route()) {
        case switchu::navigation::Route::Settings:
            return m_settings ? m_settings.get() : &rootBox();
        case switchu::navigation::Route::ThemeShop:
            return m_themeShop ? m_themeShop.get() : &rootBox();
        case switchu::navigation::Route::GameOptions:
            return m_gameOptions ? m_gameOptions.get() : &rootBox();
        case switchu::navigation::Route::FolderOptions:
            return m_folderOptions ? m_folderOptions.get() : &rootBox();
        case switchu::navigation::Route::ControllerTest:
            return m_controllerTest ? m_controllerTest.get() : &rootBox();
        case switchu::navigation::Route::ProfileSelect:
            return m_profileCarousel ? m_profileCarousel.get() : &rootBox();
        case switchu::navigation::Route::AutoTheme:
            return m_autoThemeScreen ? m_autoThemeScreen.get() : &rootBox();
        case switchu::navigation::Route::ListDetail:
            return m_detail ? m_detail.get() : &rootBox();
        case switchu::navigation::Route::Home:
            break;
    }
    return &rootBox();
}

void WiiUMenuApp::toggleAccessibilitySpeech() {
    auto& i18n = nxui::I18n::instance();
    const bool enabled = !m_config.accessibilityEnabled;
    m_config.accessibilityEnabled = enabled;
    if (m_settings)
        m_settings->setAccessibilityEnabledState(enabled);
    if (m_themeShop)
        m_themeShop->setAccessibilityVoiceEnabled(enabled);
    if (m_gameOptions)
        m_gameOptions->setAccessibilityVoiceEnabled(enabled);
    if (m_folderOptions)
        m_folderOptions->setAccessibilityVoiceEnabled(enabled);

    if (enabled) {
        m_audio.playSfx(Sfx::ThemeToggle);
        m_accessibility.setEnabled(true);
        m_accessibility.announce(i18n.tr("accessibility.speech.enabled",
                                         "Voice guidance enabled."), true, true);
    } else {
        m_accessibility.announceAndDisable(i18n.tr("accessibility.speech.disabled",
                                                   "Voice guidance disabled."));
    }
    m_config.save();
}

bool WiiUMenuApp::handleAccessibilityToggleCombo() {
    if (m_navigator.route() == switchu::navigation::Route::ControllerTest)
        return false;
    auto& input = app().input();
    if (!input.isDown(nxui::Button::Plus) || !input.isDown(nxui::Button::Minus))
        return false;
    if (m_accessibilityToggleComboHeld)
        return true;
    m_accessibilityToggleComboHeld = true;
    m_plusExitPending = false;
    m_plusExitPendingTimer = 0.f;
    toggleAccessibilitySpeech();
    return true;
}

void WiiUMenuApp::wireGlobalActions() {
    auto& root = rootBox();

    root.addAction(static_cast<uint64_t>(nxui::Button::B), [this]() {
        if ((m_contextMenu && m_contextMenu->isActive()) ||
            (m_dialog && m_dialog->isActive()) ||
            (m_settings && m_settings->isActive()) ||
            (m_themeShop && m_themeShop->isActive()) ||
            (m_gameOptions && m_gameOptions->isActive())
        || (m_detail && m_detail->isActive()) ||
            (m_folderOptions && m_folderOptions->isActive()) ||
            (m_controllerTest && m_controllerTest->isActive()) ||
            (m_profileCarousel && m_profileCarousel->isActive()) ||
            (m_userSelect && m_userSelect->isActive()))
            return;
        if (m_openFolderId != 0 && !(m_dialog && m_dialog->isActive()))
            closeFolder(false, true);
    });

    // HomeNX: Quick Settings is on R.
    root.addAction(static_cast<uint64_t>(nxui::Button::R), [this]() {
        if (m_editMode || m_navigator.route() != switchu::navigation::Route::Home ||
            focusRoot() != &rootBox())
            return;
        openQuickSettings();
    });

    // HomeNX: Discord is on L (it slides in from the left), straight to the
    // chat of an alert that's on screen.
    root.addAction(static_cast<uint64_t>(nxui::Button::L), [this]() {
        if (m_editMode || m_navigator.route() != switchu::navigation::Route::Home ||
            focusRoot() != &rootBox())
            return;
        const bool toAlert = m_discordBanner && m_discordBanner->shown() && m_bannerIsDiscord;
        openDiscordPanel();
        if (toAlert && m_discordPanel && m_discordPanel->isOpen()) {
            m_discordPanel->openChat(m_shownAlert.channelId, m_shownAlert.guildId, m_shownAlert.chat,
                                     m_shownAlert.chatAvatar);
            m_discordBanner->slideOut();
        }
    });

    root.addAction(static_cast<uint64_t>(nxui::Button::LStick), [this]() {
        if (m_navigator.route() == switchu::navigation::Route::ControllerTest)
            return;
        m_accessibility.repeatLastAnnouncement();
    });

    // HomeNX: no sort-order button (it was on R, then L).

    // The grid holds the open folder's model, so paging works inside a folder too.
    root.addAction(static_cast<uint64_t>(nxui::Button::ZL), [this]() {
        if (m_navigator.route() != switchu::navigation::Route::Home || focusRoot() != &rootBox())
            return;
        flipPage(-1);
    });
    root.addAction(static_cast<uint64_t>(nxui::Button::ZR), [this]() {
        if (m_navigator.route() != switchu::navigation::Route::Home || focusRoot() != &rootBox())
            return;
        flipPage(+1);
    });
    root.addAction(static_cast<uint64_t>(nxui::Button::Y), [this]() {
        if ((m_contextMenu && m_contextMenu->isActive()) ||
            (m_dialog && m_dialog->isActive()) ||
            (m_themeShop && m_themeShop->isActive()) ||
            (m_settings && m_settings->isActive()) ||
            (m_gameOptions && m_gameOptions->isActive())
        || (m_detail && m_detail->isActive()) ||
            (m_folderOptions && m_folderOptions->isActive()) ||
            (m_controllerTest && m_controllerTest->isActive()) ||
            (m_profileCarousel && m_profileCarousel->isActive()) ||
            (m_userSelect && m_userSelect->isActive())) {
            return;
        }

        auto* cur = focusManager().current();
        if (m_editMode)
            return;
        if (!isEditableIcon(cur))
            return;

        enterEditMode();
        m_audio.playSfx(Sfx::Activate);
    });
#ifdef SWITCHU_DEBUG_UI
    root.addAction(static_cast<uint64_t>(nxui::Button::Minus), [this]() {
        if (m_navigator.route() == switchu::navigation::Route::ControllerTest)
            return;
        if (handleAccessibilityToggleCombo())
            return;
        m_showDebugOverlay = !m_showDebugOverlay;
        DebugLog::log("[debug] ImGui overlay toggled: %d", m_showDebugOverlay ? 1 : 0);
    });
#else
    root.addAction(static_cast<uint64_t>(nxui::Button::Minus), [this]() {
        if (m_navigator.route() == switchu::navigation::Route::ControllerTest)
            return;
        handleAccessibilityToggleCombo();
    });
#endif
#ifdef SWITCHU_HOMEBREW
    root.addAction(static_cast<uint64_t>(nxui::Button::Plus), [this]() {
        if (m_navigator.route() == switchu::navigation::Route::ControllerTest)
            return;
        if (handleAccessibilityToggleCombo())
            return;
        m_plusExitPending = true;
        m_plusExitPendingTimer = 0.80f;
    });
#endif

#ifdef SWITCHU_MENU
    root.addAction(static_cast<uint64_t>(nxui::Button::X), [this]() {
        if (m_editMode) return;
        if (m_launcher.suspendedTitleId() == 0) return;
        auto* cur = focusManager().current();
        if (!cur || cur->tag() != "glossy_icon") return;
        auto* icon = static_cast<GlossyIcon*>(cur);
        if (!m_launcher.isAppSuspended(icon->titleId())) return;

        m_audio.playSfx(Sfx::ModalShow);
        m_dialogReturnFocus = cur;
        auto& i18n = nxui::I18n::instance();
        m_dialog->show(
            i18n.tr("game.close_title", "Close game"),
            i18n.tr("game.close_prefix", "Close") + std::string(" ") + icon->title()
                + i18n.tr("game.close_suffix", "?\nUnsaved progress will be lost."),
            {
                {i18n.tr("button.cancel", "Cancel"), [this]() {}, true},
                {i18n.tr("button.close", "Close"),  [this]() {
                    m_launcher.terminateApplication();
                    // Keep the visible suspended state until the daemon emits
                    // ApplicationExited. Clearing it optimistically made a
                    // failed termination indistinguishable from success.
                }, true}
            },
            1,
            {}
        );
        focusManager().setFocus(m_dialog.get());
    });
#endif
}

#ifdef SWITCHU_MENU
void WiiUMenuApp::showGameContextMenu(GlossyIcon* icon) {
    if (!icon || !m_gameOptions || icon->titleId() == 0)
        return;

    const uint64_t titleId = icon->titleId();
    const std::string title = icon->title();
    auto& i18n = nxui::I18n::instance();
    switchu::control_cache::Meta meta{};
    const bool hasMeta = switchu::control_cache::readMeta(titleId, meta);
    const std::string version = hasMeta && meta.display_version[0] != '\0'
        ? std::string(meta.display_version)
        : i18n.tr("game.version_unknown", "Unknown");

    GameOptionsScreen::GameInfo game;
    game.titleId = titleId;
    game.name = title;
    game.version = version;
    game.publisher = hasMeta && meta.publisher[0] != '\0'
        ? std::string(meta.publisher)
        : i18n.tr("game.publisher_unknown", "Unknown publisher");
    game.icon = icon->texture();
    game.gameCard = icon->isGameCard();
    game.suspended = m_launcher.isAppSuspended(titleId);
    game.canMove = m_openFolderId == 0;
    game.canResize = m_openFolderId == 0;
    const auto currentSize = gameGridSize(titleId, AppLayoutMode::Grid);
    game.sizeIndex = currentSize == switchu::widgets::WidgetSize{2, 2}
        ? 2 : (currentSize == switchu::widgets::WidgetSize{2, 1} ? 1 : 0);
    {
        std::error_code ec;
        game.hasHeroArt = std::filesystem::exists(
            SteamGridDbManager::heroPath(titleId), ec);
        ec.clear();
        game.hasLogoArt = std::filesystem::exists(
            SteamGridDbManager::logoPath(titleId), ec);
        game.hasIconArt = SteamGridDbManager::hasIcon(titleId);
    }
    // HomeNX: the player's play record (one pdm query, a millisecond or two).
    AccountUid uid = activeProfileUid();
    if (!accountUidIsValid(&uid) && !m_profiles.empty()) uid = m_profiles.front().uid;
    if (accountUidIsValid(&uid)) {
        const std::vector<PlayRecord> played = loadMyPlayHistory(uid, {titleId});
        if (!played.empty()) {
            const PlayRecord& r = played.front();
            game.playTime = hoursText(r.minutes);
            game.timesPlayed = std::to_string(r.launches);
            if (r.lastPlayed > 0) game.lastPlayed = dateOf(r.lastPlayed) + "  (" + ago(r.lastPlayed) + ")";
            if (r.firstPlayed > 0) game.firstPlayed = dateOf(r.firstPlayed);
        }
    }
    m_gameOptions->setGame(game);
    m_gameOptions->onMove([this]() {
        if (m_gameOptions) m_gameOptions->hide();
        if (focusTitle(m_gameOptionsTitleId)) {
            if (auto* focused = m_grid->focusManager().current())
                focusManager().setFocus(focused);
        }
        enterEditMode();
        m_audio.playSfx(Sfx::Activate);
    });
    m_gameOptions->onResize([this, titleId](int sizeIndex) {
        const switchu::widgets::WidgetSize requested = sizeIndex == 2
            ? switchu::widgets::WidgetSize{2, 2}
            : (sizeIndex == 1 ? switchu::widgets::WidgetSize{2, 1}
                              : switchu::widgets::WidgetSize{1, 1});
        m_gameSizes[titleId] = requested;
        if (requested == switchu::widgets::WidgetSize{1, 1})
            m_gameArtwork.erase(titleId);
        normalizeWidgetPlacements();
        m_layoutDirty = true;
        saveMenuLayout();
        if (m_gameOptions) m_gameOptions->hide();
        m_navigator.resetToHome();
        if (m_openFolderId == 0)
            applyDisplayModel(buildRootFolderModel(), titleId, false);
    });
    m_gameOptions->onRename([this, titleId, title]() {
        auto& i18n = nxui::I18n::instance();
        const auto custom = m_customNames.find(titleId);
        requestTextEntry(i18n.tr("game.rename_title", "Rename"),
            i18n.tr("game.rename_guide", "Leave empty to use the game's own name"),
            custom != m_customNames.end() ? custom->second : title, 64, false,
            [this, titleId](const std::string& value) {
                if (value.empty()) m_customNames.erase(titleId);
                else m_customNames[titleId] = value;
                saveCustomNames();
                if (m_gameOptions) m_gameOptions->hide();
                m_refreshQueued = true; // reload the list so the new name shows everywhere
                m_deferredRefreshFrames = std::max(m_deferredRefreshFrames, 2);
            });
    });
    m_gameOptions->onCloseSoftware([this]() { m_launcher.terminateApplication(); });
    m_gameOptions->onDeleteSoftware([this, titleId, title]() {
        auto& localI18n = nxui::I18n::instance();
        m_dialogReturnFocus = m_gameOptions.get();
        m_dialog->show(
            localI18n.tr("game.uninstall_title", "Delete software"),
            localI18n.tr("game.delete_confirmation", "The software will be deleted. Save data will remain.")
                + std::string("\n") + title,
            {
                {localI18n.tr("button.cancel", "Cancel"), []() {}, true},
                {localI18n.tr("button.delete", "Delete"), [this, titleId, title]() {
                    startSoftwareDeletion(titleId, title, true);
                }, true},
            }, 0, {});
        focusManager().setFocus(m_dialog.get());
    });
    m_gameOptions->onSelectArtwork([this](GameOptionsScreen::ArtworkKind kind) {
        openSteamGridDbPicker(kind);
    });
    m_gameOptions->onClearArtwork([this, titleId](GameOptionsScreen::ArtworkKind kind) {
        clearSteamGridDbArtwork(titleId, kind);
    });
    m_audio.playSfx(Sfx::ModalShow);
    m_gameOptionsTitleId = titleId;
    m_navigator.navigate(switchu::navigation::Route::GameOptions);
    m_gameOptions->show();
    focusManager().setFocus(m_gameOptions.get());
}

void WiiUMenuApp::showFolderContextMenu(std::uint32_t folderId) {
    const auto* folder = m_folderStore.find(folderId);
    if (!folder || !m_folderOptions || !m_dialog) return;

    const std::string name = folder->name;
    FolderOptionsScreen::FolderInfo info;
    info.id = folder->id;
    info.name = folder->name;
    info.itemCount = static_cast<int>(folder->titleCount());
    info.colorIndex = folder->colorIndex;
    info.sizeIndex = folder->sizeIndex;
    info.styleIndex = m_config.folderStyle;
    info.showCover = m_config.folderShowCover;
    info.hasCustomIcon = SteamGridDbManager::hasIcon(folderTitleId(folderId));
    const std::uint64_t coverTitleId = info.hasCustomIcon
        ? folderTitleId(folderId)
        : switchu::folders::firstCoverTitleId(*folder);
    info.cover = folderCoverTexture(coverTitleId);
    m_folderOptions->setFolder(info);
    m_folderOptions->onOpen([this, folderId]() {
        if (m_folderOptions) m_folderOptions->hide();
        m_navigator.resetToHome();
        requestOpenFolder(folderId);
    });
    m_folderOptions->onRename([this, folderId]() {
        if (m_folderOptions) m_folderOptions->hide();
        m_navigator.resetToHome();
        renameFolder(folderId);
    });
    m_folderOptions->onColorChange([this, folderId](int colorIndex) {
        if (!m_folderStore.setColorIndex(folderId, colorIndex) ||
            !saveFoldersOrReport("folder_color"))
            return;
        const std::uint64_t id = folderTitleId(folderId);
        const int index = findTitleIndex(id);
        if (index >= 0 && index < m_model.count()) {
            m_model.at(index).folderColorIndex = colorIndex;
            const auto& icons = m_grid->allIcons();
            if (index < static_cast<int>(icons.size()) && icons[static_cast<std::size_t>(index)])
                icons[static_cast<std::size_t>(index)]->setFolderColorIndex(colorIndex);
        }
        if (m_openFolderId == folderId && m_pageIndicator)
            m_pageIndicator->setActiveColor(switchu::folders::colorForIndex(colorIndex));
    });
    m_folderOptions->onSizeChange([this, folderId](int sizeIndex) {
        if (!m_folderStore.setSizeIndex(folderId, sizeIndex))
            return;
        saveFoldersOrReport("folder_size");
    });
    m_folderOptions->onStyleChange([this](int styleIndex) {
        const int clamped = std::clamp(styleIndex, 0, switchu::folders::kFolderStyleCount - 1);
        if (m_config.folderStyle == clamped)
            return;
        m_config.folderStyle = clamped;
        if (m_configSaveFuture.valid())
            m_configSaveFuture.wait();
        m_configSaveFuture = m_threadPool.submit([config = m_config]() {
            config.save();
        });
        if (!m_grid)
            return;
        const auto& icons = m_grid->allIcons();
        for (int i = 0; i < m_model.count() && i < static_cast<int>(icons.size()); ++i) {
            if (!m_model.at(i).isFolder() || !icons[static_cast<std::size_t>(i)])
                continue;
            icons[static_cast<std::size_t>(i)]->setFolderStyleIndex(clamped);
        }
        applyFolderCoversToIcons();
    });
    m_folderOptions->onCoverChange([this](bool showCover) {
        if (m_config.folderShowCover == showCover)
            return;
        m_config.folderShowCover = showCover;
        if (m_configSaveFuture.valid())
            m_configSaveFuture.wait();
        m_configSaveFuture = m_threadPool.submit([config = m_config]() {
            config.save();
        });
        applyFolderCoversToIcons();
    });
    m_folderOptions->onCustomIconSelect([this, folderId, name]() {
        const std::uint64_t pseudo = folderTitleId(folderId);
        openSteamGridDbPickerForTitle(pseudo, name,
                                      GameOptionsScreen::ArtworkKind::Icon, name);
    });
    m_folderOptions->onCustomIconClear([this, folderId]() {
        const std::uint64_t pseudo = folderTitleId(folderId);
        clearSteamGridDbArtwork(pseudo, GameOptionsScreen::ArtworkKind::Icon);
        // Refresh folder options state + cover textures.
        if (m_folderOptions) {
            FolderOptionsScreen::FolderInfo info = {};
            if (const auto* folder = m_folderStore.find(folderId)) {
                info.id = folder->id;
                info.name = folder->name;
                info.itemCount = static_cast<int>(folder->titleCount());
                info.colorIndex = folder->colorIndex;
                info.sizeIndex = folder->sizeIndex;
                info.styleIndex = m_config.folderStyle;
                info.showCover = m_config.folderShowCover;
                info.hasCustomIcon = false;
                info.cover = folderCoverTexture(
                    switchu::folders::firstCoverTitleId(*folder));
                m_folderOptions->setFolder(info);
            }
        }
        if (m_openFolderId == 0)
            applyDisplayModel(buildRootFolderModel(), folderTitleId(folderId), false);
        else
            applyFolderCoversToIcons();
    });
    m_folderOptions->onDelete([this, folderId, name]() {
        auto& local = nxui::I18n::instance();
        m_dialogReturnFocus = m_folderOptions.get();
        m_dialog->show(local.tr("folder.delete", "Delete folder"),
                       local.tr("folder.delete_desc", "Games inside will return to the HOME menu."),
                       {
                           {local.tr("button.cancel", "Cancel"), {}, true},
                           {local.tr("button.delete", "Delete"), [this, folderId]() {
                               m_folderStore.remove(folderId);
                               if (!saveFoldersOrReport("delete")) return;
                               const auto pseudo = folderTitleId(folderId);
                               std::replace(m_layoutSlots.begin(), m_layoutSlots.end(), pseudo, std::uint64_t{0});
                               m_layoutDirty = true;
                               if (m_folderOptions) m_folderOptions->hide();
                               m_navigator.resetToHome();
                               applyDisplayModel(buildRootFolderModel(), 0, false);
                               m_audio.playSfx(Sfx::ConfirmPositive);
                           }, true}
                       }, 0, {});
        focusManager().setFocus(m_dialog.get());
    });

    m_audio.playSfx(Sfx::ModalShow);
    m_folderOptionsId = folderId;
    m_navigator.navigate(switchu::navigation::Route::FolderOptions);
    m_folderOptions->show();
    focusManager().setFocus(m_folderOptions.get());
}
#endif

void WiiUMenuApp::handleTouch() {
    constexpr float kSwipeThreshold = 80.f;
    constexpr float kLongPressThreshold = 0.55f;
    constexpr float kLongPressMoveThreshold = 18.f;

    auto& input = app().input();

    auto hitProfileLock = [this](float x, float y) {
        return m_profileLock && m_profileLock->isVisible() && m_profileLock->hitTest(x, y);
    };

    auto focusTouchedIcon = [this](int localHit) -> GlossyIcon* {
        if (!m_grid || localHit < 0)
            return nullptr;

        int global = m_grid->currentPage() * m_grid->iconsPerPage() + localHit;
        if (m_editMode)
            m_editTargetIndex = global;
        if (!m_grid->focusGlobalIndex(global))
            return nullptr;

        auto* cur = m_grid->focusManager().current();
        if (!cur)
            return nullptr;

        focusManager().setFocus(cur);
        if (m_cursor) {
            nxui::Rect cursorRect = cur->focusRect();
            if (m_editMode && m_editGhostIcon)
                cursorRect = m_grid->gridSpanRect(
                    global, m_editGhostIcon->gridSpanColumns(),
                    m_editGhostIcon->gridSpanRows());
            m_cursor->moveTo(cursorRect.expanded(4.f), 0.f);
            m_cursor->setVisible(true);
        } else {
            updateCursor();
        }

        if (!isEditableIcon(cur))
            return nullptr;
        return static_cast<GlossyIcon*>(cur);
    };

    if (input.touchDown()) {
        float tx = input.touchX();
        float ty = input.touchY();

        m_touchArrowLeft = m_touchArrowRight = false;
        m_touchBattery = false;
        m_touchBell = false;
        if (m_arrowAnimLeft.show > 0.5f && pageArrowRect(true).expanded(12.f).contains(tx, ty)) {
            m_touchArrowLeft = true;
            m_touchHitIndex = -1;
            return;
        }
        if (m_arrowAnimRight.show > 0.5f && pageArrowRect(false).expanded(12.f).contains(tx, ty)) {
            m_touchArrowRight = true;
            m_addPageTouchHold = m_addPageMode;
            m_touchHitIndex = -1;
            return;
        }

        // Whole battery pill opens Quick Settings (chevron = "more").
        if (m_battery && m_battery->isVisible() && m_battery->hitTest(tx, ty)
            && m_topHud && m_topHud->isVisible()) {
            m_touchBattery = true;
            m_touchHitIndex = -1;
            return;
        }
        if (m_bell && m_bell->isVisible() && m_bell->hitTest(tx, ty) && m_topHud && m_topHud->isVisible()) {
            m_touchBell = true;
            m_touchHitIndex = -1;
            return;
        }

        m_touchProfileLock = hitProfileLock(tx, ty);
        m_touchProfileLockWasFocused = m_touchProfileLock &&
            focusManager().current() == m_profileLock.get();
        if (m_touchProfileLock) {
            m_touchHitIndex = -1;
            m_touchOnFocused = false;
            m_touchEditDragActive = false;
            m_editGhostTouchFollow = false;
            return;
        }

        int hit = m_grid->hitTest(tx, ty);
        m_touchHitIndex = hit;
        m_touchOnFocused = false;
        m_touchEditDragActive = false;
        m_editGhostTouchFollow = false;
        if (hit >= 0) {
            auto icons = m_grid->pageIcons();
            if (hit < (int)icons.size())
                m_touchOnFocused = (icons[hit] == focusManager().current());
        }
    }

    if (input.isTouching() && m_addPageTouchHold &&
        !pageArrowRect(false).expanded(20.f).contains(input.touchX(), input.touchY())) {
        m_addPageTouchHold = false;
    }

    if (input.isTouching() && m_touchHitIndex >= 0) {
        float dx = input.touchDeltaX();
        float dy = input.touchDeltaY();

        if (!m_editMode
            && std::abs(dx) <= kLongPressMoveThreshold
            && std::abs(dy) <= kLongPressMoveThreshold
            && input.touchDuration() >= kLongPressThreshold)
        {
            if (auto* icon = focusTouchedIcon(m_touchHitIndex)) {
                enterEditMode();
                if (m_editMode) {
                    m_touchEditDragActive = true;
                    m_editGhostTouchFollow = true;
                    m_editGhostTouchPos = {input.touchX(), input.touchY()};
                    m_audio.playSfx(Sfx::Activate);
                    m_editGhostTargetRect = m_grid->gridSpanRect(
                        m_editTargetIndex,
                        m_editGhostIcon->gridSpanColumns(),
                        m_editGhostIcon->gridSpanRows());
                }
            }
        }

        if (m_editMode && m_touchEditDragActive) {
            // The dragged tile follows the finger; the slot under it is still
            // tracked so the drop target and cursor update as before.
            m_editGhostTouchFollow = true;
            m_editGhostTouchPos = {input.touchX(), input.touchY()};
            // While the grid swaps to the folder opened by the hover timer the
            // hits still belong to the root model, so skip slot tracking until
            // the folder model is live.
            int dragHit = m_editDragFolderOpenWait > 0.f
                ? -1 : m_grid->hitTest(input.touchX(), input.touchY());
            if (dragHit >= 0)
                focusTouchedIcon(dragHit);
            else
                updateCursor();
        }
    }

    if (input.touchUp()) {
        if (m_touchBattery) {
            m_touchBattery = false;
            float dx = input.touchDeltaX();
            float dy = input.touchDeltaY();
            if (std::abs(dx) < 20.f && std::abs(dy) < 20.f
                && m_battery && m_battery->isVisible()
                && m_battery->hitTest(input.touchX(), input.touchY())) {
                openQuickSettings();
            }
            return;
        }
        if (m_touchBell) {
            m_touchBell = false;
            if (std::abs(input.touchDeltaX()) < 20.f && std::abs(input.touchDeltaY()) < 20.f && m_bell
                && m_bell->hitTest(input.touchX(), input.touchY())) {
                focusManager().setFocus(m_bell.get());
                m_bell->activate();
            }
            return;
        }

        if (m_touchArrowLeft || m_touchArrowRight) {
            const bool left = m_touchArrowLeft;
            const bool wasAddHold = m_addPageTouchHold;
            m_touchArrowLeft = m_touchArrowRight = false;
            m_addPageTouchHold = false;
            if (!wasAddHold &&
                pageArrowRect(left).expanded(12.f).contains(input.touchX(), input.touchY()))
                flipPage(left ? -1 : +1);
            return;
        }

        if (m_touchProfileLock) {
            float dx = input.touchDeltaX();
            float dy = input.touchDeltaY();
            m_touchProfileLock = false;
            if (std::abs(dx) < 20.f && std::abs(dy) < 20.f &&
                hitProfileLock(input.touchX(), input.touchY()))
            {
                focusManager().setFocus(m_profileLock.get());
                if (!m_touchProfileLockWasFocused)
                    m_profileLock->activate();
            }
            m_touchProfileLockWasFocused = false;
            return;
        }

        if (m_editMode && m_touchEditDragActive) {
            const bool folderSwapPending = m_editDragFolderOpenWait > 0.f;
            bool changed = activateEditModeTarget();
            // A release with no resolvable target (the hover-opened folder is
            // still swapping in) must not leave move mode armed on a floating
            // ghost: a touch user has no way to grab it again. Put it back.
            if (!changed && folderSwapPending && m_editMode)
                exitEditMode();
            m_audio.playSfx(changed ? Sfx::ConfirmPositive : Sfx::ModalHide);
            m_touchHitIndex = -1;
            m_touchEditDragActive = false;
            m_editGhostTouchFollow = false;
            m_editDragEdgeDir = 0;
            m_editDragEdgeHold = 0.f;
            m_editDragFolderHold = 0.f;
            m_editDragFolderOpenWait = 0.f;
            return;
        }

        float dx = input.touchDeltaX();
        float dy = input.touchDeltaY();
        if (std::abs(dx) > kSwipeThreshold && std::abs(dx) > std::abs(dy) * 1.5f) {
            flipPage(dx < 0 ? 1 : -1);
        } else if (m_openFolderId != 0
                   && m_touchHitIndex < 0
                   && std::abs(dx) < 20.f && std::abs(dy) < 20.f
                   && m_grid
                   && m_grid->hitTest(input.touchX(), input.touchY()) < 0) {
            // Tap anywhere that isn't an icon (dimmed margins left/right/above/below,
            // and empty gaps) to leave - mirrors B, including edit-mode keep-move.
            closeFolder(m_editMode, true);
        }
        m_touchHitIndex = -1;
        m_touchEditDragActive = false;
    }
}

#ifdef SWITCHU_MENU
void WiiUMenuApp::handleSystemAction(SysAction a) {
    switch (a) {
        case SysAction::HomeDismiss: {
            // Physical HOME while already browsing the menu.
            DebugLog::log("[pump] HomeDismiss -> close folder/overlays");
            m_launcher.setAppHasForeground(false);
            const std::uint32_t folderBefore = m_openFolderId;
            closeActiveOverlays(true);
            if (folderBefore != 0) {
                // Land on the folder tile — never focusTitle(inner game) or the
                // folder reopens and the frosted dim stacks.
                focusTitle(folderTitleId(folderBefore));
            }
            showFocusedSteamGridDbArtwork(true);
            break;
        }
        case SysAction::HomeButton: {
            DebugLog::log("[pump] HomeButton -> UI update");
            m_launcher.setAppHasForeground(false);

            std::uint64_t returnFocusId = m_launcher.suspendedTitleId();
            if (returnFocusId == 0 && m_grid) {
                if (auto* focused = m_grid->focusManager().current();
                    focused && focused->tag() == "glossy_icon")
                    returnFocusId = static_cast<GlossyIcon*>(focused)->titleId();
            }
            refreshRecentActivityDuration();
            // HomeNX: no SD commits here (closing a game used to stall on three).
            if (!m_widgetStore.save(false))
                DebugLog::log("[widgets] recent activity duration could not be saved");
            // Keep an already-open folder — closing+refocus was reopening it and
            // stacking FolderBackdrop dims on every HOME-from-title return.
            closeActiveOverlays(false);
            resumeMenuMusicAfterReturn();
            const bool hasActivityWidget = std::any_of(
                m_widgetStore.all().begin(), m_widgetStore.all().end(),
                [](const switchu::widgets::Widget& widget) {
                    return widget.type == switchu::widgets::WidgetType::RecentlyPlayed
                        || widget.type == switchu::widgets::WidgetType::RecentPlaytime;
                });
            if (hasActivityWidget && m_openFolderId == 0) {
                applyDisplayModel(buildRootFolderModel(), returnFocusId, false);
            } else {
                markSuspendedIcon(m_launcher.suspendedTitleId());
                focusTitle(returnFocusId);
            }
            break;
        }
        default:
            break;
    }
}
#endif

void WiiUMenuApp::updateCursor() {
    // The Discord panel is a full-screen focus target: a ring around it turns
    // the whole screen blue.
    if ((m_contextMenu && m_contextMenu->isActive()) ||
        (m_discordPanel && m_discordPanel->isOpen()) ||
        (m_notifPanel && m_notifPanel->isOpen()) ||
        (m_social && m_social->isOpen())) {
        if (m_cursor) m_cursor->setVisible(false);
        return;
    }
    if ((m_grid && m_grid->isTransitioning()) ||
        (m_folderZoom && m_folderZoom->isPlaying())) {
        if (m_cursor) m_cursor->setVisible(false); // it would sit at the landing spot
        return;
    }
    if (m_navigator.route() != switchu::navigation::Route::Home ||
        (m_dialog && m_dialog->isActive()) ||
        (m_progressDialog && m_progressDialog->isActive()) ||
        (m_userSelect && m_userSelect->isActive()))
        return;

    auto* cur = focusManager().current();
    if (cur) {
        const bool morphing = m_grid && m_grid->isLayoutMorphing();
        const bool movingLineFocus = m_grid
                                  && (m_grid->isDynamicLine() || morphing)
                                  && cur->tag() == "glossy_icon";
        nxui::Rect fr = movingLineFocus
            ? m_grid->focusedDisplayRect()
            : cur->focusRect();
        if (m_editMode && m_editGhostIcon && m_grid) {
            fr = m_grid->gridSpanRect(m_editTargetIndex,
                                      m_editGhostIcon->gridSpanColumns(),
                                      m_editGhostIcon->gridSpanRows());
        }
        // The app carousel already owns the motion curve; attaching the ring
        // directly avoids a second easing curve that would visibly lag behind.
        const bool carouselScrolling = movingLineFocus
                                    && (morphing || m_grid->isDynamicLineScrolling());
        m_cursor->moveTo(fr.expanded(4.f), carouselScrolling ? 0.f : 0.2f);
        m_cursor->setVisible(true);
    } else {
        m_cursor->setVisible(false);
    }
}
