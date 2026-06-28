// MainWindow_Shortcuts.cpp — keyboard-shortcut system of MainWindow.
//
// Part of the #3351 monolith decomposition (Phase 1c). Holds:
//
//   • The shortcut-state definitions (s_keyboardShortcutsEnabled,
//     s_sliderShortcutLeaseActive) and their accessors — declared in
//     MainWindowShortcutState.h, owned here as of this phase.
//   • registerShortcutActions(): the full keyboard-shortcut action table.
//   • The slider shortcut lease (#745): begin/renew/release + the
//     MainWindow::eventFilter() that drives it (and app-quit interception).
//   • handleCwMomentaryShortcut() + the trivial keyPress/keyRelease
//     overrides.
//
// Pure code motion from MainWindow.cpp — same class, no header changes.

#include "MainWindow.h"

#include "MainWindowHelpers.h"
#include "AppletPanel.h"
#include "BandStackPanel.h"
#include "CwxPanel.h"
#include "DvkPanel.h"
#include "GuardedSlider.h"
#include "MeterSlider.h"
#include "PanLayoutDialog.h"
#include "PanadapterStack.h"
#include "RxApplet.h"
#include "SpectrumOverlayMenu.h"
#include "SpectrumWidget.h"
#include "TitleBar.h"
#include "VfoWidget.h"
#include "MainWindowShortcutState.h"
#include "core/AppSettings.h"
#include "core/CwTrace.h"
#include "core/KiwiSdrProtocol.h"
#include "core/LogManager.h"
#include "models/SliceModel.h"

#include <QAbstractSlider>
#include <QToolTip>
#include <QApplication>
#include <QApplicationStateChangeEvent>
#include <QComboBox>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QTextEdit>
#include <QTimer>

#include <algorithm>
#include <iterator>

namespace AetherSDR {

namespace {
// Slider keeps the keyboard-shortcut lease this long after the last
// interaction (#745); moved with its only users from MainWindow.cpp.
constexpr int kSliderShortcutLeaseMs = 2000;
} // namespace

// ─── Shortcut state (definitions — declared in MainWindowShortcutState.h) ───

bool s_keyboardShortcutsEnabled = false;
bool s_sliderShortcutLeaseActive = false;

bool textInputCaptured()
{
    auto* w = QApplication::focusWidget();
    if (!w) return false;
    return qobject_cast<QLineEdit*>(w) || qobject_cast<QTextEdit*>(w)
        || qobject_cast<QPlainTextEdit*>(w) || qobject_cast<QSpinBox*>(w)
        || qobject_cast<QComboBox*>(w);
}

bool shortcutInputCaptured()
{
    if (s_sliderShortcutLeaseActive)
        return true;
    return textInputCaptured();
}

bool shortcutGuard() {
    return s_keyboardShortcutsEnabled && !shortcutInputCaptured();
}

// True while the lease holder is actively being dragged with the mouse, so the
// lease must not time out mid-drag.  Covers both QAbstractSlider handles and the
// MeterSlider (TCI/DAX) custom fader.
bool leaseHolderBusy(QWidget* w) {
    if (auto* s = qobject_cast<QAbstractSlider*>(w)) return s->isSliderDown();
    if (auto* m = qobject_cast<AetherSDR::MeterSlider*>(w)) return m->isDragging();
    return false;
}



// ─── Key events ─────────────────────────────────────────────────────────────

void MainWindow::keyPressEvent(QKeyEvent* event)
{
    QMainWindow::keyPressEvent(event);
}

void MainWindow::keyReleaseEvent(QKeyEvent* event)
{
    QMainWindow::keyReleaseEvent(event);
}


bool MainWindow::handleCwMomentaryShortcut(QKeyEvent* keyEvent, QEvent::Type eventType)
{
    if (!keyEvent || keyEvent->isAutoRepeat())
        return false;
    if (eventType != QEvent::KeyPress && eventType != QEvent::KeyRelease)
        return false;

    enum class CwAction { None, StraightKey, LeftPaddle, RightPaddle };

    const QKeySequence seq = shortcutSequenceFromKeyEvent(keyEvent);
    const auto* action = m_shortcutManager.actionForKey(seq);

    CwAction cwAction = CwAction::None;
    if (action) {
        if (action->id == QLatin1String(kCwStraightKeyActionId))
            cwAction = CwAction::StraightKey;
        else if (action->id == QLatin1String(kCwLeftPaddleActionId))
            cwAction = CwAction::LeftPaddle;
        else if (action->id == QLatin1String(kCwRightPaddleActionId))
            cwAction = CwAction::RightPaddle;
    }

    // Modifier-tolerant release (Principle VI): a combo binding released
    // modifier-first delivers a KeyRelease whose `seq` no longer matches the
    // bound `modifiers|key`, so the un-key above would miss and TX would stay
    // keyed. On a release, if any CW momentary action is active whose bound
    // base key matches this key, release that one — fail safe to RX.
    if (cwAction == CwAction::None && eventType == QEvent::KeyRelease) {
        if (m_cwStraightKeyActive
            && keyEventMatchesActionBaseKey(kCwStraightKeyActionId, keyEvent))
            cwAction = CwAction::StraightKey;
        else if (m_cwLeftPaddleActive
            && keyEventMatchesActionBaseKey(kCwLeftPaddleActionId, keyEvent))
            cwAction = CwAction::LeftPaddle;
        else if (m_cwRightPaddleActive
            && keyEventMatchesActionBaseKey(kCwRightPaddleActionId, keyEvent))
            cwAction = CwAction::RightPaddle;
    }

    if (cwAction == CwAction::None)
        return false;

    const bool press = eventType == QEvent::KeyPress;
    const bool currentlyActive =
        cwAction == CwAction::StraightKey ? m_cwStraightKeyActive :
        cwAction == CwAction::LeftPaddle ? m_cwLeftPaddleActive :
                                           m_cwRightPaddleActive;

    if (press && (!m_keyboardShortcutsEnabled || textInputCaptured()))
        return false;
    if (!press && !currentlyActive)
        return m_keyboardShortcutsEnabled && !textInputCaptured();

    const quint64 sourceMs = cwTraceNowMs();
    const quint64 traceId = nextCwTraceId();
    const bool down = press;

    switch (cwAction) {
    case CwAction::StraightKey:
        setCwStraightKeyState(down, QStringLiteral("keyboard:cwkey"), traceId, sourceMs);
        break;
    case CwAction::LeftPaddle:
        setCwLeftPaddleState(down, QStringLiteral("keyboard:cwdit"), traceId, sourceMs);
        break;
    case CwAction::RightPaddle:
        setCwRightPaddleState(down, QStringLiteral("keyboard:cwdah"), traceId, sourceMs);
        break;
    case CwAction::None:
        break;
    }

    return true;
}


bool MainWindow::handlePttHoldShortcut(QKeyEvent* keyEvent, QEvent::Type eventType)
{
    // PTT (Hold) can't go through a QShortcut (no key-released signal), so it
    // is driven here from the app-level event filter. Resolve the bound key
    // through ShortcutManager — exactly like handleCwMomentaryShortcut — so a
    // reassigned PTT-hold key actually keys the radio instead of staying stuck
    // on the old Space default (#3879).
    if (!keyEvent || keyEvent->isAutoRepeat())
        return false;
    if (eventType != QEvent::KeyPress && eventType != QEvent::KeyRelease)
        return false;

    const QKeySequence seq = shortcutSequenceFromKeyEvent(keyEvent);
    const auto* action = m_shortcutManager.actionForKey(seq);
    bool isPttHold = action && action->id == QLatin1String(kPttHoldActionId);

    // Modifier-tolerant release (Principle VI): a combo binding like Ctrl+T
    // released modifier-first delivers the `T` KeyRelease after Ctrl is already
    // gone, so `seq` resolves to plain `T` and no longer matches `Ctrl+T` — the
    // un-key would never fire and TX would stay keyed. While PTT-hold is active,
    // also accept a release whose base key matches the bound key, ignoring
    // modifiers, so releasing any part of the combo fails safe to RX.
    if (!isPttHold && eventType == QEvent::KeyRelease && m_pttHoldActive)
        isPttHold = keyEventMatchesActionBaseKey(kPttHoldActionId, keyEvent);

    if (!isPttHold)
        return false;

    // Mirror the prior Space behavior: only key while connected and not typing
    // into a text field. When those gates fail, do not consume the key — let it
    // fall through (matching the old `&& m_radioModel.isConnected()` guard).
    if (textInputCaptured() || !m_radioModel.isConnected())
        return false;

    if (m_keyboardShortcutsEnabled) {
        // Route through the PTT coordinator (not the raw setTransmit() path) so
        // the Quindar intro/outro runs for keyboard PTT just like the GUI MOX
        // button. requestPttOn/Off still terminate in an `xmit` command, so the
        // interlock/gating in RadioModel's xmit handler is preserved; the
        // coordinator's preflight applies the same local interlock check.
        // (#3610)
        if (eventType == QEvent::KeyPress && !m_pttHoldActive) {
            m_pttHoldActive = true;
            m_radioModel.transmitModel().requestPttOn(
                TransmitModel::PttSource::Mox);
        } else if (eventType == QEvent::KeyRelease && m_pttHoldActive) {
            m_pttHoldActive = false;
            m_radioModel.transmitModel().requestPttOff(
                TransmitModel::PttSource::Mox);
        }
    }
    return true;  // consume the bound key so it can't also activate a button
}


bool MainWindow::keyEventMatchesActionBaseKey(const char* actionId,
                                              const QKeyEvent* ev)
{
    if (!ev)
        return false;
    const auto* a = m_shortcutManager.action(QLatin1String(actionId));
    if (!a || a->currentKey.isEmpty())
        return false;
    // currentKey[0] is the (modifiers | key) combination; compare the key half
    // only, so a combo binding matches on release regardless of which
    // modifiers are still down.
    return static_cast<int>(a->currentKey[0].key()) == ev->key();
}


void MainWindow::failSafeMomentaryKeyingToRx(const char* reason)
{
    // Principle VI fail-safe: a held momentary-keying key whose KeyRelease is
    // lost (focus left the window) would otherwise strand the transmitter. On
    // deactivation force the whole family back to RX. Cheap and safe to call on
    // every deactivation: bail out unless something is actually keyed.
    const bool anyActive = m_pttHoldActive || m_cwStraightKeyActive
        || m_cwLeftPaddleActive || m_cwRightPaddleActive;
    if (!anyActive)
        return;

    qCInfo(lcCw).noquote().nospace()
        << "Fail-safe to RX on " << (reason ? reason : "deactivate")
        << " — releasing held momentary keying (ptt=" << m_pttHoldActive
        << " straight=" << m_cwStraightKeyActive
        << " dit=" << m_cwLeftPaddleActive
        << " dah=" << m_cwRightPaddleActive << ")";

    if (m_pttHoldActive) {
        m_pttHoldActive = false;
        // Same un-key path as the normal KeyRelease branch, so the interlock
        // gating in RadioModel's xmit handler and the Quindar outro both run.
        m_radioModel.transmitModel().requestPttOff(TransmitModel::PttSource::Mox);
    }

    // setCw*State(false, …) clears its own flag and issues the un-key through
    // the existing sendCwKey / paddle path; each no-ops when already released,
    // so calling all three unconditionally is safe.
    const quint64 sourceMs = cwTraceNowMs();
    const quint64 traceId = nextCwTraceId();
    setCwStraightKeyState(false, QStringLiteral("failsafe:deactivate"), traceId, sourceMs);
    setCwLeftPaddleState(false, QStringLiteral("failsafe:deactivate"), traceId, sourceMs);
    setCwRightPaddleState(false, QStringLiteral("failsafe:deactivate"), traceId, sourceMs);
}


void MainWindow::beginSliderShortcutLease(QWidget* slider)
{
    if (!slider) return;

    m_sliderShortcutLease = slider;
    s_sliderShortcutLeaseActive = true;
    m_shortcutManager.setShortcutsEnabled(false);
    renewSliderShortcutLease();
}

void MainWindow::renewSliderShortcutLease()
{
    if (!m_sliderShortcutLease) {
        releaseSliderShortcutLease(false);
        return;
    }

    s_sliderShortcutLeaseActive = true;
    m_shortcutManager.setShortcutsEnabled(false);

    if (leaseHolderBusy(m_sliderShortcutLease.data())) {
        m_sliderShortcutLeaseTimer.stop();
        return;
    }

    m_sliderShortcutLeaseTimer.start(kSliderShortcutLeaseMs);
}

void MainWindow::releaseSliderShortcutLease(bool clearFocus)
{
    auto* slider = m_sliderShortcutLease.data();

    if (clearFocus && slider && leaseHolderBusy(slider)) {
        renewSliderShortcutLease();
        return;
    }

    m_sliderShortcutLeaseTimer.stop();
    m_sliderShortcutLease.clear();
    s_sliderShortcutLeaseActive = false;
    m_shortcutManager.setShortcutsEnabled(true);

    if (clearFocus && slider && QApplication::focusWidget() == slider)
        slider->clearFocus();
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == qApp && event->type() == QEvent::Quit) {
        if (!m_shuttingDown) {
            if (m_panStack) {
                m_panStack->setShuttingDown(true);
            }
            QTimer::singleShot(0, this, [this]() { close(); });
            return true;
        }
    }

    // Belt-and-suspenders for the deactivation fail-safe (#3888, Principle VI):
    // on macOS the per-window QEvent::ActivationChange can be unreliable when
    // the whole app is backgrounded, so also unkey any held momentary key when
    // the application leaves the active state. Do not consume the event.
    if (obj == qApp && event->type() == QEvent::ApplicationStateChange) {
        auto* stateEvent = static_cast<QApplicationStateChangeEvent*>(event);
        if (stateEvent->applicationState() != Qt::ApplicationActive)
            failSafeMomentaryKeyingToRx("app-deactivate");
    }

    if (auto* slider = qobject_cast<QAbstractSlider*>(obj)) {
        if (event->type() == QEvent::MouseButtonPress
            || event->type() == QEvent::MouseButtonDblClick) {
            beginSliderShortcutLease(slider);
        } else if (event->type() == QEvent::MouseButtonRelease
                   && m_sliderShortcutLease.data() == slider) {
            renewSliderShortcutLease();
        }
    } else if (auto* meter = qobject_cast<AetherSDR::MeterSlider*>(obj)) {
        // MeterSlider (TCI/DAX gain) gets the same lease so a mouse drag hands
        // off to keyboard nudges, then global shortcuts resume after a beat.
        if (event->type() == QEvent::MouseButtonPress
            || event->type() == QEvent::MouseButtonDblClick) {
            beginSliderShortcutLease(meter);
        } else if (event->type() == QEvent::MouseButtonRelease
                   && m_sliderShortcutLease.data() == meter) {
            renewSliderShortcutLease();
        }
    }

    // Applet-panel floating window — save geometry on move/resize, and
    // dock back on close so the menu action stays in sync.
    if (obj == m_appletPanelFloatWindow) {
        if (event->type() == QEvent::Move || event->type() == QEvent::Resize) {
            AppSettings::instance().setValue(
                "AppletPanelFloatGeometry",
                m_appletPanelFloatWindow->saveGeometry().toBase64());
        } else if (event->type() == QEvent::Close) {
            // Distinguish user-initiated close from app shutdown.
            // During shutdown, leave AppletPanelFloating=True so the
            // next launch re-opens the panel floating — the user
            // didn't "dock it back", the whole app is exiting.
            if (m_shuttingDown) {
                AppSettings::instance().setValue(
                    "AppletPanelFloatGeometry",
                    m_appletPanelFloatWindow->saveGeometry().toBase64());
                // Fall through — let Qt close the window normally.
            } else {
                // User clicked the X on the floating window — dock the
                // panel back and persist AppletPanelFloating=False.
                QTimer::singleShot(0, this, [this]() {
                    toggleAppletPanelFloating(false);
                });
            }
        }
    }

    // Space PTT: intercept at application level so it works regardless of
    // which widget has focus (buttons, combos, etc. won't steal Space).
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (m_swrSweep.running) {
            if (event->type() == QEvent::KeyPress
                && ke->key() == Qt::Key_Escape
                && !ke->isAutoRepeat()) {
                finishSwrSweep(true, QStringLiteral("SWR sweep stopped"));
            }
            return true;
        }

        if (handleCwMomentaryShortcut(ke, event->type()))
            return true;

        // PTT (Hold) — resolves its (rebindable) key through ShortcutManager
        // rather than a hardcoded Space, so reassigning it actually moves the
        // transmit key (#3879).
        if (handlePttHoldShortcut(ke, event->type()))
            return true;

        // MeterSlider (TCI/DAX gain) handles its own arrow stepping, badge,
        // and Enter-to-release inside keyPressEvent; the lease only frees the
        // arrows from the global tune/AF shortcuts.  Renew it on each step so
        // it doesn't expire mid-adjustment, then let the key fall through.
        if (event->type() == QEvent::KeyPress) {
            auto* meter = qobject_cast<AetherSDR::MeterSlider*>(QApplication::focusWidget());
            if (meter && m_sliderShortcutLease.data() == meter && s_sliderShortcutLeaseActive) {
                int k = ke->key();
                if (k == Qt::Key_Left || k == Qt::Key_Right
                    || k == Qt::Key_Up || k == Qt::Key_Down)
                    renewSliderShortcutLease();
            }
        }

        // A clicked slider gets a short keyboard lease so arrow nudges adjust
        // the slider, then global operating shortcuts automatically resume.
        if (event->type() == QEvent::KeyPress) {
            auto* slider = qobject_cast<QAbstractSlider*>(QApplication::focusWidget());
            if (slider && m_sliderShortcutLease.data() == slider && s_sliderShortcutLeaseActive) {
                int k = ke->key();
                // Enter hands keyboard control straight back to the panadapter's
                // global shortcuts, instead of waiting for the lease to time out.
                if (k == Qt::Key_Return || k == Qt::Key_Enter) {
                    releaseSliderShortcutLease(true);
                    return true;
                }
                // After a keyboard nudge, flash the same value badge the mouse
                // drag shows so keyboard and mouse give identical feedback.
                // GuardedSlider has no Q_OBJECT macro, so reach it via
                // dynamic_cast rather than qobject_cast.
                auto flashBadge = [slider]() {
                    if (auto* gs = dynamic_cast<GuardedSlider*>(slider))
                        gs->flashDragValue();
                };
                if (k == Qt::Key_Left || k == Qt::Key_Right
                    || k == Qt::Key_Up || k == Qt::Key_Down) {
                    bool increase = (k == Qt::Key_Right || k == Qt::Key_Up);
                    int step = (ke->modifiers() & Qt::ControlModifier)
                                   ? slider->pageStep() : slider->singleStep();
                    slider->setValue(slider->value() + (increase ? step : -step));
                    flashBadge();
                    renewSliderShortcutLease();
                    return true;
                }
                if (k == Qt::Key_PageUp || k == Qt::Key_PageDown) {
                    const int step = slider->pageStep();
                    slider->setValue(slider->value()
                                     + (k == Qt::Key_PageUp ? step : -step));
                    flashBadge();
                    renewSliderShortcutLease();
                    return true;
                }
                if (k == Qt::Key_Home || k == Qt::Key_End) {
                    slider->setValue(k == Qt::Key_Home
                                         ? slider->minimum()
                                         : slider->maximum());
                    flashBadge();
                    renewSliderShortcutLease();
                    return true;
                }
            }
        }
    }

    if (obj == m_paTempLabel && event->type() == QEvent::MouseButtonPress) {
        setPaTempDisplayUnit(!m_paTempUseFahrenheit);
        return true;
    }
    if (obj == m_networkLabel && event->type() == QEvent::MouseButtonDblClick) {
        showNetworkDiagnosticsDialog();
        return true;
    }
    if (obj == m_networkLabel && event->type() == QEvent::Enter) {
        m_networkTooltipRefreshTimer.start();
    }
    if (obj == m_networkLabel && event->type() == QEvent::Leave) {
        m_networkTooltipRefreshTimer.stop();
        QToolTip::hideText();
    }
    if (obj == m_networkLabel && event->type() == QEvent::ToolTip) {
        const QString tooltip = buildNetworkTooltip(m_radioModel);
        m_networkLabel->setToolTip(tooltip);
        auto* helpEvent = static_cast<QHelpEvent*>(event);
        QToolTip::showText(helpEvent->globalPos(), tooltip, m_networkLabel);
        m_networkTooltipRefreshTimer.start();
        return true;
    }
    if (obj == m_tnfIndicator && event->type() == QEvent::ToolTip) {
        const QString tooltip = buildTnfTooltip(m_radioModel.tnfModel());
        m_tnfIndicator->setToolTip(tooltip);
        auto* helpEvent = static_cast<QHelpEvent*>(event);
        QToolTip::showText(helpEvent->globalPos(), tooltip, m_tnfIndicator);
        return true;
    }
    if (obj == m_stationNickLabel && event->type() == QEvent::MouseButtonDblClick) {
        toggleConnectionDialog();
        return true;
    }
    if (obj == m_cwxIndicator && event->type() == QEvent::MouseButtonPress) {
        if (!m_cwxIndicator->isEnabled()) return true;
        bool show = !m_cwxPanel->isVisible();
        // Close DVK (mutual exclusion)
        if (show && m_dvkPanel->isVisible()) {
            m_dvkPanel->hide();
            auto* sl = activeSlice();
            updateKeyerAvailability(sl ? sl->mode() : QString());
        }
        m_cwxPanel->setVisible(show);
        m_cwxIndicator->setStyleSheet(show
            ? "QLabel { color: #00b4d8; font-weight: bold; font-size: 24px; }"
            : "QLabel { color: #404858; font-weight: bold; font-size: 24px; }");
        if (show) {
            auto sizes = m_splitter->sizes();
            if (sizes.size() >= 4) {
                int cwxW = 250;
                int total = sizes[0] + sizes[1] + sizes[2];
                sizes[0] = cwxW;
                sizes[1] = 0;
                sizes[2] = total - cwxW;
                m_splitter->setSizes(sizes);
            }
        }
        return true;
    }
    if (obj == m_dvkIndicator && event->type() == QEvent::MouseButtonPress) {
        if (!m_dvkIndicator->isEnabled()) return true;
        bool show = !m_dvkPanel->isVisible();
        // Close CWX (mutual exclusion)
        if (show && m_cwxPanel->isVisible()) {
            m_cwxPanel->hide();
            auto* sl = activeSlice();
            updateKeyerAvailability(sl ? sl->mode() : QString());
        }
        m_dvkPanel->setVisible(show);
        m_dvkIndicator->setStyleSheet(show
            ? "QLabel { color: #00b4d8; font-weight: bold; font-size: 24px; }"
            : "QLabel { color: #404858; font-weight: bold; font-size: 24px; }");
        if (show) {
            auto sizes = m_splitter->sizes();
            if (sizes.size() >= 4) {
                int dvkW = 250;
                int total = sizes[0] + sizes[1] + sizes[2];
                sizes[0] = 0;
                sizes[1] = dvkW;
                sizes[2] = total - dvkW;
                m_splitter->setSizes(sizes);
            }
        }
        return true;
    }
    if (obj == m_tnfIndicator && event->type() == QEvent::MouseButtonPress) {
        m_radioModel.tnfModel().requestGlobalTnfEnabled(!m_radioModel.tnfModel().globalEnabled());
        return true;
    }
    if (obj == m_fdxIndicator && event->type() == QEvent::MouseButtonPress) {
        bool on = !m_radioModel.fullDuplexEnabled();
        m_radioModel.sendCmdPublic(
            QString("radio set full_duplex_enabled=%1").arg(on ? 1 : 0),
            [this, on](int code, const QString& body) {
                if (code != 0) {
                    showPanadapterInterlockNotification(
                        QString("FDX not available: %1").arg(body.trimmed()));
                    return;
                }
                // Radio accepted; no status echo follows, so apply manually.
                m_radioModel.setFullDuplex(on);
            });
        return true;
    }
    if (obj == m_bandStackIndicator && event->type() == QEvent::MouseButtonPress) {
        bool show = !m_panStack->bandStackPanel()->isVisible();
        m_panStack->setBandStackVisible(show);
        updateBandStackIndicator();
        return true;
    }
    if (obj == m_tgxlContainer && event->type() == QEvent::MouseButtonPress) {
        auto& t = m_radioModel.tunerModel();
        // Cycle: OPERATE → BYPASS → STANDBY → OPERATE
        if (t.isOperate() && !t.isBypass())
            t.setBypass(true);
        else if (t.isOperate() && t.isBypass())
            t.setOperate(false);
        else {
            t.setBypass(false);
            t.setOperate(true);
        }
        return true;
    }
    if (obj == m_pgxlContainer && event->type() == QEvent::MouseButtonPress) {
        // Simple toggle: OPERATE ↔ STANDBY (PGXL has no BYPASS)
        m_radioModel.setAmpOperate(!m_radioModel.ampOperate());
        return true;
    }
    if (obj == m_txIndicator && event->type() == QEvent::MouseButtonPress) {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() == Qt::LeftButton)
            cancelTransmitFromIndicator();
        return true;
    }
    if (obj == m_addPanLabel && event->type() == QEvent::MouseButtonPress) {
        if (!m_radioModel.isConnected()) return true;
        int maxPans = m_radioModel.maxPanadapters();
        // Determine current layout from actual pan count, not saved setting
        int activePanCount = m_panStack ? m_panStack->count() : 1;
        QString currentLayout = "1";
        if (activePanCount >= 2)
            currentLayout = AppSettings::instance()
                .value("PanadapterLayout", "1").toString();
        PanLayoutDialog dlg(maxPans, currentLayout, this);
        if (dlg.exec() == QDialog::Accepted && !dlg.selectedLayout().isEmpty()) {
            const QString layoutId = dlg.selectedLayout();
            const int requestedPanCount = panCountForLayoutId(layoutId);
            const int currentSliceCount = static_cast<int>(m_radioModel.slices().size());
            if (requestedPanCount > activePanCount
                    && currentSliceCount >= m_radioModel.maxSlices()) {
                showPanadapterSliceCapacityMessage();
                return true;
            }
            m_suppressStartupPanLayoutRearrange = true;
            auto& s = AppSettings::instance();
            s.setValue("PanadapterLayout", layoutId);
            s.save();
            applyPanLayout(layoutId);
        }
        return true;
    }
    return QMainWindow::eventFilter(obj, event);
}


void MainWindow::registerShortcutActions()
{
    // Helper: nudge active slice frequency by N steps.
    // Share the incremental tune policy with wheel/knob/VFO tuning so pan
    // follow uses the same trigger + settle margins everywhere.
    auto nudgeFreq = [this](int steps) {
        if (!m_radioModel.isConnected()) return;
        auto* s = activeSlice();
        if (!s) return;
        if (s->isLocked()) {
            s->notifyTuneBlockedByLock();
            return;
        }
        int stepHz = spectrum() ? spectrum()->stepSize() : 100;
        double newMhz = s->frequency() + steps * stepHz / 1e6;
        applyTuneRequest(s, newMhz, TuneIntent::IncrementalTune, "keyboard-step");
    };

    // Step cycle helper
    auto cycleStep = [this](int dir) {
        auto* sw = spectrum();
        if (!sw) return;
        static const int steps[] = {10, 50, 100, 250, 500, 1000, 2500, 5000, 10000};
        int cur = sw->stepSize();
        if (dir > 0) {
            for (int i = 0; i < static_cast<int>(std::size(steps)); ++i)
                if (steps[i] > cur) { sw->setStepSize(steps[i]); return; }
        } else {
            for (int i = static_cast<int>(std::size(steps)) - 1; i >= 0; --i)
                if (steps[i] < cur) { sw->setStepSize(steps[i]); return; }
        }
    };

    auto stepActivePanRfGain = [this](int direction) {
        if (!m_radioModel.isConnected()) return;
        auto* pan = m_radioModel.activePanadapter();
        if (!pan) return;

        auto* sw = m_panStack ? m_panStack->spectrum(pan->panId()) : spectrum();
        if (!sw) sw = spectrum();

        const int step = std::max(1, pan->rfGainStep());
        const int current = sw ? sw->rfGainValue() : pan->rfGain();
        const int next = std::clamp(current + (direction * step),
                                    pan->rfGainLow(),
                                    pan->rfGainHigh());
        if (next == current) return;

        m_radioModel.setPanRfGain(next);
        if (!sw) return;

        sw->setRfGain(next);
        if (auto* menu = sw->overlayMenu())
            menu->setRfGain(next);

        auto& settings = AppSettings::instance();
        settings.setValue(sw->settingsKey("DisplayRfGain"), QString::number(next));
        settings.save();
    };

    // ── Frequency ───────────────────────────────────────────────────────
    // autoRepeat=true so holding the key continuously tunes (accessibility).
    m_shortcutManager.registerAction("tune_up_1", "Tune Up (1 step)", "Frequency",
        QKeySequence(Qt::Key_Right), [nudgeFreq]() { nudgeFreq(1); }, true);
    m_shortcutManager.registerAction("tune_down_1", "Tune Down (1 step)", "Frequency",
        QKeySequence(Qt::Key_Left), [nudgeFreq]() { nudgeFreq(-1); }, true);
    m_shortcutManager.registerAction("tune_up_10", "Tune Up (10 steps)", "Frequency",
        QKeySequence(Qt::SHIFT | Qt::Key_Right), [nudgeFreq]() { nudgeFreq(10); }, true);
    m_shortcutManager.registerAction("tune_down_10", "Tune Down (10 steps)", "Frequency",
        QKeySequence(Qt::SHIFT | Qt::Key_Left), [nudgeFreq]() { nudgeFreq(-10); }, true);
    m_shortcutManager.registerAction("tune_up_1mhz", "Tune Up 1 MHz", "Frequency",
        QKeySequence(), [nudgeFreq]() { nudgeFreq(10000); });
    m_shortcutManager.registerAction("tune_down_1mhz", "Tune Down 1 MHz", "Frequency",
        QKeySequence(), [nudgeFreq]() { nudgeFreq(-10000); });
    m_shortcutManager.registerAction("go_to_freq", "Go to Frequency", "Frequency",
        QKeySequence(Qt::Key_G), [this]() {
            auto* s = activeSlice();
            auto* sw = s ? spectrumForSlice(s) : nullptr;
            auto* vfo = (s && sw) ? sw->vfoWidget(s->sliceId()) : nullptr;
            if (!s || !vfo) return;
            QPointer<VfoWidget> vfoGuard = vfo;
            QTimer::singleShot(0, this, [vfoGuard]() {
                if (vfoGuard)
                    vfoGuard->beginDirectEntry("go-to-frequency");
            });
        });

    // ── Band ────────────────────────────────────────────────────────────
    struct BandDef { const char* id; const char* name; double mhz; };
    static const BandDef bands[] = {
        {"band_160m", "160m", 1.900}, {"band_80m", "80m", 3.800},
        {"band_60m", "60m", 5.357},   {"band_40m", "40m", 7.200},
        {"band_30m", "30m", 10.125},  {"band_20m", "20m", 14.225},
        {"band_17m", "17m", 18.118},  {"band_15m", "15m", 21.300},
        {"band_12m", "12m", 24.940},  {"band_10m", "10m", 28.400},
        {"band_6m",  "6m",  50.125},  {"band_2m",  "2m",  146.000},
    };
    for (const auto& b : bands) {
        double freq = b.mhz;
        m_shortcutManager.registerAction(b.id, b.name, "Band",
            QKeySequence(), [this, freq]() {
                if (!m_radioModel.isConnected()) return;
                auto* s = activeSlice();
                if (!s) return;
                if (s->isLocked()) {
                    s->notifyTuneBlockedByLock();
                    return;
                }
                TuneCenteringResult result;
                if (auto* pan = m_radioModel.panadapter(s->panId())) {
                    result.oldCenterMhz = pan->centerMhz();
                    result.bandwidthMhz = pan->bandwidthMhz();
                }
                result.newCenterMhz = freq;
                result.followRevealTriggered = true;
                result.hardCenterUsed = true;
                logTunePolicyDecision("band-shortcut", TuneIntent::AbsoluteJump,
                                      s->frequency(), freq, result);
                s->tuneAndRecenter(freq);
            });
    }

    // ── Mode ────────────────────────────────────────────────────────────
    static const char* modes[] = {"USB", "LSB", "CW", "CWL", "AM", "SAM", "FM", "NFM", "DFM", "DIGU", "DIGL", "RTTY"};
    for (const char* mode : modes) {
        QString m = mode;
        m_shortcutManager.registerAction(
            QString("mode_%1").arg(m.toLower()), m, "Mode",
            QKeySequence(), [this, m]() {
                if (!m_radioModel.isConnected()) return;
                auto* s = activeSlice();
                if (s) s->setMode(m);
            });
    }

    // ── TX ──────────────────────────────────────────────────────────────
    m_shortcutManager.registerAction("mox_toggle", "MOX Toggle", "TX",
        QKeySequence(Qt::Key_T), [this]() {
            if (!m_radioModel.isConnected()) return;
            // Route through the PTT coordinator so Quindar tones fire for the
            // keyboard MOX toggle, matching the GUI MOX button. (#3610)
            auto& tx = m_radioModel.transmitModel();
            if (tx.isTransmitting())
                tx.requestPttOff(TransmitModel::PttSource::Mox);
            else
                tx.requestPttOn(TransmitModel::PttSource::Mox);
        });
    // PTT (Hold) is handled by the app-level event filter (handlePttHoldShortcut)
    // because QShortcut has no "released" signal. Register with a null handler so
    // the keyboard map shows it as bound; the event filter looks the binding up
    // by kPttHoldActionId so a reassigned key takes effect (#3879).
    m_shortcutManager.registerAction(kPttHoldActionId, "PTT (Hold)", "TX",
        QKeySequence(Qt::Key_Space), nullptr);
    m_shortcutManager.registerAction("atu_start", "ATU Start", "TX",
        QKeySequence(), [this]() {
            if (!m_radioModel.isConnected()) return;
            m_radioModel.transmitModel().atuStart();
        });
    m_shortcutManager.registerAction("tune_toggle", "TUNE Toggle", "TX",
        QKeySequence(), [this]() {
            if (!m_radioModel.isConnected()) return;
            if (m_radioModel.transmitModel().isTuning())
                m_radioModel.transmitModel().stopTune();
            else
                m_radioModel.transmitModel().startTune();
        });
    m_shortcutManager.registerAction("two_tone_tune", "Two-Tone Tune", "TX",
        QKeySequence(), [this]() {
            if (!m_radioModel.isConnected()) return;
            m_radioModel.transmitModel().toggleTwoToneTune();
        });
    m_shortcutManager.registerAction("vox_toggle", "VOX Toggle", "TX",
        QKeySequence(), [this]() {
            if (!m_radioModel.isConnected()) return;
            auto& tx = m_radioModel.transmitModel();
            tx.setVoxEnable(!tx.voxEnable());
        });
    m_shortcutManager.registerAction("speech_proc_toggle", "Speech Processor Toggle", "TX",
        QKeySequence(), [this]() {
            if (!m_radioModel.isConnected()) return;
            auto& tx = m_radioModel.transmitModel();
            tx.setSpeechProcessorEnable(!tx.speechProcessorEnable());
        });
    m_shortcutManager.registerAction("dax_toggle", "DAX TX Toggle", "TX",
        QKeySequence(), [this]() {
            if (!m_radioModel.isConnected()) return;
            auto& tx = m_radioModel.transmitModel();
            tx.setDax(!tx.daxOn());
        });
    m_shortcutManager.registerAction("tx_monitor_toggle", "TX Monitor Toggle", "TX",
        QKeySequence(), [this]() {
            if (!m_radioModel.isConnected()) return;
            auto& tx = m_radioModel.transmitModel();
            tx.setSbMonitor(!tx.sbMonitor());
        });

    // ── Audio ───────────────────────────────────────────────────────────
    m_shortcutManager.registerAction("af_gain_up", "AF Gain Up", "Audio",
        QKeySequence(Qt::Key_Up), [this]() {
            auto* s = activeSlice();
            if (s) s->setAudioGain(std::min(100.0f, s->audioGain() + 5.0f));
        });
    m_shortcutManager.registerAction("af_gain_down", "AF Gain Down", "Audio",
        QKeySequence(Qt::Key_Down), [this]() {
            auto* s = activeSlice();
            if (s) s->setAudioGain(std::max(0.0f, s->audioGain() - 5.0f));
        });
    m_shortcutManager.registerAction("mute_toggle", "Mute Toggle", "Audio",
        QKeySequence(Qt::Key_M), [this]() {
            auto* s = activeSlice();
            if (s) s->setAudioMute(!s->audioMute());
        });
    m_shortcutManager.registerAction("mute_all_slices_toggle", "Mute All Slices", "Audio",
        QKeySequence(), [this]() { onMuteAllSlicesToggle(); });
    m_shortcutManager.registerAction("master_mute_toggle", "Master Mute Toggle", "Audio",
        QKeySequence(), [this]() {
            m_audio->setMuted(!m_audio->isMuted());
        });
    // Master volume up/down — mirrors the Aether Control "VolumeUp"/"VolumeDown"
    // dispatch (MainWindow_Controllers.cpp): clamp MasterVolume ±5 in [0,100],
    // update the title-bar slider, and push to the audio path. No default key
    // (Up/Down are taken by per-slice AF Gain); the user binds keys themselves.
    m_shortcutManager.registerAction("master_volume_up", "Master Volume Up", "Audio",
        QKeySequence(), [this]() {
            const int next = std::clamp(
                AppSettings::instance().value("MasterVolume", "100").toInt() + 5, 0, 100);
            if (m_titleBar) m_titleBar->setMasterVolume(next);
            applyMasterVolume(next);
        }, /*autoRepeat=*/true);
    m_shortcutManager.registerAction("master_volume_down", "Master Volume Down", "Audio",
        QKeySequence(), [this]() {
            const int next = std::clamp(
                AppSettings::instance().value("MasterVolume", "100").toInt() - 5, 0, 100);
            if (m_titleBar) m_titleBar->setMasterVolume(next);
            applyMasterVolume(next);
        }, /*autoRepeat=*/true);
    m_shortcutManager.registerAction("squelch_toggle", "Squelch Toggle", "Audio",
        QKeySequence(), [this]() {
            auto* s = activeSlice();
            if (s) {
                s->setSquelch(!s->receiveSquelchOn(),
                              s->receiveSquelchLevel());
            }
        });

    // ── Slice ───────────────────────────────────────────────────────────
    m_shortcutManager.registerAction("next_slice", "Next Slice", "Slice",
        QKeySequence(), [this]() {
            const auto& slices = m_radioModel.slices();
            if (slices.size() <= 1) return;
            int idx = 0;
            for (int i = 0; i < slices.size(); ++i)
                if (slices[i]->sliceId() == m_activeSliceId) { idx = i; break; }
            setActiveSlice(slices[(idx + 1) % slices.size()]->sliceId());
        });
    m_shortcutManager.registerAction("prev_slice", "Prev Slice", "Slice",
        QKeySequence(), [this]() {
            const auto& slices = m_radioModel.slices();
            if (slices.size() <= 1) return;
            int idx = 0;
            for (int i = 0; i < slices.size(); ++i)
                if (slices[i]->sliceId() == m_activeSliceId) { idx = i; break; }
            setActiveSlice(slices[(idx - 1 + slices.size()) % slices.size()]->sliceId());
        });
    m_shortcutManager.registerAction("split_toggle", "Split Toggle", "Slice",
        QKeySequence(), [this]() {
            if (!m_splitActive) {
                if (m_radioModel.slices().size() >= m_radioModel.maxSlices()) return;
                auto* s = activeSlice();
                if (!s) return;
                QString panId = s->panId();
                if (panId.isEmpty())
                    panId = m_panStack ? m_panStack->activePanId() : m_radioModel.panId();
                bool isCw = s->mode() == "CW" || s->mode() == "CWL";
                double txFreq = s->frequency() + (isCw ? 0.001 : 0.005);
                m_splitActive = true;
                m_splitRxSliceId = s->sliceId();
                m_radioModel.sendCommand(
                    QString("slice create pan=%1 freq=%2").arg(panId).arg(txFreq, 0, 'f', 6));
            } else {
                disableSplit();
            }
        });
    m_shortcutManager.registerAction("cycle_tx_slice", "Cycle TX Slice", "Slice",
        QKeySequence(), [this]() {
            const auto slices = m_radioModel.slices();
            if (slices.size() <= 1) return;
            int txIdx = 0;
            for (int i = 0; i < slices.size(); ++i) {
                if (slices[i]->isTxSlice()) { txIdx = i; break; }
            }
            slices[(txIdx + 1) % slices.size()]->setTxSlice(true);
        });

    // ── Filter ──────────────────────────────────────────────────────────
    // Step through the per-mode preset list via RxApplet so LSB/CWL/DIGL/RTTY
    // get the correct edge moved (issue #2208 — naive +/-100 Hz on the upper
    // edge collapsed the passband on lower-sideband modes).
    m_shortcutManager.registerAction("filter_widen", "Filter Widen", "Filter",
        QKeySequence(), [this]() {
            if (auto* rx = m_appletPanel->rxApplet()) rx->stepFilterWidth(+1);
        });
    m_shortcutManager.registerAction("filter_narrow", "Filter Narrow", "Filter",
        QKeySequence(), [this]() {
            if (auto* rx = m_appletPanel->rxApplet()) rx->stepFilterWidth(-1);
        });

    // ── Tuning ──────────────────────────────────────────────────────────
    m_shortcutManager.registerAction("step_up", "Step Size Up", "Tuning",
        QKeySequence(Qt::Key_BracketRight), [cycleStep]() { cycleStep(1); });
    m_shortcutManager.registerAction("step_down", "Step Size Down", "Tuning",
        QKeySequence(Qt::Key_BracketLeft), [cycleStep]() { cycleStep(-1); });
    m_shortcutManager.registerAction("lock_toggle", "Tune Lock Toggle", "Tuning",
        QKeySequence(Qt::Key_L), [this]() {
            auto* s = activeSlice();
            if (s) s->setLocked(!s->isLocked());
        });

    static constexpr double kPanZoomFactor = 1.5;
    auto zoomActivePanadapter = [this](double factor) {
        if (!m_radioModel.isConnected()) {
            return;
        }

        auto* s = activeSlice();
        if (!s || s->panId().isEmpty()) {
            return;
        }

        auto* sw = spectrumForSlice(s);
        if (!sw) {
            return;
        }

        const double currentBw = sw->bandwidthMhz();
        // Clamp to limits so the final keypress snaps to exact min/max (#1458).
        const double newBw = std::clamp(currentBw * factor,
                                        m_radioModel.minPanBandwidthMhz(),
                                        m_radioModel.maxPanBandwidthMhz());
        if (newBw == currentBw) return;  // already at the hard limit

        double newCenter = sw->centerMhz();

        // When zooming in, center on the active slice so repeated keypresses do
        // not push it toward the panadapter edge (#1932).
        if (factor < 1.0) {
            newCenter = s->frequency();
        }
        newCenter = std::max(newCenter, newBw / 2.0);

        sw->setFrequencyRange(newCenter, newBw);
        // Keep keyboard zoom on the same combined pan-range path as trackpad /
        // on-screen zoom so mode/frequency jumps do not reintroduce stale
        // center-versus-bandwidth transitions.
        applyPanRangeRequest(s->panId(), newCenter, newBw, "keyboard-pan-zoom");
    };

    // ── DSP ─────────────────────────────────────────────────────────────
    m_shortcutManager.registerAction("nb_toggle", "NB Toggle", "DSP",
        QKeySequence(), [this]() {
            auto* s = activeSlice();
            if (s) s->setNb(!s->nbOn());
        });
    m_shortcutManager.registerAction("nr2_toggle", "NR2 Toggle", "DSP",
        QKeySequence(), [this]() {
            if (m_audio->nr2Enabled()) {
                QMetaObject::invokeMethod(m_audio, [this]() {
                    m_audio->setNr2Enabled(false);
                });
            } else {
                enableNr2WithWisdom();
            }
        });
    m_shortcutManager.registerAction("rn2_toggle", "RN2 (RNNoise) Toggle", "DSP",
        QKeySequence(), [this]() {
            QMetaObject::invokeMethod(m_audio, [this]() {
                m_audio->setRn2Enabled(!m_audio->rn2Enabled());
            });
        });
    m_shortcutManager.registerAction("nr4_toggle", "NR4 Toggle", "DSP",
        QKeySequence(), [this]() {
            QMetaObject::invokeMethod(m_audio, [this]() {
                m_audio->setNr4Enabled(!m_audio->nr4Enabled());
            });
        });
    m_shortcutManager.registerAction("dfnr_toggle", "DFNR Toggle", "DSP",
        QKeySequence(), [this]() {
            QMetaObject::invokeMethod(m_audio, [this]() {
                m_audio->setDfnrEnabled(!m_audio->dfnrEnabled());
            });
        });
    m_shortcutManager.registerAction("tnf_toggle", "TNF Global Toggle", "DSP",
        QKeySequence(), [this]() {
            if (!m_radioModel.isConnected()) return;
            const bool wasOn = m_radioModel.tnfModel().globalEnabled();
            m_radioModel.sendCommand(
                QString("radio set tnf_enabled=%1").arg(wasOn ? 0 : 1));
        });
    m_shortcutManager.registerAction("nr_cycle", "NR Cycle (Off/NR/NR2/NR4/DFNR)", "DSP",
        QKeySequence(), [this]() {
            auto* s = activeSlice();
            if (!s) return;
            if (m_audio->dfnrEnabled()) {
                // DFNR → off
                QMetaObject::invokeMethod(m_audio, [this]() { m_audio->setDfnrEnabled(false); });
            } else if (m_audio->nr4Enabled()) {
                // NR4 → DFNR
                QMetaObject::invokeMethod(m_audio, [this]() { m_audio->setNr4Enabled(false); });
                QMetaObject::invokeMethod(m_audio, [this]() { m_audio->setDfnrEnabled(true); });
            } else if (m_audio->nr2Enabled()) {
                // NR2 → NR4
                QMetaObject::invokeMethod(m_audio, [this]() { m_audio->setNr2Enabled(false); });
                QMetaObject::invokeMethod(m_audio, [this]() { m_audio->setNr4Enabled(true); });
            } else if (s->nrOn()) {
                // NR → NR2
                s->setNr(false);
                enableNr2WithWisdom();
            } else {
                // off → NR
                s->setNr(true);
            }
        });
    m_shortcutManager.registerAction("anf_toggle", "ANF Toggle", "DSP",
        QKeySequence(), [this]() {
            auto* s = activeSlice();
            if (s) s->setAnf(!s->anfOn());
        });

    // ── AGC ─────────────────────────────────────────────────────────────
    m_shortcutManager.registerAction("agc_cycle", "AGC Mode Cycle", "AGC",
        QKeySequence(), [this]() {
            auto* s = activeSlice();
            if (!s) return;
            static const char* modes[] = {"off", "slow", "med", "fast"};
            QString cur = s->receiveAgcMode().toLower();
            int idx = 0;
            for (int i = 0; i < 4; ++i)
                if (cur == modes[i]) { idx = i; break; }
            s->setAgcMode(modes[(idx + 1) % 4]);
        });
    m_shortcutManager.registerAction("rf_gain_up", "RF Gain Up", "AGC",
        QKeySequence(), [stepActivePanRfGain]() {
            stepActivePanRfGain(1);
        }, true);
    m_shortcutManager.registerAction("rf_gain_down", "RF Gain Down", "AGC",
        QKeySequence(), [stepActivePanRfGain]() {
            stepActivePanRfGain(-1);
        }, true);
    m_shortcutManager.registerAction("agct_up", "AGC-T Up", "AGC",
        QKeySequence(), [this]() {
            auto* s = activeSlice();
            if (s) {
                const bool external = s->externalReceiveReplacementActive();
                const int current = external ? s->receiveAgcThreshold()
                                             : s->agcThreshold();
                s->setAgcThreshold(std::min(
                    external ? KiwiSdrProtocol::kAgcThresholdMaxDb : 100,
                    current + 5));
            }
        }, true);
    m_shortcutManager.registerAction("agct_down", "AGC-T Down", "AGC",
        QKeySequence(), [this]() {
            auto* s = activeSlice();
            if (s) {
                const bool external = s->externalReceiveReplacementActive();
                const int current = external ? s->receiveAgcThreshold()
                                             : s->agcThreshold();
                s->setAgcThreshold(std::max(
                    external ? KiwiSdrProtocol::kAgcThresholdMinDb : 0,
                    current - 5));
            }
        }, true);

    // ── CW ──────────────────────────────────────────────────────────────
    m_shortcutManager.registerAction("cw_speed_up", "CW Speed Up (+5 WPM)", "CW",
        QKeySequence(), [this]() {
            if (!m_radioModel.isConnected()) return;
            auto& tx = m_radioModel.transmitModel();
            tx.setCwSpeed(std::min(100, tx.cwSpeed() + 5));
        });
    m_shortcutManager.registerAction("cw_speed_down", "CW Speed Down (-5 WPM)", "CW",
        QKeySequence(), [this]() {
            if (!m_radioModel.isConnected()) return;
            auto& tx = m_radioModel.transmitModel();
            tx.setCwSpeed(std::max(5, tx.cwSpeed() - 5));
        });
    m_shortcutManager.registerAction("cw_sidetone_toggle", "CW Sidetone Toggle", "CW",
        QKeySequence(), [this]() {
            if (!m_radioModel.isConnected()) return;
            auto& tx = m_radioModel.transmitModel();
            tx.setCwSidetone(!tx.cwSidetone());
        });
    m_shortcutManager.registerAction("cw_iambic_toggle", "CW Iambic Toggle", "CW",
        QKeySequence(), [this]() {
            if (!m_radioModel.isConnected()) return;
            auto& tx = m_radioModel.transmitModel();
            tx.setCwIambic(!tx.cwIambic());
        });
    m_shortcutManager.registerAction("cw_iambic_mode_toggle", "CW Iambic Mode Toggle (A/B)", "CW",
        QKeySequence(), [this]() {
            if (!m_radioModel.isConnected()) return;
            auto& tx = m_radioModel.transmitModel();
            tx.setCwIambicMode(tx.cwIambicMode() == 0 ? 1 : 0);
        });
    m_shortcutManager.registerAction("cw_swap_paddles_toggle", "CW Swap Paddles Toggle", "CW",
        QKeySequence(), [this]() {
            if (!m_radioModel.isConnected()) return;
            auto& tx = m_radioModel.transmitModel();
            tx.setCwSwapPaddles(!tx.cwSwapPaddles());
        });
    m_shortcutManager.registerAction("cwl_toggle", "CWL Frequency Offset Toggle", "CW",
        QKeySequence(), [this]() {
            if (!m_radioModel.isConnected()) return;
            auto& tx = m_radioModel.transmitModel();
            tx.setCwlEnabled(!tx.cwlEnabled());
        });
    m_shortcutManager.registerAction("cw_breakin_toggle", "CW Break-In (QSK) Toggle", "CW",
        QKeySequence(), [this]() {
            if (!m_radioModel.isConnected()) return;
            auto& tx = m_radioModel.transmitModel();
            tx.setCwBreakIn(!tx.cwBreakIn());
        });
    // Momentary CW actions are handled by the app-level event filter so
    // key release edges reach the netCW path too.
    m_shortcutManager.registerAction(kCwStraightKeyActionId, kCwStraightKeyActionName, "CW",
        QKeySequence(), nullptr);
    m_shortcutManager.registerAction(kCwLeftPaddleActionId, kCwLeftPaddleActionName, "CW",
        QKeySequence(), nullptr);
    m_shortcutManager.registerAction(kCwRightPaddleActionId, kCwRightPaddleActionName, "CW",
        QKeySequence(), nullptr);

    // ── EQ ──────────────────────────────────────────────────────────────
    m_shortcutManager.registerAction("tx_eq_toggle", "TX EQ Toggle", "EQ",
        QKeySequence(), [this]() {
            if (!m_radioModel.isConnected()) return;
            auto& eq = m_radioModel.equalizerModel();
            eq.setTxEnabled(!eq.txEnabled());
        });
    m_shortcutManager.registerAction("rx_eq_toggle", "RX EQ Toggle", "EQ",
        QKeySequence(), [this]() {
            if (!m_radioModel.isConnected()) return;
            auto& eq = m_radioModel.equalizerModel();
            eq.setRxEnabled(!eq.rxEnabled());
        });

    // ── Display ─────────────────────────────────────────────────────────
    m_shortcutManager.registerAction("band_zoom", "Band Zoom", "Display",
        QKeySequence(), [this]() {
            if (!m_radioModel.isConnected()) return;
            auto* s = activeSlice();
            if (s) m_radioModel.sendCommand(
                QString("slice set %1 band_zoom=1").arg(s->sliceId()));
        });
    m_shortcutManager.registerAction("segment_zoom", "Segment Zoom", "Display",
        QKeySequence(), [this]() {
            if (!m_radioModel.isConnected()) return;
            auto* s = activeSlice();
            if (s) m_radioModel.sendCommand(
                QString("slice set %1 segment_zoom=1").arg(s->sliceId()));
        });
    m_shortcutManager.registerAction("pan_zoom_in", "Panadapter Zoom In", "Display",
        QKeySequence(Qt::Key_Equal), [zoomActivePanadapter]() { zoomActivePanadapter(1.0 / kPanZoomFactor); });
    m_shortcutManager.registerAction("pan_zoom_out", "Panadapter Zoom Out", "Display",
        QKeySequence(Qt::Key_Minus), [zoomActivePanadapter]() { zoomActivePanadapter(kPanZoomFactor); });
    m_shortcutManager.registerAction("open_memories", "Open Memories Dialog", "Display",
        QKeySequence(Qt::Key_Slash), [this]() { showMemoryDialog(); });

    // ── RIT/XIT ─────────────────────────────────────────────────────────
    m_shortcutManager.registerAction("rit_toggle", "RIT Toggle", "RIT/XIT",
        QKeySequence(), [this]() {
            auto* s = activeSlice();
            if (s) s->setRit(!s->ritOn(), s->ritFreq());
        });
    m_shortcutManager.registerAction("xit_toggle", "XIT Toggle", "RIT/XIT",
        QKeySequence(), [this]() {
            auto* s = activeSlice();
            if (s) s->setXit(!s->xitOn(), s->xitFreq());
        });

    // ── Load user bindings and create QShortcuts ────────────────────────
    m_shortcutManager.loadBindings();
    s_keyboardShortcutsEnabled = m_keyboardShortcutsEnabled;
    m_shortcutManager.rebuildShortcuts(this, shortcutGuard);

    m_sliderShortcutLeaseTimer.setSingleShot(true);
    connect(&m_sliderShortcutLeaseTimer, &QTimer::timeout, this,
            [this]() { releaseSliderShortcutLease(true); });

    // Temporarily yield global shortcuts while a clicked slider is being
    // nudged, then return keyboard control to the operator shortcuts.
    connect(qApp, &QApplication::focusChanged, this,
            [this](QWidget* /*old*/, QWidget* now) {
        if (auto* slider = qobject_cast<QAbstractSlider*>(now))
            beginSliderShortcutLease(slider);
        else if (auto* meter = qobject_cast<AetherSDR::MeterSlider*>(now))
            beginSliderShortcutLease(meter);
        else
            releaseSliderShortcutLease(false);
    });
}


} // namespace AetherSDR
